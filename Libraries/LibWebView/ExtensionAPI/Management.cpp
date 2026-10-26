/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibURL/Parser.h>
#include <LibWebExtensions/MatchPattern.h>
#include <LibWebView/Application.h>
#include <LibWebView/ExtensionAPI/Management.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/Settings.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

using Context = ExtensionRuntime::Context;
using Reply = ExtensionRuntime::Reply;
using Result = ErrorOr<JsonValue, String>;

NonnullOwnPtr<ExtensionAPI> create_management_api(ExtensionRuntime& runtime)
{
    return make<ManagementAPI>(runtime);
}

static Result error(StringView message)
{
    return MUST(String::from_utf8(message));
}

template<typename... Parameters>
static Result error(CheckedFormatString<Parameters...>&& format, Parameters const&... parameters)
{
    return MUST(String::formatted(move(format), parameters...));
}

static JsonArray to_json(Vector<String> const& strings)
{
    JsonArray array;
    for (auto const& string : strings)
        array.must_append(string);
    return array;
}

static void append_unique(Vector<String>& strings, String const& string)
{
    if (!strings.contains_slow(string))
        strings.append(string);
}

// Whether a host permission pattern covers all a requested pattern matches (paths don't matter for hosts).
static bool pattern_covers(WebExtensions::MatchPattern const& granted, WebExtensions::MatchPattern const& requested)
{
    if (granted.matches_all_urls())
        return requested.matches_all_urls() || requested.scheme().is_one_of("*"sv, "http"sv, "https"sv, "file"sv, "ftp"sv, "ws"sv, "wss"sv);
    if (requested.matches_all_urls())
        return false;

    if (granted.scheme() != requested.scheme() && !(granted.scheme() == "*"sv && requested.scheme().is_one_of("http"sv, "https"sv)))
        return false;
    if (requested.scheme() == "file"sv)
        return true;

    if (granted.port().has_value() && granted.port() != requested.port())
        return false;

    if (granted.matches_subdomains() && granted.host().is_empty())
        return true;
    if (requested.matches_subdomains() && requested.host().is_empty())
        return false;
    if (requested.host() == granted.host())
        return !requested.matches_subdomains() || granted.matches_subdomains();
    if (!granted.matches_subdomains())
        return false;
    auto host = requested.host().bytes_as_string_view();
    auto suffix = granted.host().bytes_as_string_view();
    return host.length() > suffix.length() && host.ends_with(suffix) && host[host.length() - suffix.length() - 1] == '.';
}

static bool patterns_cover(Vector<String> const& granted, WebExtensions::MatchPattern const& requested)
{
    for (auto const& pattern : granted) {
        auto parsed = WebExtensions::MatchPattern::parse(pattern);
        if (!parsed.is_error() && pattern_covers(parsed.value(), requested))
            return true;
    }
    return false;
}

// A permissions.Permissions argument: { permissions?: string[], origins?: string[] }.
struct PermissionSet {
    Vector<String> permissions;
    Vector<String> origins;
    Vector<WebExtensions::MatchPattern> patterns;
};

static ErrorOr<PermissionSet, String> parse_permission_set(JsonArray const& arguments)
{
    if (arguments.is_empty() || !arguments.at(0).is_object())
        return "A permissions object is needed: { permissions, origins }"_string;
    auto const& object = arguments.at(0).as_object();
    PermissionSet set;
    auto read_list = [&](StringView key, Vector<String>& list) -> ErrorOr<void, String> {
        auto value = object.get(key);
        if (!value.has_value() || value->is_null())
            return {};
        if (!value->is_array())
            return MUST(String::formatted("'{}' must be a list of strings", key));
        for (auto const& item : value->as_array().values()) {
            if (!item.is_string())
                return MUST(String::formatted("'{}' must be a list of strings", key));
            append_unique(list, item.as_string());
        }
        return {};
    };
    TRY(read_list("permissions"sv, set.permissions));
    TRY(read_list("origins"sv, set.origins));
    for (auto const& origin : set.origins) {
        auto pattern = WebExtensions::MatchPattern::parse(origin);
        if (pattern.is_error())
            return MUST(String::formatted("Invalid value for origin pattern {}: {}", origin, pattern.error()));
        set.patterns.append(pattern.release_value());
    }
    return set;
}

static JsonObject permission_set_json(Vector<String> const& permissions, Vector<String> const& origins)
{
    JsonObject object;
    object.set("permissions"sv, to_json(permissions));
    object.set("origins"sv, to_json(origins));
    return object;
}

static bool is_content_script(Context const& context)
{
    return context.type == "content_script"sv;
}

ManagementAPI::ManagementAPI(ExtensionRuntime& runtime)
    : ExtensionAPI(runtime)
{
    register_permissions_functions();
    register_management_functions();
    register_privacy_functions();
    register_runtime_functions();
}

ManagementAPI::~ManagementAPI() = default;

// State: <storage>/<id>/management.json.
ManagementAPI::State& ManagementAPI::state(StringView extension_id) const
{
    return *m_states.ensure(MUST(String::from_utf8(extension_id)), [&] {
        auto state = make<State>();
        auto const& directory = runtime().storage_directory();
        if (!directory.has_value())
            return state;
        auto path = LexicalPath::join(*directory, extension_id, "management.json"sv).string();
        auto file = Core::File::open(path, Core::File::OpenMode::Read);
        if (file.is_error())
            return state;
        auto contents = file.value()->read_until_eof();
        if (contents.is_error())
            return state;
        auto json = JsonParser::parse(contents.value());
        if (json.is_error() || !json.value().is_object()) {
            dbgln("Extensions: Can't read {}'s management state", extension_id);
            return state;
        }
        auto const& object = json.value().as_object();
        auto read_list = [&](StringView key, Vector<String>& list) {
            if (auto array = object.get_array(key); array.has_value()) {
                for (auto const& item : array->values()) {
                    if (item.is_string())
                        append_unique(list, item.as_string());
                }
            }
        };
        read_list("permissions"sv, state->granted_permissions);
        read_list("origins"sv, state->granted_origins);
        if (auto url = object.get_string("uninstall_url"sv); url.has_value())
            state->uninstall_url = URL::Parser::basic_parse(*url);
        if (auto privacy = object.get_object("privacy"sv); privacy.has_value()) {
            privacy->for_each_member([&](String const& name, JsonValue const& entry) {
                if (!entry.is_object() || !entry.as_object().has("value"sv))
                    return;
                auto sequence = entry.as_object().get_u64("sequence"sv).value_or(0);
                state->privacy.set(name, { entry.as_object().get("value"sv).value(), sequence });
                m_next_privacy_sequence = max(m_next_privacy_sequence, sequence + 1);
            });
        }
        return state;
    });
}

