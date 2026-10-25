/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Parser.h>
#include <LibWebExtensions/ExtensionId.h>
#include <LibWebExtensions/MatchPattern.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/Utilities.h>

namespace WebView {

Extension::Extension(String id, ByteString root, WebExtensions::Manifest manifest, WebExtensions::MessageCatalog messages)
    : m_id(move(id))
    , m_root(move(root))
    , m_manifest(move(manifest))
    , m_messages(move(messages))
{
}

Optional<String> Extension::read_file(StringView path) const
{
    auto relative_path = path.trim("/"sv, TrimMode::Left);
    if (relative_path.is_empty() || relative_path.contains('\\') || any_of(relative_path.split_view('/'), [](auto part) { return part == ".."sv; }))
        return {};
    auto real_path = FileSystem::real_path(ByteString::formatted("{}/{}", m_root, relative_path));
    if (real_path.is_error() || !real_path.value().starts_with(ByteString::formatted("{}/", m_root)))
        return {};
    auto file = Core::File::open(real_path.value(), Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    auto string = String::from_utf8(contents.value());
    if (string.is_error())
        return {};
    return string.release_value();
}

URL::Origin Extension::origin() const
{
    return URL::Parser::basic_parse(MUST(String::formatted("chrome-extension://{}/", m_id)))->origin();
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

ErrorOr<Extension const*> ExtensionRegistry::load_unpacked(ByteString const& directory)
{
    auto root = TRY(FileSystem::real_path(directory));
    auto manifest_file = TRY(Core::File::open(ByteString::formatted("{}/manifest.json", root), Core::File::OpenMode::Read));
    auto manifest_json = TRY(manifest_file->read_until_eof());
    auto manifest = TRY(WebExtensions::Manifest::parse(manifest_json));

    auto id = manifest.key.has_value()
        ? TRY(WebExtensions::extension_id_from_base64_key(*manifest.key))
        : WebExtensions::extension_id_from_path(root);

    // FIXME: Use the browser's languages.
    auto messages = TRY(WebExtensions::MessageCatalog::load(root, manifest.default_locale.value_or({}), { "en_US"_string }, id));
    auto extension = make<Extension>(id, move(root), move(manifest), move(messages));
    auto const* stored = extension.ptr();
    m_extensions.set(move(id), move(extension));
    m_serialized_for_renderers.clear();
    if (on_change)
        on_change();
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
    m_serialized_for_renderers.clear();
    if (on_change)
        on_change();
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
    if (!real_path.starts_with(ByteString::formatted("{}/", extension->root())))
        return Error::from_errno(EACCES);

    return open_local_file_for_renderer(real_path);
}

}
