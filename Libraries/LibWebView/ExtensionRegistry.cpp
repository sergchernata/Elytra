/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/Hex.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <LibCore/DirIterator.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Cookie/IncludeCredentials.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebExtensions/Crx.h>
#include <LibWebExtensions/ExtensionId.h>
#include <LibWebExtensions/Messages.h>
#include <LibWebExtensions/MatchPattern.h>
#include <LibWebExtensions/Zip.h>
#include <LibWebView/Application.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/Utilities.h>

#if defined(AK_OS_MACOS)
#    include <sys/param.h>
#endif
#if !defined(AK_OS_WINDOWS)
#    include <dirent.h>
#    include <fcntl.h>
#    include <sys/stat.h>
#endif

namespace WebView {

// Opens a file real_path() resolved to below the root. A link put in place of a component since then mustn't lead the
// reading out of the folder: the last component isn't followed, and the file opened must still be below the root.
static ErrorOr<int> open_inside(ByteString const& root, ByteString const& real_path)
{
    if (!real_path.starts_with(ByteString::formatted("{}/", root)))
        return Error::from_errno(EACCES);
#if defined(AK_OS_WINDOWS)
    return Core::System::open(real_path, O_RDONLY);
#else
    auto fd = TRY(Core::System::open(real_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    auto close_fd = ArmedScopeGuard([fd] { (void)Core::System::close(fd); });
#    if defined(AK_OS_MACOS)
    char opened_path[MAXPATHLEN] {};
    if (::fcntl(fd, F_GETPATH, opened_path) < 0)
        return Error::from_syscall("fcntl"sv, errno);
    auto opened = ByteString { opened_path, strnlen(opened_path, sizeof(opened_path)) };
#    else
    auto opened = TRY(Core::System::readlink(ByteString::formatted("/proc/self/fd/{}", fd)));
#    endif
    if (!opened.starts_with(ByteString::formatted("{}/", root)))
        return Error::from_errno(EACCES);
    auto stat = TRY(Core::System::fstat(fd));
    if (!S_ISREG(stat.st_mode))
        return Error::from_errno(EACCES);
    // Readers block.
    auto flags = TRY(Core::System::fcntl(fd, F_GETFL));
    TRY(Core::System::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK));
    close_fd.disarm();
    return fd;
#endif
}

Extension::Extension(String id, ByteString root, WebExtensions::Manifest manifest, WebExtensions::MessageCatalog messages)
    : m_id(move(id))
    , m_root(move(root))
    , m_manifest(move(manifest))
    , m_messages(move(messages))
{
}

Optional<String> Extension::read_file(StringView path) const
{
    auto contents = read_file_bytes(path);
    if (!contents.has_value())
        return {};
    auto string = String::from_utf8(*contents);
    if (string.is_error())
        return {};
    return string.release_value();
}

Optional<ByteBuffer> Extension::read_file_bytes(StringView path, size_t maximum_size) const
{
    auto relative_path = path.trim("/"sv, TrimMode::Left);
    if (relative_path.is_empty() || relative_path.contains('\\') || any_of(relative_path.split_view('/'), [](auto part) { return part == ".."sv; }))
        return {};
    auto real_path = FileSystem::real_path(ByteString::formatted("{}/{}", m_root, relative_path));
    if (real_path.is_error())
        return {};
    auto fd = open_inside(m_root, real_path.value());
    if (fd.is_error())
        return {};
    auto file = Core::File::adopt_fd(fd.value(), Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto stat = Core::System::fstat(file.value()->fd());
    if (stat.is_error() || !S_ISREG(stat.value().st_mode) || static_cast<size_t>(stat.value().st_size) > maximum_size)
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    return contents.release_value();
}

URL::Origin Extension::origin() const
{
    return URL::Parser::basic_parse(MUST(String::formatted("chrome-extension://{}/", m_id)))->origin();
}

// Chrome's (csp_info.cc), as LibWeb's Extensions::content_security_policies_for() gives them to renderers.
Vector<String> Extension::content_security_policies() const
{
    auto declared = m_manifest.content_security_policy.extension_pages.value_or("script-src 'self';"_string);
    return { move(declared), "script-src 'self' 'wasm-unsafe-eval' 'inline-speculation-rules'; object-src 'self';"_string };
}

// Chrome matches each page as a URL pattern's path, in which * is a wildcard (sandboxed_page_info.cc).
bool Extension::is_sandboxed_page(StringView path) const
{
    auto relative_path = path.trim("/"sv, TrimMode::Left);
    return any_of(m_manifest.sandbox_pages, [&](auto const& page) {
        return relative_path.matches(page.bytes_as_string_view().trim("/"sv, TrimMode::Left), CaseSensitivity::CaseSensitive);
    });
}

Optional<URL::URL> Extension::background_page_url() const
{
    if (!background_page_html().has_value())
        return {};
    if (auto const& background = m_manifest.background; background.has_value() && background->page.has_value())
        return URL::Parser::basic_parse(MUST(String::formatted("chrome-extension://{}/{}", m_id, background->page->bytes_as_string_view().trim("/"sv, TrimMode::Left))));
    return URL::Parser::basic_parse(MUST(String::formatted("chrome-extension://{}{}", m_id, background_page_path)));
}

static String script_tag(StringView path, bool is_module)
{
    // Paths come from the manifest: keep them to what can't escape the attribute.
    StringBuilder escaped;
    for (auto byte : path.trim("/"sv, TrimMode::Left).bytes()) {
        if (byte == '"' || byte == '<' || byte == '>' || byte == '&')
            escaped.appendff("%{:02X}", byte);
        else
            escaped.append(byte);
    }
    return MUST(String::formatted("<script{} src=\"/{}\"></script>\n", is_module ? " type=\"module\""sv : ""sv, escaped.string_view()));
}

Optional<String> Extension::background_page_html() const
{
    auto const& background = m_manifest.background;
    if (!background.has_value())
        return {};
    StringBuilder html;
    html.append("<!doctype html>\n<meta charset=\"utf-8\">\n"sv);
    if (background->service_worker.has_value()) {
        html.append(script_tag(*background->service_worker, background->type == WebExtensions::BackgroundInfo::Type::Module));
    } else if (!background->scripts.is_empty()) {
        for (auto const& script : background->scripts)
            html.append(script_tag(script, false));
    } else if (!background->page.has_value()) {
        return {};
    } else {
        return String {};
    }
    return html.to_string_without_validation();
}

// The URL the "matches" of web_accessible_resources are compared with. Local files have opaque origins here; Chrome
// matches them as file:///.
static Optional<URL::URL> url_for_origin(URL::Origin const& origin)
{
    if (origin.is_opaque()) {
        if (origin.is_file_origin())
            return URL::Parser::basic_parse("file:///"sv);
        return {};
    }
    return URL::Parser::basic_parse(MUST(String::formatted("{}/", origin.serialize())));
}

bool Extension::may_be_loaded_by(StringView path, Optional<URL::Origin> const& requester) const
{
    // No requester: the browser itself, opening one of the extension's pages.
    // FIXME: Once extension pages get processes of their own, also refuse a renderer that hosts none of this extension's.
    if (!requester.has_value() || requester->is_same_origin(origin()))
        return true;

    // Resources are named relative to the root.
    auto relative_path = path.trim("/"sv, TrimMode::Left);

    for (auto const& entry : m_manifest.web_accessible_resources) {
        // FIXME: Serve these at Chrome's dynamic URL, a random ID of the session's (runtime.getURL() gives it). Until
        //        then, they are only the extension's: others mustn't reach them at its static ID.
        if (entry.use_dynamic_url)
            continue;
        bool listed = any_of(entry.resources, [&](auto const& resource) {
            return WebExtensions::glob_matches(resource.bytes_as_string_view().trim("/"sv, TrimMode::Left), relative_path);
        });
        if (!listed)
            continue;

        // Other extensions, by ID.
        if (!requester->is_opaque() && requester->scheme() == "chrome-extension"sv) {
            auto requester_id = requester->host().serialize();
            if (any_of(entry.extension_ids, [&](auto const& id) { return id == "*"sv || id == requester_id; }))
                return true;
            continue;
        }

        auto requester_url = url_for_origin(*requester);
        for (auto const& match : entry.matches) {
            auto pattern = WebExtensions::MatchPattern::parse(match);
            if (pattern.is_error())
                continue;
            if (requester_url.has_value() ? pattern.value().matches_ignoring_path(*requester_url) : pattern.value().matches_all_urls())
                return true;
        }
    }
    return false;
}

// Whether the path below the root, when it exists, is in the root: no link may lead the reading out of the folder.
static ErrorOr<void> ensure_inside(ByteString const& root, StringView relative_path)
{
    auto path = ByteString::formatted("{}/{}", root, relative_path);
    if (!FileSystem::exists(path) && !FileSystem::is_link(path))
        return {};
    auto real_path = TRY(FileSystem::real_path(path));
    if (!real_path.starts_with(ByteString::formatted("{}/", root)))
        return Error::from_string_literal("Extension files must be in the extension's folder");
    return {};
}

ErrorOr<NonnullOwnPtr<Extension>> ExtensionRegistry::read_extension(ByteString const& directory, Optional<String> const& id) const
{
    auto root = TRY(FileSystem::real_path(directory));
    if (!FileSystem::is_directory(root))
        return Error::from_errno(ENOTDIR);

    // What's read here besides the files served later (which open_file() keeps in the folder): the manifest and the
    // message catalogs.
    TRY(ensure_inside(root, "manifest.json"sv));
    TRY(ensure_inside(root, "_locales"sv));
    if (FileSystem::is_directory(ByteString::formatted("{}/_locales", root))) {
        Core::DirIterator locales(ByteString::formatted("{}/_locales", root), Core::DirIterator::SkipParentAndBaseDir);
        while (locales.has_next()) {
            auto locale = ByteString::formatted("_locales/{}", locales.next_path());
            TRY(ensure_inside(root, locale));
            TRY(ensure_inside(root, ByteString::formatted("{}/messages.json", locale)));
        }
    }

    auto manifest_file = TRY(Core::File::open(ByteString::formatted("{}/manifest.json", root), Core::File::OpenMode::Read));
    auto manifest_json = TRY(manifest_file->read_until_eof());
    auto manifest = TRY(WebExtensions::Manifest::parse(manifest_json));

    String extension_id;
    if (id.has_value())
        extension_id = *id;
    else if (manifest.key.has_value())
        extension_id = TRY(WebExtensions::extension_id_from_base64_key(*manifest.key));
    else
        extension_id = WebExtensions::extension_id_from_path(root);

    // FIXME: Use the browser's languages.
    auto messages = TRY(WebExtensions::MessageCatalog::load(root, manifest.default_locale.value_or({}), { "en_US"_string }, extension_id));
    return make<Extension>(move(extension_id), move(root), move(manifest), move(messages));
}

Extension const* ExtensionRegistry::add_loaded_extension(NonnullOwnPtr<Extension> extension)
{
    auto const* stored = extension.ptr();
    auto id = extension->id();
    m_extensions.set(move(id), move(extension));
    m_serialized_for_renderers.clear();
    return stored;
}

void ExtensionRegistry::notify_change()
{
    m_serialized_for_renderers.clear();
    if (on_change)
        on_change();
    for (auto& observer : m_change_observers)
        observer();
}

ErrorOr<Extension const*> ExtensionRegistry::load_unpacked(ByteString const& directory)
{
    auto const* stored = add_loaded_extension(TRY(read_extension(directory, {})));
    notify_change();
    return stored;
}
void ExtensionRegistry::set_allows_file_access(StringView id, bool allowed)
{
    auto key = MUST(String::from_utf8(id));
    if (allowed == m_file_access_allowed.contains(key))
        return;
    if (allowed)
        m_file_access_allowed.set(key);
    else
        m_file_access_allowed.remove(key);
    if (auto installed = m_installed.find(key); installed != m_installed.end()) {
        installed->value.allows_file_access = allowed;
        save_installed_extensions_or_complain();
    }
    notify_change();
}

bool ExtensionRegistry::allows_file_access(StringView id) const
{
    return m_file_access_allowed.contains(MUST(String::from_utf8(id)));
}

Extension const* ExtensionRegistry::find(StringView id) const
{
    auto it = m_extensions.find(id);
    return it == m_extensions.end() ? nullptr : it->value.ptr();
}

static JsonArray json_strings(Vector<String> const& strings)
{
    JsonArray array;
    for (auto const& string : strings)
        array.must_append(string);
    return array;
}

String ExtensionRegistry::serialize_for_renderers() const
{
    if (m_serialized_for_renderers.has_value())
        return *m_serialized_for_renderers;

    JsonArray extensions;
    for (auto const& [id, extension] : m_extensions) {
        auto const& manifest = extension->manifest();

        JsonObject messages;
        for (auto const& [name, message] : extension->messages().resolved_messages()) {
            JsonObject entry;
            entry.set("message"sv, message);
            messages.set(name, move(entry));
        }

        JsonArray content_scripts;
        for (auto const& script : manifest.content_scripts) {
            auto sources = [&](Vector<String> const& paths) {
                JsonArray array;
                for (auto const& path : paths) {
                    auto source = extension->read_file(path);
                    if (!source.has_value()) {
                        dbgln("Extensions: {} has no content script {}", id, path);
                        continue;
                    }
                    JsonObject entry;
                    entry.set("url"sv, MUST(String::formatted("chrome-extension://{}/{}", id, path.bytes_as_string_view().trim("/"sv, TrimMode::Left))));
                    entry.set("source"sv, source.release_value());
                    array.must_append(move(entry));
                }
                return array;
            };

            JsonObject entry;
            entry.set("matches"sv, json_strings(script.matches));
            entry.set("excludeMatches"sv, json_strings(script.exclude_matches));
            entry.set("includeGlobs"sv, json_strings(script.include_globs));
            entry.set("excludeGlobs"sv, json_strings(script.exclude_globs));
            entry.set("runAt"sv, script.run_at == WebExtensions::ContentScript::RunAt::DocumentStart ? "document_start"sv : script.run_at == WebExtensions::ContentScript::RunAt::DocumentEnd ? "document_end"sv : "document_idle"sv);
            entry.set("allFrames"sv, script.all_frames);
            entry.set("matchAboutBlank"sv, script.match_about_blank);
            entry.set("matchOriginAsFallback"sv, script.match_origin_as_fallback);
            entry.set("world"sv, script.world == WebExtensions::ContentScript::World::Main ? "MAIN"sv : "ISOLATED"sv);
            entry.set("js"sv, sources(script.js));
            entry.set("css"sv, sources(script.css));
            content_scripts.must_append(move(entry));
        }

        JsonObject entry;
        entry.set("id"sv, id);
        entry.set("manifest"sv, manifest.raw);
        entry.set("messages"sv, move(messages));
        entry.set("uiLocale"sv, extension->messages().ui_locale().is_empty() ? "en"_string : extension->messages().ui_locale());
        entry.set("allowFileAccess"sv, allows_file_access(id));
        entry.set("contentScripts"sv, move(content_scripts));
        // The policies the browser gives the extension's pages, which its workers and subresources take too.
        entry.set("contentSecurityPolicies"sv, json_strings(extension->content_security_policies()));
        extensions.must_append(move(entry));
    }

    JsonObject result;
    result.set("extensions"sv, move(extensions));
    m_serialized_for_renderers = result.serialized();
    return *m_serialized_for_renderers;
}

// A file of what the browser makes up, for a renderer to read: unlinked at once, it's gone when closed.
static ErrorOr<IPC::File> open_generated_file(ReadonlyBytes contents)
{
    auto pattern = ByteString::formatted("{}/ladybird-extension-XXXXXX", Core::StandardPaths::tempfile_directory());
    Vector<char> path;
    path.append(pattern.characters(), pattern.length() + 1);
    auto fd = TRY(Core::System::mkstemp(path.span()));
    auto file = IPC::File::adopt_fd(fd);
    (void)Core::System::unlink(StringView { path.data(), pattern.length() });
    TRY(Core::System::write(fd, contents));
    if (::lseek(fd, 0, SEEK_SET) < 0)
        return Error::from_syscall("lseek"sv, errno);
    return file;
}

ErrorOr<IPC::File> ExtensionRegistry::open_file(StringView id, StringView path, Optional<URL::Origin> const& requester) const
{
    auto const* extension = find(id);
    if (!extension)
        return Error::from_errno(ENOENT);

    if (!path.starts_with('/') || path.contains('\0') || path.contains('\\'))
        return Error::from_errno(EACCES);
    for (auto part : path.split_view('/')) {
        if (part == ".."sv)
            return Error::from_errno(EACCES);
    }

    if (!extension->may_be_loaded_by(path, requester))
        return Error::from_errno(EACCES);

    // The generated background page, which the extension only loads itself.
    if (path == Extension::background_page_path) {
        auto html = extension->background_page_html();
        if (!html.has_value() || html->is_empty() || (requester.has_value() && !requester->is_same_origin(extension->origin())))
            return Error::from_errno(ENOENT);
        return open_generated_file(html->bytes());
    }

    // Resolve links, and serve nothing outside the extension's folder.
    auto real_path = TRY(FileSystem::real_path(ByteString::formatted("{}{}", extension->root(), path)));
    return IPC::File::adopt_fd(TRY(open_inside(extension->root(), real_path)));
}

ErrorOr<IPC::File> ExtensionRegistry::open_stand_in_file(StringView id, StringView path, Optional<URL::Origin> const& requester) const
{
    (void)TRY(open_file(id, path, requester));
    return open_generated_file({});
}

// Permission warnings.

struct PermissionWords {
    StringView name;
    StringView description;
    bool warns;
};

// Every permission the browser gates something on, and the ones Chrome's install prompt names.
static constexpr PermissionWords permission_words[] = {
    { "activeTab"sv, "Use the page you're on when you click it"sv, false },
    { "alarms"sv, "Run on a schedule"sv, false },
    { "bookmarks"sv, "Read and change your bookmarks"sv, true },
    { "clipboardRead"sv, "Read what you copy"sv, true },
    { "clipboardWrite"sv, "Change what you copy"sv, true },
    { "contextMenus"sv, "Add items to menus"sv, false },
    { "cookies"sv, "Read and change cookies of sites it can access"sv, true },
    { "declarativeNetRequest"sv, "Block and change requests sites make"sv, true },
    { "declarativeNetRequestFeedback"sv, "See which requests its rules blocked or changed, and so your browsing history"sv, true },
    { "declarativeNetRequestWithHostAccess"sv, "Block and change requests to sites it can access"sv, true },
    { "downloads"sv, "Manage your downloads"sv, true },
    { "downloads.open"sv, "Open files you download"sv, true },
    { "history"sv, "Read and change your browsing history"sv, true },
    { "idle"sv, "Know when you're away"sv, false },
    { "management"sv, "Manage your extensions"sv, true },
    { "nativeMessaging"sv, "Talk to apps on this Mac"sv, true },
    { "notifications"sv, "Show notifications"sv, true },
    { "privacy"sv, "Change your privacy settings"sv, true },
    { "scripting"sv, "Run scripts in pages it can access"sv, false },
    { "storage"sv, "Store data"sv, false },
    { "tabs"sv, "Read your tabs and the pages they show"sv, true },
    { "unlimitedStorage"sv, "Store unlimited data"sv, false },
    { "webNavigation"sv, "Know which pages you visit"sv, true },
    { "webRequest"sv, "See the requests sites make"sv, true },
    { "webRequestAuthProvider"sv, "Answer sites' sign-in prompts"sv, true },
};

static PermissionWords const* words_for(StringView permission)
{
    for (auto const& entry : permission_words) {
        if (entry.name == permission)
            return &entry;
    }
    return nullptr;
}

Optional<StringView> permission_description(StringView permission)
{
    if (auto const* words = words_for(permission))
        return words->description;
    return {};
}

bool permission_has_warning(StringView permission)
{
    auto const* words = words_for(permission);
    return words && words->warns;
}

bool is_all_sites_pattern(StringView pattern)
{
    return pattern == "<all_urls>"sv || pattern.starts_with("*://*/"sv) || pattern.starts_with("https://*/"sv) || pattern.starts_with("http://*/"sv);
}

PermissionSummary summarize_permissions(ReadonlySpan<String> api_permissions, ReadonlySpan<String> site_patterns)
{
    PermissionSummary summary;
    for (auto const& permission : api_permissions) {
        if (auto description = permission_description(permission); description.has_value()) {
            auto text = MUST(String::from_utf8(*description));
            if (!summary.descriptions.contains_slow(text))
                summary.descriptions.append(move(text));
        } else if (!summary.undescribed.contains_slow(permission)) {
            summary.undescribed.append(permission);
        }
    }
    for (auto const& pattern : site_patterns) {
        if (is_all_sites_pattern(pattern))
            summary.all_sites = true;
        else if (!summary.sites.contains_slow(pattern))
            summary.sites.append(pattern);
    }
    if (summary.all_sites)
        summary.sites.clear();
    return summary;
}

static Vector<String> site_patterns_of(WebExtensions::Manifest const& manifest)
{
    Vector<String> sites = manifest.host_permissions;
    for (auto const& script : manifest.content_scripts)
        sites.extend(script.matches);
    return sites;
}

PermissionSummary summarize_permissions(WebExtensions::Manifest const& manifest)
{
    return summarize_permissions(manifest.permissions, site_patterns_of(manifest));
}

// What the new manifest asks for that the old didn't, of what the user is asked about. Optional permissions don't
// count: they're asked for when used.
static void add_permissions_to_approve(WebExtensions::Manifest const& old_manifest, WebExtensions::Manifest const& new_manifest, Vector<String>& permissions, Vector<String>& sites)
{
    for (auto const& permission : new_manifest.permissions) {
        if (permission_has_warning(permission) && !old_manifest.permissions.contains_slow(permission) && !permissions.contains_slow(permission))
            permissions.append(permission);
    }
    auto old_sites = site_patterns_of(old_manifest);
    bool had_all_sites = any_of(old_sites, [](auto const& pattern) { return is_all_sites_pattern(pattern); });
    if (had_all_sites)
        return;
    for (auto const& pattern : site_patterns_of(new_manifest)) {
        if (!old_sites.contains_slow(pattern) && !sites.contains_slow(pattern))
            sites.append(pattern);
    }
}

// Installs.

static constexpr StringView state_file_name = "Installed.json"sv;

// An extension's "version": 1-4 dot-separated integers from 0 to 65535, without leading zeros (as Chrome).
static bool is_valid_version(StringView version)
{
    if (version.is_empty() || version.length() > 23)
        return false;
    auto parts = version.split_view('.', SplitBehavior::KeepEmpty);
    if (parts.size() > 4)
        return false;
    for (auto part : parts) {
        if (part.is_empty() || part.length() > 5 || !all_of(part, is_ascii_digit) || (part.length() > 1 && part[0] == '0'))
            return false;
        if (part.to_number<u32>().value_or(65536) > 65535)
            return false;
    }
    return true;
}

// Compares valid versions by number, missing parts being 0.
static int compare_versions(StringView a, StringView b)
{
    auto a_parts = a.split_view('.');
    auto b_parts = b.split_view('.');
    for (size_t i = 0; i < max(a_parts.size(), b_parts.size()); ++i) {
        auto a_part = i < a_parts.size() ? a_parts[i].to_number<u32>().value_or(0) : 0;
        auto b_part = i < b_parts.size() ? b_parts[i].to_number<u32>().value_or(0) : 0;
        if (a_part != b_part)
            return a_part < b_part ? -1 : 1;
    }
    return 0;
}

// A folder only the user may enter: made when missing, and never a link.
static ErrorOr<void> ensure_private_directory(ByteString const& path)
{
    if (FileSystem::is_link(path))
        return Error::from_string_literal("Extensions folder is a link");
    if (!FileSystem::exists(path)) {
        auto result = Core::System::mkdir(path, 0700);
        if (result.is_error() && result.error().code() != EEXIST)
            return result.release_error();
    }
    if (!FileSystem::is_directory(path) || FileSystem::is_link(path))
        return Error::from_errno(ENOTDIR);
    TRY(Core::System::chmod(path, 0700));
    return {};
}

// Folders 0700 and files 0600, all the way down.
static ErrorOr<void> make_private_tree(ByteString const& path)
{
    auto st = TRY(Core::System::lstat(path));
    if (S_ISLNK(st.st_mode))
        return Error::from_string_literal("Unexpected link in an unpacked extension");
    if (!S_ISDIR(st.st_mode)) {
        TRY(Core::System::chmod(path, 0600));
        return {};
    }
    TRY(Core::System::chmod(path, 0700));
    Core::DirIterator iterator(path, Core::DirIterator::SkipParentAndBaseDir);
    while (iterator.has_next())
        TRY(make_private_tree(iterator.next_full_path()));
    return {};
}

void ExtensionRegistry::set_extensions_directory(ByteString directory)
{
    m_extensions_directory = move(directory);
}

// The extensions folder, made private, as a real path.
ErrorOr<ByteString> ExtensionRegistry::private_extensions_directory() const
{
    if (!m_extensions_directory.has_value())
        return Error::from_string_literal("Extensions can't be installed in this profile");
    TRY(ensure_private_directory(*m_extensions_directory));
    return FileSystem::real_path(*m_extensions_directory);
}

static StringView install_type_name(InstalledExtension::InstallType type)
{
    return type == InstalledExtension::InstallType::Normal ? "normal"sv : "development"sv;
}

ErrorOr<void> ExtensionRegistry::save_installed_extensions() const
{
    auto directory = TRY(private_extensions_directory());

    Vector<InstalledExtension const*> records;
    for (auto const& [id, record] : m_installed)
        records.append(&record);
    quick_sort(records, [](auto const* a, auto const* b) { return a->id < b->id; });

    JsonArray extensions;
    for (auto const* record : records) {
        JsonObject entry;
        entry.set("id"sv, record->id);
        entry.set("version"sv, record->version);
        entry.set("name"sv, record->name);
        entry.set("path"sv, TRY(String::from_byte_string(record->path)));
        entry.set("installType"sv, install_type_name(record->install_type));
        entry.set("enabled"sv, record->enabled);
        entry.set("allowsFileAccess"sv, record->allows_file_access);
        entry.set("installTime"sv, record->install_time.milliseconds_since_epoch());
        if (record->needs_approval()) {
            JsonArray permissions;
            for (auto const& permission : record->permissions_to_approve)
                permissions.must_append(permission);
            JsonArray sites;
            for (auto const& site : record->sites_to_approve)
                sites.must_append(site);
            entry.set("permissionsToApprove"sv, move(permissions));
            entry.set("sitesToApprove"sv, move(sites));
        }
        extensions.must_append(move(entry));
    }
    JsonObject state;
    state.set("version"sv, 1);
    state.set("extensions"sv, move(extensions));
    auto json = state.serialized();

    // Written whole to a file of its own, then renamed over the old one.
    auto pattern = ByteString::formatted("{}/{}.XXXXXX", directory, state_file_name);
    Vector<char> path;
    path.append(pattern.characters(), pattern.length() + 1);
    auto fd = TRY(Core::System::mkstemp(path.span()));
    StringView temporary_path { path.data(), pattern.length() };
    auto result = [&]() -> ErrorOr<void> {
        TRY(Core::System::fchmod(fd, 0600));
        auto remaining = json.bytes();
        while (!remaining.is_empty()) {
            auto written = TRY(Core::System::write(fd, remaining));
            remaining = remaining.slice(written);
        }
        if (::fsync(fd) < 0)
            return Error::from_syscall("fsync"sv, errno);
        return {};
    }();
    (void)Core::System::close(fd);
    if (!result.is_error())
        result = Core::System::rename(temporary_path, ByteString::formatted("{}/{}", directory, state_file_name));
    if (result.is_error())
        (void)Core::System::unlink(temporary_path);
    return result;
}

void ExtensionRegistry::save_installed_extensions_or_complain() const
{
    if (auto result = save_installed_extensions(); result.is_error())
        dbgln("Extensions: Can't remember the installed extensions: {}", result.error());
}

ErrorOr<void> ExtensionRegistry::load_installed_extensions()
{
    auto directory = TRY(private_extensions_directory());
    auto state_path = ByteString::formatted("{}/{}", directory, state_file_name);
    if (!FileSystem::exists(state_path))
        return {};

    auto file = TRY(Core::File::open(state_path, Core::File::OpenMode::Read));
    auto contents = TRY(file->read_until_eof());
    auto json = TRY(JsonValue::from_string(contents));
    if (!json.is_object())
        return Error::from_string_literal("Installed extensions: not an object");
    auto extensions = json.as_object().get_array("extensions"sv);
    if (!extensions.has_value())
        return Error::from_string_literal("Installed extensions: no list");

    bool loaded_any = false;
    for (auto const& value : extensions->values()) {
        if (!value.is_object())
            continue;
        auto const& entry = value.as_object();
        auto id = entry.get_string("id"sv);
        auto version = entry.get_string("version"sv);
        auto path = entry.get_string("path"sv);
        auto type = entry.get_string("installType"sv);
        if (!id.has_value() || !WebExtensions::is_valid_extension_id(*id) || !version.has_value() || !is_valid_version(*version) || !path.has_value() || !type.has_value())
            continue;

        InstalledExtension record;
        record.id = *id;
        record.version = *version;
        record.name = entry.get_string("name"sv).value_or({});
        record.path = path->to_byte_string();
        if (*type == "normal"sv) {
            record.install_type = InstalledExtension::InstallType::Normal;
            // CRX installs live in the profile, and nowhere else.
            auto real_path = FileSystem::real_path(record.path);
            if (real_path.is_error() || !real_path.value().starts_with(ByteString::formatted("{}/{}/", directory, record.id)))
                continue;
            record.path = real_path.release_value();
        } else if (*type == "development"sv) {
            record.install_type = InstalledExtension::InstallType::Development;
            if (!record.path.starts_with('/'))
                continue;
        } else {
            continue;
        }
        record.enabled = entry.get_bool("enabled"sv).value_or(false);
        record.allows_file_access = entry.get_bool("allowsFileAccess"sv).value_or(false);
        record.install_time = UnixDateTime::from_milliseconds_since_epoch(entry.get_integer<i64>("installTime"sv).value_or(0));
        auto read_strings = [&](StringView key, Vector<String>& strings) {
            auto array = entry.get_array(key);
            if (!array.has_value())
                return;
            for (auto const& item : array->values()) {
                if (item.is_string() && strings.size() < 1000)
                    strings.append(item.as_string());
            }
        };
        read_strings("permissionsToApprove"sv, record.permissions_to_approve);
        read_strings("sitesToApprove"sv, record.sites_to_approve);
        if (record.needs_approval())
            record.enabled = false;

        if (record.enabled) {
            auto extension = read_extension(record.path, record.install_type == InstalledExtension::InstallType::Normal ? Optional<String> { record.id } : OptionalNone {});
            if (extension.is_error()) {
                dbgln("Extensions: Can't load {} from {}: {}", record.id, record.path, extension.error());
            } else if (extension.value()->id() != record.id) {
                dbgln("Extensions: {} is now another extension, {}", record.path, extension.value()->id());
            } else {
                add_loaded_extension(extension.release_value());
                loaded_any = true;
            }
        }
        if (record.allows_file_access)
            m_file_access_allowed.set(record.id);
        else
            m_file_access_allowed.remove(record.id);
        m_installed.set(record.id, move(record));
    }
    if (loaded_any)
        notify_change();
    return {};
}

ErrorOr<Extension const*> ExtensionRegistry::install_crx(ReadonlyBytes bytes, Optional<StringView> expected_id)
{
    auto directory = TRY(private_extensions_directory());

    auto crx = TRY(WebExtensions::CrxFile::parse_and_verify(bytes));
    auto const& id = crx.extension_id;
    if (expected_id.has_value() && id != *expected_id)
        return Error::from_string_literal("The CRX is another extension");

    // Chrome takes the CRX's key as the extension's: one the manifest names must be the same.
    auto archive = TRY(WebExtensions::ZipArchive::parse(crx.zip_archive));
    auto manifest_entry = archive.find("manifest.json"sv);
    if (!manifest_entry.has_value())
        return Error::from_string_literal("The CRX has no manifest");
    auto manifest = TRY(WebExtensions::Manifest::parse(TRY(archive.read(*manifest_entry))));
    if (manifest.key.has_value() && TRY(WebExtensions::extension_id_from_base64_key(*manifest.key)) != id)
        return Error::from_string_literal("The manifest's key isn't the CRX's");
    if (!is_valid_version(manifest.version))
        return Error::from_string_literal("Invalid extension version");

    auto existing = m_installed.find(id);
    if (existing != m_installed.end()) {
        if (existing->value.install_type != InstalledExtension::InstallType::Normal)
            return Error::from_string_literal("The extension is installed from a folder; remove it first");
        if (compare_versions(manifest.version, existing->value.version) < 0)
            return Error::from_string_literal("A newer version of the extension is installed");
    }

    auto extension_directory = ByteString::formatted("{}/{}", directory, id);
    TRY(ensure_private_directory(extension_directory));
    ByteString version_directory;
    for (size_t n = 0; n < 100 && version_directory.is_empty(); ++n) {
        auto candidate = ByteString::formatted("{}/{}_{}", extension_directory, manifest.version, n);
        if (!FileSystem::exists(candidate) && !FileSystem::is_link(candidate))
            version_directory = move(candidate);
    }
    if (version_directory.is_empty())
        return Error::from_errno(EEXIST);

    bool installed = false;
    bool is_new = existing == m_installed.end();
    ScopeGuard remove_on_failure = [&] {
        if (!installed)
            (void)remove_without_following_links(is_new ? extension_directory : version_directory);
    };
    TRY(WebExtensions::ZipArchive::extract_to_directory(crx.zip_archive, version_directory));
    TRY(make_private_tree(version_directory));
    auto extension = TRY(read_extension(version_directory, id));

    // It loaded: it replaces the installed version.
    InstalledExtension record;
    if (existing != m_installed.end()) {
        record = existing->value;
    } else {
        record.id = id;
        record.install_type = InstalledExtension::InstallType::Normal;
        record.enabled = true;
        record.allows_file_access = false;
        record.install_time = UnixDateTime::now();
    }
    if (existing != m_installed.end()) {
        // What's still to approve, if the new version asks for it, and what it asks for that the old one didn't.
        auto const& new_manifest = extension->manifest();
        auto new_sites = site_patterns_of(new_manifest);
        Vector<String> permissions;
        Vector<String> sites;
        for (auto const& permission : record.permissions_to_approve) {
            if (new_manifest.permissions.contains_slow(permission))
                permissions.append(permission);
        }
        for (auto const& site : record.sites_to_approve) {
            if (new_sites.contains_slow(site))
                sites.append(site);
        }
        auto old_extension = read_extension(record.path, id);
        // An old version that's gone or broken agreed to nothing.
        add_permissions_to_approve(old_extension.is_error() ? WebExtensions::Manifest {} : old_extension.value()->manifest(), new_manifest, permissions, sites);
        record.permissions_to_approve = move(permissions);
        record.sites_to_approve = move(sites);
        if (record.needs_approval()) {
            record.enabled = false;
            m_extensions.remove(id);
        }
    }
    record.version = extension->manifest().version;
    record.name = extension->messages().substitute_manifest_messages(extension->manifest().name);
    record.path = extension->root();
    installed = true;

    Extension const* stored = nullptr;
    if (record.enabled)
        stored = add_loaded_extension(move(extension));
    m_installed.set(id, move(record));
    save_installed_extensions_or_complain();

    // Then the old versions go.
    auto kept = LexicalPath::basename(version_directory);
    Core::DirIterator versions(extension_directory, Core::DirIterator::SkipParentAndBaseDir);
    while (versions.has_next()) {
        auto name = versions.next_path();
        if (name != kept)
            (void)remove_without_following_links(ByteString::formatted("{}/{}", extension_directory, name));
    }

    notify_change();
    return stored;
}

ErrorOr<Extension const*> ExtensionRegistry::install_unpacked(ByteString const& directory)
{
    TRY(private_extensions_directory());
    auto extension = TRY(read_extension(directory, {}));
    auto id = extension->id();

    auto existing = m_installed.find(id);
    if (existing != m_installed.end() && existing->value.install_type != InstalledExtension::InstallType::Development)
        return Error::from_string_literal("The extension is installed from a CRX; remove it first");

    InstalledExtension record;
    record.id = id;
    record.version = extension->manifest().version;
    record.name = extension->messages().substitute_manifest_messages(extension->manifest().name);
    record.path = extension->root();
    record.install_type = InstalledExtension::InstallType::Development;
    record.enabled = true;
    record.allows_file_access = allows_file_access(id);
    record.install_time = existing != m_installed.end() ? existing->value.install_time : UnixDateTime::now();

    auto const* stored = add_loaded_extension(move(extension));
    m_installed.set(id, move(record));
    save_installed_extensions_or_complain();
    notify_change();
    return stored;
}

ErrorOr<void> ExtensionRegistry::set_enabled(StringView id, bool enabled)
{
    auto installed = m_installed.find(id);
    if (installed == m_installed.end())
        return Error::from_errno(ENOENT);
    auto& record = installed->value;
    if (record.enabled == enabled)
        return {};
    if (enabled && record.needs_approval())
        return Error::from_string_literal("The extension's update asks for more permissions: approve them first");

    if (enabled) {
        auto extension = TRY(read_extension(record.path, record.install_type == InstalledExtension::InstallType::Normal ? Optional<String> { record.id } : OptionalNone {}));
        if (extension->id() != record.id)
            return Error::from_string_literal("The extension's folder now holds another extension");
        add_loaded_extension(move(extension));
    } else {
        m_extensions.remove(id);
    }
    record.enabled = enabled;
    save_installed_extensions_or_complain();
    notify_change();
    return {};
}

ErrorOr<void> ExtensionRegistry::approve_permissions(StringView id)
{
    auto installed = m_installed.find(id);
    if (installed == m_installed.end())
        return Error::from_errno(ENOENT);
    auto& record = installed->value;
    if (!record.needs_approval())
        return set_enabled(id, true);
    auto permissions = move(record.permissions_to_approve);
    auto sites = move(record.sites_to_approve);
    record.permissions_to_approve.clear();
    record.sites_to_approve.clear();
    if (auto result = set_enabled(id, true); result.is_error()) {
        record.permissions_to_approve = move(permissions);
        record.sites_to_approve = move(sites);
        return result;
    }
    return {};
}

ErrorOr<void> ExtensionRegistry::uninstall(StringView id)
{
    auto installed = m_installed.find(id);
    if (installed == m_installed.end())
        return Error::from_errno(ENOENT);
    auto record = move(installed->value);
    m_installed.remove(installed);
    m_extensions.remove(record.id);
    m_file_access_allowed.remove(record.id);

    // The user's own folder stays.
    if (record.install_type == InstalledExtension::InstallType::Normal) {
        auto directory = TRY(private_extensions_directory());
        auto extension_directory = ByteString::formatted("{}/{}", directory, record.id);
        if (WebExtensions::is_valid_extension_id(record.id) && FileSystem::exists(extension_directory)) {
            if (auto result = remove_without_following_links(extension_directory); result.is_error())
                dbgln("Extensions: Can't remove {}: {}", extension_directory, result.error());
        }
    }
    save_installed_extensions_or_complain();
    notify_change();
    for (auto& observer : m_uninstall_observers)
        observer(record.id);
    return {};
}

#if !defined(AK_OS_WINDOWS)
// Removes what's in the open folder by names relative to it, opening each folder below it without following a link:
// a folder swapped for a link meanwhile is removed as the link it is, and what it names is never reached.
static ErrorOr<void> remove_directory_contents(int directory_fd, size_t depth)
{
    if (depth > 128)
        return Error::from_string_literal("Folders nested too deeply to remove");
    Vector<ByteString> names;
    {
        auto listing_fd = TRY(Core::System::dup(directory_fd));
        auto* directory = ::fdopendir(listing_fd);
        if (!directory) {
            auto error = errno;
            (void)Core::System::close(listing_fd);
            return Error::from_errno(error);
        }
        ::rewinddir(directory);
        while (auto* entry = ::readdir(directory)) {
            StringView name { entry->d_name, strlen(entry->d_name) };
            if (name != "."sv && name != ".."sv)
                names.append(name);
        }
        ::closedir(directory);
    }
    for (auto const& name : names) {
        struct stat st;
        if (::fstatat(directory_fd, name.characters(), &st, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT)
                continue;
            return Error::from_syscall("fstatat"sv, errno);
        }
        if (S_ISDIR(st.st_mode)) {
            auto child_fd = ::openat(directory_fd, name.characters(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child_fd < 0) {
                // No longer a folder: a link or file now, removed as such.
                if (errno != ELOOP && errno != ENOTDIR)
                    return Error::from_syscall("openat"sv, errno);
                if (::unlinkat(directory_fd, name.characters(), 0) < 0 && errno != ENOENT)
                    return Error::from_syscall("unlinkat"sv, errno);
                continue;
            }
            auto result = remove_directory_contents(child_fd, depth + 1);
            (void)Core::System::close(child_fd);
            TRY(result);
            if (::unlinkat(directory_fd, name.characters(), AT_REMOVEDIR) < 0 && errno != ENOENT)
                return Error::from_syscall("unlinkat"sv, errno);
        } else if (::unlinkat(directory_fd, name.characters(), 0) < 0 && errno != ENOENT) {
            return Error::from_syscall("unlinkat"sv, errno);
        }
    }
    return {};
}
#endif

ErrorOr<void> ExtensionRegistry::remove_without_following_links(ByteString const& path)
{
    auto stat = TRY(Core::System::lstat(path));
    if (!S_ISDIR(stat.st_mode))
        return Core::System::unlink(path);

#if defined(AK_OS_WINDOWS)
    Vector<ByteString> entries;
    {
        auto directory = TRY(Core::Directory::create(path, Core::Directory::CreateDirectories::No));
        TRY(directory.for_each_entry(Core::DirIterator::Flags::SkipParentAndBaseDir, [&](auto const& entry, auto const&) -> ErrorOr<IterationDecision> {
            entries.append(ByteString::formatted("{}/{}", path, entry.name));
            return IterationDecision::Continue;
        }));
    }
    for (auto const& entry : entries)
        TRY(remove_without_following_links(entry));
    return Core::System::rmdir(path);
#else
    auto fd = ::open(path.characters(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        // Swapped for a link since: only the link goes.
        if (errno == ELOOP || errno == ENOTDIR)
            return Core::System::unlink(path);
        return Error::from_syscall("open"sv, errno);
    }
    auto result = remove_directory_contents(fd, 0);
    (void)Core::System::close(fd);
    TRY(result);
    return Core::System::rmdir(path);
#endif
}

Vector<InstalledExtension> ExtensionRegistry::installed_extensions() const
{
    Vector<InstalledExtension> records;
    for (auto const& [id, record] : m_installed)
        records.append(record);
    quick_sort(records, [](auto const& a, auto const& b) { return a.name == b.name ? a.id < b.id : a.name < b.name; });
    return records;
}

Optional<InstalledExtension const&> ExtensionRegistry::installed_extension(StringView id) const
{
    auto installed = m_installed.find(id);
    if (installed == m_installed.end())
        return {};
    return installed->value;
}

// The Chrome Web Store.

static Optional<String> extension_id_in(StringView candidate)
{
    auto id = String::from_utf8(candidate);
    if (id.is_error() || !WebExtensions::is_valid_extension_id(id.value().to_ascii_lowercase()))
        return {};
    return id.value().to_ascii_lowercase();
}

Optional<String> ExtensionRegistry::extension_id_from_web_store_input(StringView input)
{
    input = input.trim_whitespace();
    if (auto id = extension_id_in(input); id.has_value())
        return id;

    auto url = input.contains("://"sv) ? URL::Parser::basic_parse(input) : URL::Parser::basic_parse(ByteString::formatted("https://{}", input));
    if (!url.has_value() || (url->scheme() != "https"sv && url->scheme() != "http"sv) || !url->host().has_value())
        return {};

    Vector<String> segments;
    for (auto segment : url->path_segments()) {
        auto string = String::from_utf8(segment);
        if (string.is_error())
            return {};
        segments.append(string.release_value());
    }

    // chromewebstore.google.com/detail/<slug>/<id>, or /detail/<id>; chrome.google.com/webstore/detail/...
    size_t first = 0;
    auto host = url->serialized_host();
    if (host == "chromewebstore.google.com"sv)
        first = 0;
    else if (host == "chrome.google.com"sv && !segments.is_empty() && segments[0] == "webstore"sv)
        first = 1;
    else
        return {};
    if (segments.size() <= first + 1 || segments[first] != "detail"sv)
        return {};
    if (auto id = extension_id_in(segments[first + 1]); id.has_value())
        return id;
    if (segments.size() > first + 2)
        return extension_id_in(segments[first + 2]);
    return {};
}

// Where the store's answers and downloads may come from: Google's update service and the hosts it sends CRXs from.
static bool is_web_store_url(URL::URL const& url)
{
    if (url.scheme() != "https"sv || !url.host().has_value() || !url.username().is_empty() || !url.password().is_empty())
        return false;
    if (url.port().has_value() && *url.port() != 443)
        return false;
    auto host = url.serialized_host();
    return host == "clients2.google.com"sv
        || host == "clients2.googleusercontent.com"sv
        || (host.ends_with(".gvt1.com"sv) && host.length() > ".gvt1.com"sv.length());
}

static void fetch_with_request_server(URL::URL const& url, size_t maximum_size, ExtensionRegistry::FetchCallback callback)
{
    struct Fetch : RefCounted<Fetch> {
        RefPtr<Requests::Request> request;
        RefPtr<Core::Timer> timeout;
        ExtensionRegistry::FetchCallback callback;
        ExtensionRegistry::FetchResponse response;
        size_t maximum_size { 0 };

        void finish(ErrorOr<ExtensionRegistry::FetchResponse> result, bool stop_request = false)
        {
            if (!callback)
                return;
            auto finished = move(callback);
            if (timeout)
                timeout->stop();
            // The request and timer go once their callbacks have returned.
            Core::deferred_invoke([request = move(request), timeout = move(timeout), stop_request] {
                if (request && stop_request)
                    request->stop();
            });
            finished(move(result));
        }
    };

    auto client = Application::request_server_client();
    if (client.is_error()) {
        callback(client.release_error());
        return;
    }
    auto headers = HTTP::HeaderList::create();
    headers->set({ "User-Agent"sv, Web::default_user_agent });
    auto request = client.value()->start_request("GET"sv, url, *headers, {}, HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No);
    if (!request) {
        callback(Error::from_string_literal("Can't start the request"));
        return;
    }

    auto fetch = adopt_ref(*new Fetch);
    fetch->request = request;
    fetch->callback = move(callback);
    fetch->maximum_size = maximum_size;
    fetch->timeout = Core::Timer::create_single_shot(AK::Duration::from_seconds(5 * 60), [fetch = fetch.ptr()] {
        fetch->finish(Error::from_string_literal("The download timed out"), true);
    });
    fetch->timeout->start();
    request->set_unbuffered_request_callbacks(
        [fetch](NonnullRefPtr<HTTP::HeaderList> headers, Optional<u32> response_code, Optional<String> const&, Optional<Core::ImmutableBytes>, Optional<u64>, Requests::CacheState) {
            fetch->response.status = response_code.value_or(0);
            if (auto location = headers->get("Location"sv); location.has_value())
                fetch->response.location = String::from_utf8(location->bytes()).value_or({});
        },
        [fetch](Requests::ResponseData data) {
            if (!fetch->callback)
                return;
            if (data.bytes().size() > fetch->maximum_size - fetch->response.body.size()) {
                fetch->finish(Error::from_string_literal("The download is too large"), true);
                return;
            }
            if (auto result = fetch->response.body.try_append(data.bytes()); result.is_error())
                fetch->finish(result.release_error(), true);
        },
        [](Core::ImmutableBytes) {},
        [fetch](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> network_error) {
            if (network_error.has_value()) {
                fetch->finish(Error::from_string_view(Requests::network_error_to_string(*network_error)));
                return;
            }
            fetch->finish(move(fetch->response));
        });
}

void ExtensionRegistry::fetch_following_redirects(URL::URL url, size_t maximum_size, u32 redirects_left, Function<void(ErrorOr<ByteBuffer>)> callback)
{
    if (!is_web_store_url(url)) {
        callback(Error::from_string_literal("Extensions download only from the Chrome Web Store"));
        return;
    }
    auto on_response = [this, url, maximum_size, redirects_left, callback = move(callback)](ErrorOr<FetchResponse> result) mutable {
        if (result.is_error()) {
            callback(result.release_error());
            return;
        }
        auto response = result.release_value();
        if (response.status == 301 || response.status == 302 || response.status == 303 || response.status == 307 || response.status == 308) {
            if (redirects_left == 0) {
                callback(Error::from_string_literal("Too many redirects"));
                return;
            }
            auto next = response.location.has_value() ? url.complete_url(*response.location) : Optional<URL::URL> {};
            if (!next.has_value()) {
                callback(Error::from_string_literal("Invalid redirect"));
                return;
            }
            fetch_following_redirects(next.release_value(), maximum_size, redirects_left - 1, move(callback));
            return;
        }
        if (response.status != 200) {
            callback(Error::from_string_literal("The Chrome Web Store didn't answer"));
            return;
        }
        if (response.body.size() > maximum_size) {
            callback(Error::from_string_literal("The download is too large"));
            return;
        }
        callback(move(response.body));
    };
    if (m_fetcher)
        m_fetcher(url, maximum_size, move(on_response));
    else
        fetch_with_request_server(url, maximum_size, move(on_response));
}

static constexpr u32 maximum_redirects = 10;

// The manifest in a package, as it would install, for the user to look at first.
static ErrorOr<WebExtensions::Manifest> manifest_in_crx(ReadonlyBytes crx)
{
    auto file = TRY(WebExtensions::CrxFile::parse_and_verify(crx));
    auto archive = TRY(WebExtensions::ZipArchive::parse(file.zip_archive));
    auto entry = archive.find("manifest.json"sv);
    if (!entry.has_value())
        return Error::from_string_literal("The package has no manifest");
    auto json = TRY(archive.read(*entry));
    auto manifest = TRY(WebExtensions::Manifest::parse(json));

    // Its name may come from its messages.
    if (manifest.default_locale.has_value()) {
        auto locale = WebExtensions::normalize_locale_name(*manifest.default_locale);
        auto messages_entry = locale.has_value() ? archive.find(ByteString::formatted("_locales/{}/messages.json", *locale)) : Optional<WebExtensions::ZipArchive::Entry const&> {};
        if (messages_entry.has_value()) {
            if (auto messages_json = archive.read(*messages_entry); !messages_json.is_error()) {
                WebExtensions::MessageCatalog catalog;
                if (!catalog.add_catalog(*locale, messages_json.value()).is_error())
                    WebExtensions::localize(manifest, catalog);
            }
        }
    }
    return manifest;
}

void ExtensionRegistry::download_and_install(URL::URL const& url, String id, Optional<ByteBuffer> expected_sha256, Function<void(ErrorOr<Extension const*>)> callback, ConfirmInstall confirm)
{
    fetch_following_redirects(url, m_maximum_download_size, maximum_redirects, [this, id = move(id), expected_sha256 = move(expected_sha256), callback = move(callback), confirm = move(confirm)](ErrorOr<ByteBuffer> crx) mutable {
        if (crx.is_error()) {
            callback(crx.release_error());
            return;
        }
        if (expected_sha256.has_value() && Crypto::Hash::SHA256::hash(crx.value()).bytes() != expected_sha256->bytes()) {
            callback(Error::from_string_literal("The download isn't the one the Chrome Web Store named"));
            return;
        }
        if (!confirm) {
            callback(install_crx(crx.value(), id));
            return;
        }
        auto manifest = manifest_in_crx(crx.value());
        if (manifest.is_error()) {
            callback(manifest.release_error());
            return;
        }
        confirm(manifest.value(), [this, id = move(id), crx = crx.release_value(), callback = move(callback)](bool install) mutable {
            if (!install) {
                callback(Error::from_string_literal("Not installed"));
                return;
            }
            callback(install_crx(crx, id));
        });
    });
}

void ExtensionRegistry::install_from_web_store(StringView id_or_url, Function<void(ErrorOr<Extension const*>)> callback, ConfirmInstall confirm)
{
    auto id = extension_id_from_web_store_input(id_or_url);
    if (!id.has_value()) {
        callback(Error::from_string_literal("Not a Chrome Web Store extension"));
        return;
    }
    auto url = URL::Parser::basic_parse(ByteString::formatted("https://clients2.google.com/service/update2/crx?response=redirect&prodversion={}&acceptformat=crx3&x=id%3D{}%26uc", web_store_product_version, *id));
    VERIFY(url.has_value());
    download_and_install(*url, id.release_value(), {}, move(callback), move(confirm));
}

// What the update service's answer (Chrome's update2 protocol) says of one extension:
// <gupdate><app appid="..." status="ok"><updatecheck status="ok"|"noupdate" codebase="..." version="..." hash_sha256="..."/></app></gupdate>
struct UpdateCheck {
    String status;
    Optional<String> codebase;
    Optional<String> version;
    Optional<String> hash_sha256;
};

static Optional<String> decode_xml_attribute(StringView value)
{
    StringBuilder builder;
    for (size_t i = 0; i < value.length(); ++i) {
        if (value[i] != '&') {
            builder.append(value[i]);
            continue;
        }
        auto rest = value.substring_view(i);
        auto entity = [&](StringView name, char replacement) {
            if (!rest.starts_with(name))
                return false;
            builder.append(replacement);
            i += name.length() - 1;
            return true;
        };
        if (!entity("&amp;"sv, '&') && !entity("&lt;"sv, '<') && !entity("&gt;"sv, '>') && !entity("&quot;"sv, '"') && !entity("&apos;"sv, '\''))
            return {};
    }
    // Not UTF-8: a broken response.
    auto decoded = builder.to_string();
    if (decoded.is_error())
        return {};
    return decoded.release_value();
}

static ErrorOr<Optional<UpdateCheck>> parse_update_check(StringView xml, StringView id)
{
    Optional<String> current_app;
    Optional<UpdateCheck> found;
    GenericLexer lexer(xml);
    while (!lexer.is_eof()) {
        lexer.ignore_until('<');
        if (lexer.is_eof())
            break;
        lexer.ignore();
        if (lexer.next_is('?') || lexer.next_is('!')) {
            lexer.ignore_until('>');
            if (!lexer.consume_specific('>'))
                return Error::from_string_literal("Update response: unterminated declaration");
            continue;
        }
        bool closing = lexer.consume_specific('/');
        auto name = lexer.consume_while([](char c) { return is_ascii_alphanumeric(c) || c == '_' || c == '-' || c == ':'; });
        HashMap<StringView, StringView> attributes;
        while (true) {
            lexer.ignore_while(is_ascii_space);
            if (lexer.is_eof())
                return Error::from_string_literal("Update response: unterminated tag");
            if (lexer.consume_specific("/>"sv) || lexer.consume_specific('>'))
                break;
            auto attribute = lexer.consume_while([](char c) { return is_ascii_alphanumeric(c) || c == '_' || c == '-' || c == ':'; });
            if (attribute.is_empty() || !lexer.consume_specific('='))
                return Error::from_string_literal("Update response: invalid attribute");
            auto quote = lexer.peek();
            if (quote != '"' && quote != '\'')
                return Error::from_string_literal("Update response: unquoted attribute");
            lexer.ignore();
            auto value = lexer.consume_until(quote);
            if (!lexer.consume_specific(quote))
                return Error::from_string_literal("Update response: unterminated attribute");
            attributes.set(attribute, value);
        }

        if (name == "app"sv) {
            if (closing) {
                current_app.clear();
            } else {
                auto appid = attributes.get("appid"sv);
                current_app = appid.has_value() ? decode_xml_attribute(*appid) : Optional<String> {};
            }
        } else if (name == "updatecheck"sv && !closing && current_app == id && !found.has_value()) {
            auto attribute = [&](StringView key) -> Optional<String> {
                auto value = attributes.get(key);
                return value.has_value() ? decode_xml_attribute(*value) : Optional<String> {};
            };
            found = UpdateCheck { attribute("status"sv).value_or({}), attribute("codebase"sv), attribute("version"sv), attribute("hash_sha256"sv) };
        }
    }
    return found;
}

void ExtensionRegistry::check_for_updates(Function<void(Vector<UpdateResult>)> callback)
{
    struct Check : RefCounted<Check> {
        Vector<UpdateResult> results;
        size_t pending { 0 };
        Function<void(Vector<UpdateResult>)> callback;

        void done()
        {
            if (--pending == 0)
                callback(move(results));
        }
    };

    Vector<InstalledExtension> to_check;
    for (auto const& [id, record] : m_installed) {
        if (record.install_type == InstalledExtension::InstallType::Normal)
            to_check.append(record);
    }
    if (to_check.is_empty()) {
        callback({});
        return;
    }
    quick_sort(to_check, [](auto const& a, auto const& b) { return a.id < b.id; });

    auto check = adopt_ref(*new Check);
    check->callback = move(callback);
    check->pending = to_check.size();
    for (auto const& record : to_check)
        check->results.append({ record.id, {}, {} });

    for (size_t i = 0; i < to_check.size(); ++i) {
        auto const& record = to_check[i];
        auto url = URL::Parser::basic_parse(ByteString::formatted("https://clients2.google.com/service/update2/crx?response=updatecheck&prodversion={}&acceptformat=crx3&x=id%3D{}%26v%3D{}%26uc", web_store_product_version, record.id, record.version));
        VERIFY(url.has_value());
        auto fail = [check, i](StringView error) {
            check->results[i].error = MUST(String::from_utf8(error));
            check->done();
        };
        fetch_following_redirects(*url, 1 * MiB, 3, [this, check, i, fail, id = record.id, installed_version = record.version](ErrorOr<ByteBuffer> response) mutable {
            if (response.is_error())
                return fail(response.error().string_literal().is_empty() ? "Can't reach the Chrome Web Store"sv : response.error().string_literal());
            auto update = parse_update_check(StringView { response.value().bytes() }, id);
            if (update.is_error())
                return fail(update.error().string_literal());
            if (!update.value().has_value())
                return fail("The Chrome Web Store didn't answer for the extension"sv);
            auto const& answer = *update.value();
            if (answer.status == "noupdate"sv) {
                check->done();
                return;
            }
            if (answer.status != "ok"sv)
                return fail("The Chrome Web Store has no update"sv);
            if (!answer.version.has_value() || !is_valid_version(*answer.version) || !answer.codebase.has_value())
                return fail("Invalid update response"sv);
            if (compare_versions(*answer.version, installed_version) <= 0) {
                check->done();
                return;
            }
            auto codebase = URL::Parser::basic_parse(*answer.codebase);
            if (!codebase.has_value())
                return fail("Invalid update URL"sv);
            Optional<ByteBuffer> hash;
            if (answer.hash_sha256.has_value() && !answer.hash_sha256->is_empty()) {
                auto decoded = decode_hex(*answer.hash_sha256);
                if (decoded.is_error() || decoded.value().size() != 32)
                    return fail("Invalid update hash"sv);
                hash = decoded.release_value();
            }
            download_and_install(*codebase, id, move(hash), [this, check, i, fail, id](ErrorOr<Extension const*> installed) {
                if (installed.is_error())
                    return fail(installed.error().string_literal().is_empty() ? "Can't install the update"sv : installed.error().string_literal());
                if (auto record = installed_extension(id); record.has_value())
                    check->results[i].updated_to = record->version;
                check->done();
            });
        });
    }
}
}