void ManagementAPI::save_state(StringView extension_id) const
{
    auto const& directory = runtime().storage_directory();
    if (!directory.has_value())
        return;
    auto const& state = this->state(extension_id);

    JsonObject object;
    object.set("permissions"sv, to_json(state.granted_permissions));
    object.set("origins"sv, to_json(state.granted_origins));
    if (state.uninstall_url.has_value())
        object.set("uninstall_url"sv, state.uninstall_url->serialize());
    JsonObject privacy;
    for (auto const& [name, entry] : state.privacy) {
        JsonObject value;
        value.set("value"sv, entry.value);
        value.set("sequence"sv, entry.sequence);
        privacy.set(name, move(value));
    }
    object.set("privacy"sv, move(privacy));

    auto extension_directory = LexicalPath::join(*directory, extension_id).string();
    auto saved = [&]() -> ErrorOr<void> {
        TRY(Core::Directory::create(extension_directory, Core::Directory::CreateDirectories::Yes));
        auto file = TRY(Core::File::open(LexicalPath::join(extension_directory, "management.json"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        auto text = object.serialized();
        TRY(file->write_until_depleted(text.bytes()));
        return {};
    }();
    if (saved.is_error())
        dbgln("Extensions: Can't save {}'s management state: {}", extension_id, saved.error());
}

Vector<String> ManagementAPI::api_permissions(Extension const& extension) const
{
    auto permissions = extension.manifest().permissions;
    for (auto const& permission : state(extension.id()).granted_permissions) {
        if (extension.manifest().optional_permissions.contains_slow(permission))
            append_unique(permissions, permission);
    }
    return permissions;
}

Vector<String> ManagementAPI::host_permissions(Extension const& extension) const
{
    auto hosts = extension.manifest().host_permissions;
    for (auto const& script : extension.manifest().content_scripts) {
        for (auto const& match : script.matches)
            append_unique(hosts, match);
    }
    for (auto const& origin : state(extension.id()).granted_origins)
        append_unique(hosts, origin);
    return hosts;
}

bool ManagementAPI::has_permission(StringView extension_id, StringView permission) const
{
    auto const* extension = runtime().registry().find(extension_id);
    if (!extension)
        return false;
    return api_permissions(*extension).contains_slow(permission);
}

bool ManagementAPI::has_host_permission(StringView extension_id, URL::URL const& url) const
{
    auto const* extension = runtime().registry().find(extension_id);
    if (!extension)
        return false;
    for (auto const& pattern : host_permissions(*extension)) {
        auto parsed = WebExtensions::MatchPattern::parse(pattern);
        if (!parsed.is_error() && parsed.value().matches_ignoring_path(url))
            return true;
    }
    return false;
}

Vector<String> ManagementAPI::host_permission_patterns(StringView extension_id) const
{
    auto const* extension = runtime().registry().find(extension_id);
    if (!extension)
        return {};
    return host_permissions(*extension);
}

bool ManagementAPI::is_enabled(StringView extension_id) const
{
    return runtime().registry().find(extension_id) != nullptr;
}

Optional<URL::URL> ManagementAPI::uninstall_url(StringView extension_id) const
{
    return state(extension_id).uninstall_url;
}

// chrome.privacy: each setting has Chrome's default, unless an extension controls it.
namespace {

enum class PrivacyType : u8 {
    Boolean,
    WebRTCPolicy,
};

struct PrivacySetting {
    StringView name;
    PrivacyType type;
    JsonValue default_value;
};

}

static Vector<PrivacySetting> const& privacy_settings()
{
    static auto& settings = *new Vector<PrivacySetting>([] {
        Vector<PrivacySetting> settings;
        auto boolean = [&](StringView name, bool value) { settings.append({ name, PrivacyType::Boolean, JsonValue { value } }); };
        boolean("network.networkPredictionEnabled"sv, true);
        settings.append({ "network.webRTCIPHandlingPolicy"sv, PrivacyType::WebRTCPolicy, JsonValue { "default"_string } });
        boolean("services.alternateErrorPagesEnabled"sv, true);
        boolean("services.autofillAddressEnabled"sv, true);
        boolean("services.autofillCreditCardEnabled"sv, true);
        boolean("services.autofillEnabled"sv, true);
        boolean("services.passwordSavingEnabled"sv, true);
        boolean("services.safeBrowsingEnabled"sv, true);
        boolean("services.safeBrowsingExtendedReportingEnabled"sv, false);
        boolean("services.searchSuggestEnabled"sv, false); // The browser's own setting, really.
        boolean("services.spellingServiceEnabled"sv, false);
        boolean("services.translationServiceEnabled"sv, true);
        boolean("websites.thirdPartyCookiesAllowed"sv, true);
        boolean("websites.hyperlinkAuditingEnabled"sv, true);
        boolean("websites.referrersEnabled"sv, true);
        boolean("websites.doNotTrackEnabled"sv, false);
        boolean("websites.protectedContentEnabled"sv, true);
        return settings;
    }());
    return settings;
}

// chrome.permissions
void ManagementAPI::register_permissions_functions()
{
    runtime().register_function("permissions.getAll"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        auto const* extension = runtime().registry().find(context.extension_id);
        if (!extension || is_content_script(context))
            return reply(error("permissions.getAll isn't available here"sv));
        reply(JsonValue { permission_set_json(api_permissions(*extension), host_permissions(*extension)) });
    });

    runtime().register_function("permissions.contains"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        auto const* extension = runtime().registry().find(context.extension_id);
        if (!extension || is_content_script(context))
            return reply(error("permissions.contains isn't available here"sv));
        auto set = parse_permission_set(arguments);
        if (set.is_error())
            return reply(set.release_error());
        auto permissions = api_permissions(*extension);
        auto hosts = host_permissions(*extension);
        for (auto const& permission : set.value().permissions) {
            if (!permissions.contains_slow(permission))
                return reply(JsonValue { false });
        }
        for (auto const& pattern : set.value().patterns) {
            if (!patterns_cover(hosts, pattern))
                return reply(JsonValue { false });
        }
        reply(JsonValue { true });
    });

    // Chrome lets a request through only during a user gesture. One question at a time; after the user says no, a
    // request for any of what they refused is refused for a while without asking (a deviation: Chrome asks again).
    runtime().register_function("permissions.request"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        auto const* extension = runtime().registry().find(context.extension_id);
        if (!extension || is_content_script(context))
            return reply(error("permissions.request isn't available here"sv));
        auto set = parse_permission_set(arguments);
        if (set.is_error())
            return reply(set.release_error());
        auto const& manifest = extension->manifest();

        auto declared_hosts = manifest.host_permissions;
        declared_hosts.extend(manifest.optional_host_permissions);
        for (auto const& permission : set.value().permissions) {
            if (!manifest.permissions.contains_slow(permission) && !manifest.optional_permissions.contains_slow(permission))
                return reply(error("Only permissions specified in the manifest may be requested."sv));
        }
        for (auto const& pattern : set.value().patterns) {
            if (!patterns_cover(declared_hosts, pattern))
                return reply(error("Only permissions specified in the manifest may be requested."sv));
        }

        // Only what isn't granted yet needs asking.
        auto permissions = api_permissions(*extension);
        auto hosts = host_permissions(*extension);
        Vector<String> new_permissions;
        Vector<String> new_origins;
        for (auto const& permission : set.value().permissions) {
            if (!permissions.contains_slow(permission))
                new_permissions.append(permission);
        }
        for (size_t i = 0; i < set.value().patterns.size(); ++i) {
            if (!patterns_cover(hosts, set.value().patterns[i]))
                new_origins.append(set.value().origins[i]);
        }
        if (new_permissions.is_empty() && new_origins.is_empty())
            return reply(JsonValue { true });
        if (m_asking_for_permissions.contains(extension->id()))
            return reply(error("A permission request is already showing"sv));
        if (!runtime().consume_user_activation(context))
            return reply(error("This function must be called during a user gesture"sv));
        if (auto denial = m_denials.find(extension->id()); denial != m_denials.end()) {
            if (MonotonicTime::now() >= denial->value.until) {
                m_denials.remove(denial);
            } else if (any_of(new_permissions, [&](auto const& permission) { return denial->value.permissions.contains_slow(permission); })
                || any_of(new_origins, [&](auto const& origin) { return denial->value.origins.contains_slow(origin); })) {
                return reply(JsonValue { false });
            }
        }
        if (!runtime().request_permissions_prompt)
            return reply(JsonValue { false });

        m_asking_for_permissions.set(extension->id());
        runtime().request_permissions_prompt(*extension, new_permissions, new_origins, [this, id = extension->id(), new_permissions, new_origins, reply = move(reply)](bool granted) mutable {
            m_asking_for_permissions.remove(id);
            if (!granted) {
                static constexpr auto denial_cooldown = AK::Duration::from_seconds(10);
                m_denials.set(id, Denial { new_permissions, new_origins, MonotonicTime::now() + denial_cooldown });
            }
            if (!granted || !runtime().registry().find(id))
                return reply(JsonValue { false });
            auto& state = this->state(id);
            for (auto const& permission : new_permissions)
                append_unique(state.granted_permissions, permission);
            for (auto const& origin : new_origins)
                append_unique(state.granted_origins, origin);
            save_state(id);
            runtime().dispatch_event(id, "permissions.onAdded"sv, JsonArray { Vector<JsonValue> { permission_set_json(new_permissions, new_origins) } });
            if (!new_origins.is_empty())
                runtime().host_access_changed(id);
            reply(JsonValue { true });
        });
    });

    runtime().register_function("permissions.remove"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        auto const* extension = runtime().registry().find(context.extension_id);
        if (!extension || is_content_script(context))
            return reply(error("permissions.remove isn't available here"sv));
        auto set = parse_permission_set(arguments);
        if (set.is_error())
            return reply(set.release_error());
        auto const& manifest = extension->manifest();

        for (auto const& permission : set.value().permissions) {
            if (manifest.permissions.contains_slow(permission))
                return reply(error("You cannot remove required permissions."sv));
            if (!manifest.optional_permissions.contains_slow(permission))
                return reply(error("Only permissions specified in the manifest may be removed."sv));
        }
        for (auto const& pattern : set.value().patterns) {
            if (patterns_cover(manifest.host_permissions, pattern))
                return reply(error("You cannot remove required permissions."sv));
            if (!patterns_cover(manifest.optional_host_permissions, pattern))
                return reply(error("Only permissions specified in the manifest may be removed."sv));
        }

        auto& state = this->state(extension->id());
        Vector<String> removed_permissions;
        Vector<String> removed_origins;
        state.granted_permissions.remove_all_matching([&](auto const& permission) {
            if (!set.value().permissions.contains_slow(permission))
                return false;
            removed_permissions.append(permission);
            return true;
        });
        state.granted_origins.remove_all_matching([&](auto const& origin) {
            auto granted = WebExtensions::MatchPattern::parse(origin);
            if (granted.is_error())
                return false;
            for (auto const& pattern : set.value().patterns) {
                if (pattern_covers(pattern, granted.value())) {
                    removed_origins.append(origin);
                    return true;
                }
            }
            return false;
        });
        if (!removed_permissions.is_empty() || !removed_origins.is_empty()) {
            save_state(extension->id());
            runtime().dispatch_event(extension->id(), "permissions.onRemoved"sv, JsonArray { Vector<JsonValue> { permission_set_json(removed_permissions, removed_origins) } });
            if (!removed_origins.is_empty())
                runtime().host_access_changed(extension->id());
        }
        reply(JsonValue { true });
    });

    // Chrome shows these requests in its toolbar; there is nowhere to show them yet, so they're only checked.
    for (auto name : { "permissions.addHostAccessRequest"sv, "permissions.removeHostAccessRequest"sv }) {
        runtime().register_function(name, [name](Context const& context, JsonArray const& arguments, Reply reply) {
            if (is_content_script(context))
                return reply(error("{} isn't available here", name));
            if (arguments.is_empty() || !arguments.at(0).is_object())
                return reply(error("{} needs a request object", name));
            auto const& request = arguments.at(0).as_object();
            auto tab_id = request.get("tabId"sv);
            auto document_id = request.get("documentId"sv);
            bool has_tab = tab_id.has_value() && !tab_id->is_null();
            bool has_document = document_id.has_value() && !document_id->is_null();
            if (has_tab == has_document)
                return reply(error("Must specify either 'documentId' or 'tabId'."sv));
            if (has_tab && (!tab_id->is_integer<i64>() || tab_id->as_integer<i64>() < 0))
                return reply(error("Invalid 'tabId'."sv));
            if (has_document && !document_id->is_string())
                return reply(error("Invalid 'documentId'."sv));
            if (auto pattern = request.get("pattern"sv); pattern.has_value() && !pattern->is_null()) {
                if (!pattern->is_string() || WebExtensions::MatchPattern::parse(pattern->as_string()).is_error())
                    return reply(error("Invalid 'pattern'."sv));
            }
            reply(JsonValue {});
        });
    }
}

// chrome.management
JsonObject ManagementAPI::extension_info(Extension const& extension) const
{
    auto const& manifest = extension.manifest();
    auto const& messages = extension.messages();
    auto base = MUST(String::formatted("chrome-extension://{}/", extension.id()));
    auto url_of = [&](StringView path) {
        auto trimmed = path.trim("/"sv, TrimMode::Left);
        return MUST(String::formatted("{}{}", base, trimmed));
    };

    JsonObject info;
    info.set("id"sv, extension.id());
    auto name = messages.substitute_manifest_messages(manifest.name);
    info.set("name"sv, name);
    info.set("shortName"sv, manifest.short_name.has_value() ? messages.substitute_manifest_messages(*manifest.short_name) : name);
    info.set("description"sv, manifest.description.has_value() ? messages.substitute_manifest_messages(*manifest.description) : String {});
    info.set("version"sv, manifest.version);
    if (manifest.version_name.has_value())
        info.set("versionName"sv, *manifest.version_name);
    info.set("mayDisable"sv, true);
    info.set("mayEnable"sv, true);
    bool enabled = is_enabled(extension.id());
    info.set("enabled"sv, enabled);
    if (!enabled)
        info.set("disabledReason"sv, "unknown"sv);
    info.set("isApp"sv, false);
    info.set("type"sv, "extension"sv);
    // As the registry installed it; one only loaded is the user's (or, with an update_url, the Web Store's).
    if (auto installed = runtime().registry().installed_extension(extension.id()); installed.has_value())
        info.set("installType"sv, installed->install_type_name());
    else
        info.set("installType"sv, manifest.raw.has("update_url"sv) ? "normal"sv : "development"sv);
    if (manifest.homepage_url.has_value())
        info.set("homepageUrl"sv, *manifest.homepage_url);
    info.set("offlineEnabled"sv, manifest.offline_enabled.value_or(false));
    if (manifest.options_ui.has_value())
        info.set("optionsUrl"sv, url_of(manifest.options_ui->page));
    else if (manifest.options_page.has_value())
        info.set("optionsUrl"sv, url_of(*manifest.options_page));
    else
        info.set("optionsUrl"sv, String {});

    Vector<u32> sizes;
    for (auto const& [size, path] : manifest.icons)
        sizes.append(size);
    quick_sort(sizes);
    if (!sizes.is_empty()) {
        JsonArray icons;
        for (auto size : sizes) {
            JsonObject icon;
            icon.set("size"sv, size);
            icon.set("url"sv, url_of(manifest.icons.get(size).value()));
            icons.must_append(move(icon));
        }
        info.set("icons"sv, move(icons));
    }
    info.set("permissions"sv, to_json(api_permissions(extension)));
    info.set("hostPermissions"sv, to_json(host_permissions(extension)));
    return info;
}

void ManagementAPI::dispatch_to_managers(StringView event, JsonValue const& argument, Optional<StringView> skip)
{
    Vector<String> managers;
    runtime().registry().for_each_extension([&](Extension const& extension) {
        if ((!skip.has_value() || extension.id() != *skip) && api_permissions(extension).contains_slow("management"sv))
            managers.append(extension.id());
    });
    for (auto const& id : managers)
        runtime().dispatch_event(id, event, JsonArray { Vector<JsonValue> { argument } });
}

// What's known of an installed extension the registry hasn't loaded, as it's disabled.
JsonObject ManagementAPI::disabled_extension_info(StringView id) const
{
    auto installed = runtime().registry().installed_extension(id);
    JsonObject info;
    info.set("id"sv, id);
    info.set("name"sv, installed.has_value() ? installed->name : String {});
    info.set("shortName"sv, installed.has_value() ? installed->name : String {});
    info.set("description"sv, String {});
    info.set("version"sv, installed.has_value() ? installed->version : String {});
    info.set("mayDisable"sv, true);
    info.set("mayEnable"sv, true);
    info.set("enabled"sv, false);
    info.set("disabledReason"sv, "unknown"sv);
    info.set("isApp"sv, false);
    info.set("type"sv, "extension"sv);
    info.set("installType"sv, installed.has_value() ? installed->install_type_name() : "development"sv);
    info.set("offlineEnabled"sv, false);
    info.set("optionsUrl"sv, String {});
    info.set("permissions"sv, JsonArray {});
    info.set("hostPermissions"sv, JsonArray {});
    return info;
}

Optional<JsonObject> ManagementAPI::info_of(StringView id) const
{
    if (auto const* extension = runtime().registry().find(id))
        return extension_info(*extension);
    if (runtime().registry().installed_extension(id).has_value())
        return disabled_extension_info(id);
    return {};
}

void ManagementAPI::open_url(URL::URL const& url)
{
    if (open_url_in_new_tab)
        open_url_in_new_tab(url);
    else
        Application::the().open_url_in_new_tab(url, Web::HTML::ActivateTab::Yes);
}

// management.onInstalled for what's newly loaded with a new version; onUninstalled (and the uninstall URL) for what
// the registry no longer knows at all.
void ManagementAPI::extensions_changed()
{
    HashMap<String, String> loaded;
    HashTable<String> known;
    runtime().registry().for_each_extension([&](Extension const& extension) {
        loaded.set(extension.id(), extension.manifest().version);
        known.set(extension.id());
    });
    for (auto const& installed : runtime().registry().installed_extensions())
        known.set(installed.id);

    for (auto const& [id, version] : loaded) {
        auto previous = m_loaded_versions.get(id);
        if (previous.has_value() && *previous == version)
            continue;
        if (!previous.has_value() && m_known_extensions.contains(id))
            continue; // Enabled again.
        if (auto const* extension = runtime().registry().find(id))
            dispatch_to_managers("management.onInstalled"sv, extension_info(*extension), id.bytes_as_string_view());
    }
    for (auto const& id : m_known_extensions) {
        if (known.contains(id))
            continue;
        dispatch_to_managers("management.onUninstalled"sv, JsonValue { id });
        if (auto url = state(id).uninstall_url; url.has_value())
            open_url(*url);
        m_states.remove(id);
    }
    m_loaded_versions = move(loaded);
    m_known_extensions = move(known);
}

void ManagementAPI::register_management_functions()
{
    runtime().register_function("management.getSelf"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        auto const* extension = runtime().registry().find(context.extension_id);
        if (!extension || is_content_script(context))
            return reply(error("management.getSelf isn't available here"sv));
        reply(JsonValue { extension_info(*extension) });
    });

    // Without the "management" permission, an extension sees only itself.
    runtime().register_function("management.getAll"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        auto const* self = runtime().registry().find(context.extension_id);
        if (!self || is_content_script(context))
            return reply(error("management.getAll isn't available here"sv));
        JsonArray all;
        if (!has_permission(context.extension_id, "management"sv)) {
            all.must_append(extension_info(*self));
            return reply(JsonValue { move(all) });
        }
        runtime().registry().for_each_extension([&](Extension const& extension) {
            all.must_append(extension_info(extension));
        });
        reply(JsonValue { move(all) });
    });

    // The extension asked for, when the caller may see it: its ID.
    auto find_target = [this](Context const& context, JsonArray const& arguments, StringView function) -> ErrorOr<Optional<String>, String> {
        if (is_content_script(context))
            return MUST(String::formatted("{} isn't available here", function));
        if (arguments.is_empty() || !arguments.at(0).is_string())
            return MUST(String::formatted("{} needs an extension ID", function));
        auto const& id = arguments.at(0).as_string();
        if (id != context.extension_id && !has_permission(context.extension_id, "management"sv))
            return MUST(String::formatted("{} needs the \"management\" permission", function));
        if (!info_of(id).has_value())
            return MUST(String::formatted("Failed to find extension with id {}.", id));
        return Optional<String> { id };
    };

    runtime().register_function("management.get"sv, [this, find_target](Context const& context, JsonArray const& arguments, Reply reply) {
        auto id = find_target(context, arguments, "management.get"sv);
        if (id.is_error())
            return reply(id.release_error());
        reply(JsonValue { info_of(*id.value()).release_value() });
    });

    // The registry enables and disables: a disabled extension stays installed, but isn't loaded or run.
    runtime().register_function("management.setEnabled"sv, [this, find_target](Context const& context, JsonArray const& arguments, Reply reply) {
        if (!has_permission(context.extension_id, "management"sv))
            return reply(error("management.setEnabled needs the \"management\" permission"sv));
        if (!arguments.is_empty() && arguments.at(0).is_string() && arguments.at(0).as_string() == context.extension_id)
            return reply(error("Cannot change the enabled state of the calling extension."sv));
        if (arguments.size() < 2 || !arguments.at(1).is_bool())
            return reply(error("management.setEnabled needs an extension ID and whether it's enabled"sv));
        auto id = find_target(context, arguments, "management.setEnabled"sv);
        if (id.is_error())
            return reply(id.release_error());
        bool enabled = arguments.at(1).as_bool();
        if (is_enabled(*id.value()) == enabled)
            return reply(JsonValue {});

        // FIXME: Chrome asks the user before enabling an extension whose permissions grew.
        // A disabled extension's privacy settings no longer count, and an enabled one's count again.
        HashMap<StringView, JsonValue> previous_values;
        HashMap<StringView, Optional<String>> previous_controllers;
        for (auto const& setting : privacy_settings()) {
            previous_values.set(setting.name, privacy_setting(setting.name));
            previous_controllers.set(setting.name, privacy_controller(setting.name));
        }
        if (auto result = runtime().registry().set_enabled(*id.value(), enabled); result.is_error())
            return reply(error("Can't change whether {} is enabled: {}", *id.value(), result.error()));
        for (auto const& [name, controller] : previous_controllers)
            privacy_setting_changed(name, previous_values.get(name).value(), controller);

        if (auto info = info_of(*id.value()); info.has_value())
            dispatch_to_managers(enabled ? "management.onEnabled"sv : "management.onDisabled"sv, info.release_value(), id.value()->bytes_as_string_view());
        reply(JsonValue {});
    });

    // The uninstall options at the index: { showConfirmDialog }.
    auto confirm_dialog = [](JsonArray const& arguments, size_t index, bool fallback) -> ErrorOr<bool, String> {
        if (arguments.size() <= index || arguments.at(index).is_null())
            return fallback;
        if (!arguments.at(index).is_object())
            return "The uninstall options must be an object: { showConfirmDialog }"_string;
        auto confirm = arguments.at(index).as_object().get("showConfirmDialog"sv);
        if (!confirm.has_value() || confirm->is_null())
            return fallback;
        if (!confirm->is_bool())
            return "showConfirmDialog must be a boolean"_string;
        return confirm->as_bool();
    };

    runtime().register_function("management.uninstallSelf"sv, [this, confirm_dialog](Context const& context, JsonArray const& arguments, Reply reply) {
        if (is_content_script(context) || !runtime().registry().find(context.extension_id))
            return reply(error("management.uninstallSelf isn't available here"sv));
        auto show = confirm_dialog(arguments, 0, false);
        if (show.is_error())
            return reply(show.release_error());
        uninstall(context.extension_id, show.value(), move(reply), {});
    });

    // Another extension's uninstall is always the user's to confirm.
    runtime().register_function("management.uninstall"sv, [this, find_target, confirm_dialog](Context const& context, JsonArray const& arguments, Reply reply) {
        if (!has_permission(context.extension_id, "management"sv))
            return reply(error("management.uninstall needs the \"management\" permission"sv));
        auto id = find_target(context, arguments, "management.uninstall"sv);
        if (id.is_error())
            return reply(id.release_error());
        bool self = *id.value() == context.extension_id;
        auto show = confirm_dialog(arguments, 1, !self);
        if (show.is_error())
            return reply(show.release_error());
        // As Chrome, only during the user's gesture.
        if (!runtime().consume_user_activation(context))
            return reply(error("chrome.management.uninstall requires a user gesture."sv));
        uninstall(id.release_value().release_value(), show.value() || !self, move(reply), self ? Optional<String> {} : Optional<String> { context.extension_id });
    });
}

void ManagementAPI::uninstall(String id, bool show_confirm_dialog, Reply reply, Optional<String> requester_id)
{
    auto const* extension = runtime().registry().find(id);
    // One question at a time from each extension.
    auto asker = requester_id.value_or(id);
    if (show_confirm_dialog && runtime().confirm_uninstall && extension && m_asking_to_uninstall.contains(asker))
        return reply(error("The user is already being asked to remove an extension."sv));

    auto finish = [this, id, reply = move(reply)](bool confirmed) mutable {
        if (!confirmed)
            return reply(error("User cancelled uninstall of extension."sv));
        if (auto result = runtime().registry().uninstall(id); result.is_error())
            return reply(error("Can't uninstall {}: {}", id, result.error()));
        reply(JsonValue {});
    };
    if (!show_confirm_dialog)
        return finish(true);
    if (!runtime().confirm_uninstall || !extension)
        return finish(false);
    auto const* requester = requester_id.has_value() ? runtime().registry().find(*requester_id) : nullptr;
    m_asking_to_uninstall.set(asker);
    runtime().confirm_uninstall(*extension, requester, [this, asker, finish = move(finish)](bool confirmed) mutable {
        m_asking_to_uninstall.remove(asker);
        finish(confirmed);
    });
}

static PrivacySetting const* find_privacy_setting(StringView name)
{
    for (auto const& setting : privacy_settings()) {
        if (setting.name == name)
            return &setting;
    }
    return nullptr;
}

static constexpr StringView search_suggestions_setting = "services.searchSuggestEnabled"sv;

Optional<String> ManagementAPI::privacy_controller(StringView name) const
{
    Optional<String> controller;
    u64 latest = 0;
    runtime().registry().for_each_extension([&](Extension const& extension) {
        auto const& state = this->state(extension.id());
        if (!extension.manifest().has_permission("privacy"sv) && !state.granted_permissions.contains_slow("privacy"sv))
            return;
        auto entry = state.privacy.get(name);
        if (entry.has_value() && entry->sequence >= latest) {
            latest = entry->sequence;
            controller = extension.id();
        }
    });
    return controller;
}

JsonValue ManagementAPI::privacy_setting(StringView name) const
{
    auto const* setting = find_privacy_setting(name);
    if (!setting)
        return {};
    if (auto controller = privacy_controller(name); controller.has_value())
        return state(*controller).privacy.get(name)->value;
    if (name == search_suggestions_setting)
        return JsonValue { Application::settings().search_engine_settings().suggestions };
    return setting->default_value;
}

JsonObject ManagementAPI::privacy_details(StringView name, Extension const& viewer) const
{
    JsonObject details;
    details.set("value"sv, privacy_setting(name));
    auto controller = privacy_controller(name);
    StringView level;
    if (!has_permission(viewer.id(), "privacy"sv))
        level = "not_controllable"sv;
    else if (controller == viewer.id())
        level = "controlled_by_this_extension"sv;
    else if (controller.has_value())
        level = "controlled_by_other_extensions"sv;
    else
        level = "controllable_by_this_extension"sv;
    details.set("levelOfControl"sv, level);
    return details;
}

void ManagementAPI::privacy_setting_changed(StringView name, JsonValue const& previous_value, Optional<String> const& previous_controller)
{
    auto value = privacy_setting(name);
    auto controller = privacy_controller(name);
    if (previous_value.serialized() == value.serialized() && previous_controller == controller)
        return;

    apply_privacy_setting_to_browser(name);

    auto event = MUST(String::formatted("privacy.{}.onChange", name));
    runtime().registry().for_each_extension([&](Extension const& extension) {
        if (has_permission(extension.id(), "privacy"sv))
            runtime().dispatch_event(extension.id(), event, JsonArray { Vector<JsonValue> { privacy_details(name, extension) } });
    });
    if (on_privacy_setting_changed)
        on_privacy_setting_changed(name);
}

// What Elytra itself has: search suggestions. While an extension controls them, the user's own value waits in
// <storage>/privacy.json, to come back once no extension does. The rest (Elytra has no password manager, autofill,
// Safe Browsing, ...) is only kept, for privacy_setting() and on_privacy_setting_changed.
void ManagementAPI::apply_privacy_setting_to_browser(StringView name)
{
    if (name != search_suggestions_setting)
        return;

    Optional<ByteString> path;
    if (runtime().storage_directory().has_value())
        path = LexicalPath::join(*runtime().storage_directory(), "privacy.json"sv).string();
    if (!m_user_search_suggestions.has_value() && path.has_value()) {
        if (auto file = Core::File::open(*path, Core::File::OpenMode::Read); !file.is_error()) {
            if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
                auto json = JsonParser::parse(contents.value());
                if (!json.is_error() && json.value().is_object())
                    m_user_search_suggestions = json.value().as_object().get_bool("user_search_suggestions"sv);
            }
        }
    }

    auto& settings = Application::settings();
    auto search = settings.search_engine_settings();
    auto controller = privacy_controller(name);
    if (controller.has_value()) {
        if (!m_user_search_suggestions.has_value())
            m_user_search_suggestions = search.suggestions;
        search.suggestions = state(*controller).privacy.get(name)->value.as_bool();
    } else if (m_user_search_suggestions.has_value()) {
        search.suggestions = *m_user_search_suggestions;
        m_user_search_suggestions.clear();
    }
    if (search.suggestions != settings.search_engine_settings().suggestions)
        settings.set_search_engine_settings(move(search));

    if (!path.has_value())
        return;
    JsonObject saved;
    if (m_user_search_suggestions.has_value())
        saved.set("user_search_suggestions"sv, *m_user_search_suggestions);
    auto result = [&]() -> ErrorOr<void> {
        TRY(Core::Directory::create(*runtime().storage_directory(), Core::Directory::CreateDirectories::Yes));
        auto file = TRY(Core::File::open(*path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        auto text = saved.serialized();
        TRY(file->write_until_depleted(text.bytes()));
        return {};
    }();
    if (result.is_error())
        dbgln("Extensions: Can't save the user's privacy settings: {}", result.error());
}

static constexpr auto incognito_error = "You do not have permission to access incognito preferences."sv;

// The scope of a set or clear: only the regular profile's, as no extension has incognito access.
static Optional<String> check_scope(JsonObject const& details)
{
    auto scope = details.get("scope"sv);
    if (!scope.has_value() || scope->is_null())
        return {};
    if (!scope->is_string() || !scope->as_string().is_one_of("regular"sv, "regular_only"sv, "incognito_persistent"sv, "incognito_session_only"sv))
        return "Invalid scope: it's one of \"regular\", \"regular_only\", \"incognito_persistent\" or \"incognito_session_only\""_string;
    if (scope->as_string().starts_with_bytes("incognito"sv))
        return MUST(String::from_utf8(incognito_error));
    return {};
}

void ManagementAPI::register_privacy_functions()
{
    for (auto const& setting : privacy_settings()) {
        auto name = setting.name;
        auto function = [name](StringView which) { return MUST(String::formatted("privacy.{}.{}", name, which)); };

        runtime().register_function(function("get"sv), [this, name](Context const& context, JsonArray const& arguments, Reply reply) {
            auto const* extension = runtime().registry().find(context.extension_id);
            if (!extension || is_content_script(context))
                return reply(error("chrome.privacy isn't available here"sv));
            if (!arguments.is_empty() && !arguments.at(0).is_null()) {
                if (!arguments.at(0).is_object())
                    return reply(error("The details must be an object: { incognito }"sv));
                auto incognito = arguments.at(0).as_object().get("incognito"sv);
                if (incognito.has_value() && !incognito->is_null() && !incognito->is_bool())
                    return reply(error("incognito must be a boolean"sv));
                if (incognito.has_value() && incognito->is_bool() && incognito->as_bool())
                    return reply(error(incognito_error));
            }
            reply(JsonValue { privacy_details(name, *extension) });
        });

        auto change = [this, name](Context const& context, JsonArray const& arguments, bool is_set, Reply reply) {
            auto const* extension = runtime().registry().find(context.extension_id);
            if (!extension || is_content_script(context))
                return reply(error("chrome.privacy isn't available here"sv));
            if (!has_permission(context.extension_id, "privacy"sv))
                return reply(error("Changing privacy settings needs the \"privacy\" permission"sv));
            if (arguments.is_empty() || !arguments.at(0).is_object())
                return reply(error(is_set ? "The details must be an object: { value, scope }"sv : "The details must be an object: { scope }"sv));
            auto const& details = arguments.at(0).as_object();
            if (auto scope_error = check_scope(details); scope_error.has_value())
                return reply(scope_error.release_value());

            Optional<JsonValue> value;
            if (is_set) {
                if (auto given = details.get("value"sv); given.has_value())
                    value = *given;
                auto const& setting = *find_privacy_setting(name);
                if (!value.has_value())
                    return reply(error("A value is needed"sv));
                if (setting.type == PrivacyType::Boolean && !value->is_bool())
                    return reply(error("The value must be a boolean"sv));
                if (setting.type == PrivacyType::WebRTCPolicy && (!value->is_string() || !value->as_string().is_one_of("default"sv, "default_public_and_private_interfaces"sv, "default_public_interface_only"sv, "disable_non_proxied_udp"sv)))
                    return reply(error("The value must be one of \"default\", \"default_public_and_private_interfaces\", \"default_public_interface_only\" or \"disable_non_proxied_udp\""sv));
            }

            auto previous_value = privacy_setting(name);
            auto previous_controller = privacy_controller(name);
            auto& state = this->state(extension->id());
            if (value.has_value())
                state.privacy.set(MUST(String::from_utf8(name)), { value.release_value(), m_next_privacy_sequence++ });
            else
                state.privacy.remove(name);
            save_state(extension->id());
            privacy_setting_changed(name, previous_value, previous_controller);
            reply(JsonValue {});
        };

        runtime().register_function(function("set"sv), [change](Context const& context, JsonArray const& arguments, Reply reply) {
            change(context, arguments, true, move(reply));
        });
        runtime().register_function(function("clear"sv), [change](Context const& context, JsonArray const& arguments, Reply reply) {
            change(context, arguments, false, move(reply));
        });
    }
}

// The rest of chrome.runtime, chrome.extension and chrome.storage.managed.
static StringView context_type_name(Context const& context)
{
    if (context.type == "background"sv)
        return "BACKGROUND"sv;
    if (context.type == "popup"sv)
        return "POPUP"sv;
    return "TAB"sv;
}

void ManagementAPI::register_runtime_functions()
{
    // FIXME: Window IDs, once there are windows.
    runtime().register_function("runtime.getContexts"sv, [this](Context const& caller, JsonArray const& arguments, Reply reply) {
        if (is_content_script(caller))
            return reply(error("runtime.getContexts isn't available here"sv));
        JsonObject filter;
        if (!arguments.is_empty() && !arguments.at(0).is_null()) {
            if (!arguments.at(0).is_object())
                return reply(error("runtime.getContexts needs a filter object"sv));
            filter = arguments.at(0).as_object();
        }
        for (auto key : { "contextIds"sv, "contextTypes"sv, "documentIds"sv, "documentOrigins"sv, "documentUrls"sv, "frameIds"sv, "tabIds"sv, "windowIds"sv }) {
            if (auto value = filter.get(key); value.has_value() && !value->is_null() && !value->is_array())
                return reply(error("The filter's {} must be a list", key));
        }
        if (auto incognito = filter.get("incognito"sv); incognito.has_value() && !incognito->is_null() && !incognito->is_bool())
            return reply(error("The filter's incognito must be a boolean"sv));

        auto passes = [&](StringView key, JsonValue const& value) {
            auto list = filter.get_array(key);
            if (!list.has_value())
                return true;
            for (auto const& item : list->values()) {
                if (item.serialized() == value.serialized())
                    return true;
            }
            return false;
        };

        JsonArray contexts;
        runtime().for_each_context([&](Context const& context) {
            if (context.extension_id != caller.extension_id || is_content_script(context))
                return;
            bool is_background = context.type == "background"sv;
            JsonObject info;
            info.set("contextId"sv, MUST(String::formatted("00000000-0000-4000-8000-{:012x}", context.id)));
            info.set("contextType"sv, context_type_name(context));
            i64 tab_id = -1;
            if (!is_background && context.page && !runtime().is_background_page(*context.page))
                tab_id = static_cast<i64>(context.page->view().view_id());
            info.set("tabId"sv, tab_id);
            info.set("windowId"sv, -1);
            info.set("frameId"sv, is_background ? -1 : context.is_top_level ? 0 : static_cast<i64>(context.id));
            if (!is_background) {
                info.set("documentId"sv, MUST(String::formatted("{:032X}", context.id)));
                info.set("documentUrl"sv, context.url.serialize());
                info.set("documentOrigin"sv, context.url.origin().serialize());
            }
            info.set("incognito"sv, false);

            static constexpr Array<StringView, 16> keys_and_fields { "contextIds"sv, "contextId"sv, "contextTypes"sv, "contextType"sv, "documentIds"sv, "documentId"sv, "documentOrigins"sv, "documentOrigin"sv, "documentUrls"sv, "documentUrl"sv, "frameIds"sv, "frameId"sv, "tabIds"sv, "tabId"sv, "windowIds"sv, "windowId"sv };
            for (size_t i = 0; i < keys_and_fields.size(); i += 2) {
                if (!passes(keys_and_fields[i], info.get(keys_and_fields[i + 1]).value_or(JsonValue {})))
                    return;
            }
            if (auto incognito = filter.get_bool("incognito"sv); incognito.has_value() && *incognito)
                return;
            contexts.must_append(move(info));
        });
        reply(JsonValue { move(contexts) });
    });

    runtime().register_function("runtime.openOptionsPage"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        auto const* extension = runtime().registry().find(context.extension_id);
        if (!extension || is_content_script(context))
            return reply(error("runtime.openOptionsPage isn't available here"sv));
        auto const& manifest = extension->manifest();
        Optional<String> page;
        if (manifest.options_ui.has_value())
            page = manifest.options_ui->page;
        else if (manifest.options_page.has_value())
            page = manifest.options_page;
        if (!page.has_value())
            return reply(error("Could not create an options page."sv));
        auto url = URL::Parser::basic_parse(MUST(String::formatted("chrome-extension://{}/{}", extension->id(), page->bytes_as_string_view().trim("/"sv, TrimMode::Left))));
        if (!url.has_value())
            return reply(error("Could not create an options page."sv));
        reply(JsonValue {});
        if (runtime().open_options_page)
            runtime().open_options_page(*extension, *url);
        else
            open_url(*url);
    });

    runtime().register_function("runtime.setUninstallURL"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        if (is_content_script(context) || !runtime().registry().find(context.extension_id))
            return reply(error("runtime.setUninstallURL isn't available here"sv));
        if (arguments.is_empty() || !arguments.at(0).is_string())
            return reply(error("runtime.setUninstallURL needs a URL"sv));
        auto const& string = arguments.at(0).as_string();
        auto& state = this->state(context.extension_id);
        if (string.is_empty()) {
            state.uninstall_url.clear();
        } else {
            if (string.bytes().size() > 1023)
                return reply(error("URL is too long."sv));
            auto url = URL::Parser::basic_parse(string);
            if (!url.has_value() || !url->scheme().is_one_of("http"sv, "https"sv))
                return reply(error("Invalid URL: \"{}\".", string));
            state.uninstall_url = url.release_value();
        }
        save_state(context.extension_id);
        reply(JsonValue {});
    });

    // FIXME: Ask the Web Store, once extensions are updated from it.
    runtime().register_function("runtime.requestUpdateCheck"sv, [](Context const& context, JsonArray const&, Reply reply) {
        if (is_content_script(context))
            return reply(error("runtime.requestUpdateCheck isn't available here"sv));
        JsonObject result;
        result.set("status"sv, "no_update"sv);
        reply(JsonValue { move(result) });
    });

    runtime().register_function("runtime.reload"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        if (is_content_script(context))
            return reply(error("runtime.reload isn't available here"sv));
        auto id = context.extension_id;
        reply(JsonValue {});
        runtime().reload_extension(id);
    });

    runtime().register_function("runtime.restart"sv, [](Context const&, JsonArray const&, Reply reply) {
        reply(error("Function available only for ChromeOS kiosk mode."sv));
    });

    runtime().register_function("extension.isAllowedIncognitoAccess"sv, [](Context const& context, JsonArray const&, Reply reply) {
        if (is_content_script(context))
            return reply(error("extension.isAllowedIncognitoAccess isn't available here"sv));
        // FIXME: Private windows don't run extensions yet.
        reply(JsonValue { false });
    });

    runtime().register_function("extension.isAllowedFileSchemeAccess"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        if (is_content_script(context))
            return reply(error("extension.isAllowedFileSchemeAccess isn't available here"sv));
        reply(JsonValue { runtime().registry().allows_file_access(context.extension_id) });
    });

    // chrome.storage.managed is what policy sets, and there's no policy: always empty, never changing.
    runtime().register_function("storage.managed.get"sv, [](Context const&, JsonArray const&, Reply reply) {
        reply(JsonValue { JsonObject {} });
    });
    runtime().register_function("storage.managed.getKeys"sv, [](Context const&, JsonArray const&, Reply reply) {
        reply(JsonValue { JsonArray {} });
    });
    runtime().register_function("storage.managed.getBytesInUse"sv, [](Context const&, JsonArray const&, Reply reply) {
        reply(JsonValue { 0 });
    });
}

}
