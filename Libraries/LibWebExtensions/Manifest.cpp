/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonParser.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibURL/Parser.h>
#include <LibWebExtensions/ExtensionId.h>
#include <LibWebExtensions/Manifest.h>

namespace WebExtensions {

// Messages passed to these must be string literals; Error only keeps a view.
static Error invalid(StringView literal_message)
{
    return Error::from_string_view(literal_message);
}

Optional<String> best_icon_for_size(IconMap const& icons, u32 size)
{
    Optional<u32> best_larger;
    Optional<u32> largest;
    for (auto const& [icon_size, path] : icons) {
        if (icon_size >= size && (!best_larger.has_value() || icon_size < *best_larger))
            best_larger = icon_size;
        if (!largest.has_value() || icon_size > *largest)
            largest = icon_size;
    }
    if (best_larger.has_value())
        return icons.get(*best_larger).value();
    if (largest.has_value())
        return icons.get(*largest).value();
    return {};
}

Optional<String> SuggestedKey::for_platform(StringView platform) const
{
    Optional<String> const* specific = nullptr;
    if (platform == "mac"sv)
        specific = &mac;
    else if (platform == "windows"sv)
        specific = &windows;
    else if (platform == "linux"sv)
        specific = &linux;
    else if (platform == "chromeos"sv)
        specific = &chromeos;
    if (specific && specific->has_value())
        return **specific;
    return default_key;
}

ErrorOr<String> strip_json_comments(StringView input)
{
    if (input.starts_with("\xEF\xBB\xBF"sv))
        input = input.substring_view(3);

    StringBuilder builder;
    size_t i = 0;
    bool in_string = false;
    while (i < input.length()) {
        char c = input[i];
        if (in_string) {
            builder.append(c);
            if (c == '\\' && i + 1 < input.length()) {
                builder.append(input[i + 1]);
                i += 2;
                continue;
            }
            if (c == '"')
                in_string = false;
            ++i;
            continue;
        }
        if (c == '"') {
            in_string = true;
            builder.append(c);
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < input.length() && input[i + 1] == '/') {
            i += 2;
            while (i < input.length() && input[i] != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < input.length() && input[i + 1] == '*') {
            auto end = input.find("*/"sv, i + 2);
            if (!end.has_value())
                return Error::from_string_literal("Unterminated comment in JSON");
            builder.append(' ');
            i = *end + 2;
            continue;
        }
        builder.append(c);
        ++i;
    }
    return builder.to_string();
}

namespace {

// Returns the value as an integer only if it is integral (3 and 3.0, but not 3.5).
Optional<i64> exact_integer(JsonValue const& value)
{
    if (!value.is_number())
        return {};
    auto as_double = value.get_double_with_precision_loss();
    auto as_integer = value.get_i64();
    if (!as_double.has_value() || !as_integer.has_value())
        return {};
    if (static_cast<double>(*as_integer) != *as_double)
        return {};
    return as_integer;
}

ErrorOr<Optional<String>> optional_string(JsonObject const& object, StringView key, StringView error)
{
    auto value = object.get(key);
    if (!value.has_value())
        return Optional<String> {};
    if (!value->is_string())
        return invalid(error);
    return Optional<String> { value->as_string() };
}

ErrorOr<Optional<bool>> optional_bool(JsonObject const& object, StringView key, StringView error)
{
    auto value = object.get(key);
    if (!value.has_value())
        return Optional<bool> {};
    if (!value->is_bool())
        return invalid(error);
    return Optional<bool> { value->as_bool() };
}

ErrorOr<Vector<String>> string_list(JsonValue const& value, StringView error)
{
    if (!value.is_array())
        return invalid(error);
    Vector<String> result;
    for (auto const& item : value.as_array().values()) {
        if (!item.is_string())
            return invalid(error);
        result.append(item.as_string());
    }
    return result;
}

ErrorOr<Optional<Vector<String>>> optional_string_list(JsonObject const& object, StringView key, StringView error)
{
    auto value = object.get(key);
    if (!value.has_value())
        return Optional<Vector<String>> {};
    return Optional<Vector<String>> { TRY(string_list(*value, error)) };
}

ErrorOr<IconMap> parse_icon_map(JsonValue const& value, StringView error)
{
    if (!value.is_object())
        return invalid(error);
    IconMap icons;
    TRY(value.as_object().try_for_each_member([&](String const& key, JsonValue const& path) -> ErrorOr<void> {
        auto size = key.to_number<u32>();
        if (!size.has_value() || !path.is_string() || path.as_string().is_empty())
            return invalid(error);
        icons.set(*size, path.as_string());
        return {};
    }));
    return icons;
}

// Extension "version": 1-4 dot-separated integers, each 0..65535 (base::Version rules).
bool is_valid_version_string(StringView version, size_t max_components, u32 max_value)
{
    if (version.is_empty())
        return false;
    auto parts = version.split_view('.', SplitBehavior::KeepEmpty);
    if (parts.size() > max_components)
        return false;
    for (size_t i = 0; i < parts.size(); ++i) {
        auto part = parts[i];
        if (part.is_empty() || part.length() > 10)
            return false;
        for (auto c : part) {
            if (!is_ascii_digit(c))
                return false;
        }
        if (i == 0 && part.length() > 1 && part[0] == '0')
            return false;
        auto number = part.to_number<u64>();
        if (!number.has_value() || *number > max_value)
            return false;
    }
    return true;
}

bool looks_like_host_pattern(StringView permission)
{
    return permission == "<all_urls>"sv || permission.contains("://"sv);
}

void append_unique(Vector<String>& list, String const& value)
{
    if (!list.contains_slow(value))
        list.append(value);
}

ErrorOr<ActionInfo> parse_action(JsonValue const& value)
{
    if (!value.is_object())
        return invalid("Invalid value for 'action'."sv);
    auto const& object = value.as_object();
    ActionInfo action;
    action.default_popup = TRY(optional_string(object, "default_popup"sv, "Invalid value for 'action.default_popup'."sv));
    action.default_title = TRY(optional_string(object, "default_title"sv, "Invalid value for 'action.default_title'."sv));
    if (auto icon = object.get("default_icon"sv); icon.has_value()) {
        if (icon->is_string()) {
            if (icon->as_string().is_empty())
                return invalid("Invalid value for 'action.default_icon'."sv);
            action.default_icon.set(16, icon->as_string());
        } else {
            action.default_icon = TRY(parse_icon_map(*icon, "Invalid value for 'action.default_icon'."sv));
        }
    }
    return action;
}

ErrorOr<Optional<BackgroundInfo>> parse_background(JsonValue const& value, u8 manifest_version, Vector<String>& warnings)
{
    if (!value.is_object())
        return invalid("Invalid value for 'background'."sv);
    auto const& object = value.as_object();
    BackgroundInfo background;

    auto service_worker = TRY(optional_string(object, "service_worker"sv, "Invalid value for 'background.service_worker'."sv));
    auto scripts = TRY(optional_string_list(object, "scripts"sv, "Invalid value for 'background.scripts'."sv));
    auto page = TRY(optional_string(object, "page"sv, "Invalid value for 'background.page'."sv));
    auto persistent = TRY(optional_bool(object, "persistent"sv, "Invalid value for 'background.persistent'."sv));
    auto type = TRY(optional_string(object, "type"sv, "Invalid value for 'background.type'."sv));

    if (type.has_value()) {
        if (*type == "module"sv)
            background.type = BackgroundInfo::Type::Module;
        else if (*type == "classic"sv)
            background.type = BackgroundInfo::Type::Classic;
        else
            return invalid("Invalid value for 'background.type'."sv);
    }

    if (manifest_version >= 3) {
        if (scripts.has_value() || page.has_value())
            warnings.append("'background.scripts' and 'background.page' require manifest version 2 or lower."_string);
        if (persistent.has_value())
            warnings.append("'background.persistent' requires manifest version 2 or lower."_string);
        if (!service_worker.has_value())
            return Optional<BackgroundInfo> {};
        if (service_worker->is_empty())
            return invalid("Invalid value for 'background.service_worker'."sv);
        background.service_worker = move(service_worker);
        return background;
    }

    if (service_worker.has_value())
        warnings.append("'background.service_worker' requires manifest version 3 or higher."_string);
    if (scripts.has_value() && page.has_value())
        return invalid("The background.page and background.scripts properties cannot be used at the same time."sv);
    if (scripts.has_value())
        background.scripts = scripts.release_value();
    if (page.has_value()) {
        if (page->is_empty())
            return invalid("Invalid value for 'background.page'."sv);
        background.page = move(page);
    }
    background.persistent = persistent;
    return background;
}

ErrorOr<ContentScript> parse_content_script(JsonValue const& value)
{
    if (!value.is_object())
        return invalid("Invalid value for 'content_scripts'."sv);
    auto const& object = value.as_object();
    ContentScript script;

    auto matches = TRY(optional_string_list(object, "matches"sv, "Invalid value for 'content_scripts[].matches'."sv));
    if (!matches.has_value() || matches->is_empty())
        return invalid("Required value 'content_scripts[].matches' is missing or invalid."sv);
    script.matches = matches.release_value();

    script.exclude_matches = TRY(optional_string_list(object, "exclude_matches"sv, "Invalid value for 'content_scripts[].exclude_matches'."sv)).value_or({});
    script.include_globs = TRY(optional_string_list(object, "include_globs"sv, "Invalid value for 'content_scripts[].include_globs'."sv)).value_or({});
    script.exclude_globs = TRY(optional_string_list(object, "exclude_globs"sv, "Invalid value for 'content_scripts[].exclude_globs'."sv)).value_or({});
    script.js = TRY(optional_string_list(object, "js"sv, "Invalid value for 'content_scripts[].js'."sv)).value_or({});
    script.css = TRY(optional_string_list(object, "css"sv, "Invalid value for 'content_scripts[].css'."sv)).value_or({});
    if (script.js.is_empty() && script.css.is_empty())
        return invalid("Required value 'content_scripts[].js' or 'content_scripts[].css' is missing."sv);

    if (auto run_at = TRY(optional_string(object, "run_at"sv, "Invalid value for 'content_scripts[].run_at'."sv)); run_at.has_value()) {
        if (*run_at == "document_start"sv)
            script.run_at = ContentScript::RunAt::DocumentStart;
        else if (*run_at == "document_end"sv)
            script.run_at = ContentScript::RunAt::DocumentEnd;
        else if (*run_at == "document_idle"sv)
            script.run_at = ContentScript::RunAt::DocumentIdle;
        else
            return invalid("Invalid value for 'content_scripts[].run_at'."sv);
    }

    script.all_frames = TRY(optional_bool(object, "all_frames"sv, "Invalid value for 'content_scripts[].all_frames'."sv)).value_or(false);
    script.match_about_blank = TRY(optional_bool(object, "match_about_blank"sv, "Invalid value for 'content_scripts[].match_about_blank'."sv)).value_or(false);
    script.match_origin_as_fallback = TRY(optional_bool(object, "match_origin_as_fallback"sv, "Invalid value for 'content_scripts[].match_origin_as_fallback'."sv)).value_or(false);

    if (auto world = TRY(optional_string(object, "world"sv, "Invalid value for 'content_scripts[].world'."sv)); world.has_value()) {
        if (*world == "ISOLATED"sv)
            script.world = ContentScript::World::Isolated;
        else if (*world == "MAIN"sv)
            script.world = ContentScript::World::Main;
        else
            return invalid("Invalid value for 'content_scripts[].world'."sv);
    }
    return script;
}

ErrorOr<Vector<WebAccessibleResource>> parse_web_accessible_resources(JsonValue const& value, u8 manifest_version)
{
    if (!value.is_array())
        return invalid("Invalid value for 'web_accessible_resources'."sv);

    Vector<WebAccessibleResource> result;
    if (manifest_version < 3) {
        auto resources = TRY(string_list(value, "Invalid value for 'web_accessible_resources'."sv));
        if (!resources.is_empty())
            result.append({ .resources = move(resources), .matches = { "<all_urls>"_string }, .extension_ids = {}, .use_dynamic_url = false });
        return result;
    }

    for (auto const& item : value.as_array().values()) {
        if (!item.is_object())
            return invalid("Invalid value for 'web_accessible_resources[]'. Entries must be objects."sv);
        auto const& object = item.as_object();
        WebAccessibleResource resource;
        auto resources = TRY(optional_string_list(object, "resources"sv, "Invalid value for 'web_accessible_resources[].resources'."sv));
        if (!resources.has_value())
            return invalid("Invalid value for 'web_accessible_resources[]'. Entry must at least have resources, and one other valid key."sv);
        resource.resources = resources.release_value();
        auto matches = TRY(optional_string_list(object, "matches"sv, "Invalid value for 'web_accessible_resources[].matches'."sv));
        auto extension_ids = TRY(optional_string_list(object, "extension_ids"sv, "Invalid value for 'web_accessible_resources[].extension_ids'."sv));
        auto use_dynamic_url = TRY(optional_bool(object, "use_dynamic_url"sv, "Invalid value for 'web_accessible_resources[].use_dynamic_url'."sv));
        if (!matches.has_value() && !extension_ids.has_value() && !use_dynamic_url.has_value())
            return invalid("Invalid value for 'web_accessible_resources[]'. Entry must at least have resources, and one other valid key."sv);
        resource.matches = matches.value_or({});
        resource.extension_ids = extension_ids.value_or({});
        resource.use_dynamic_url = use_dynamic_url.value_or(false);
        result.append(move(resource));
    }
    return result;
}

ErrorOr<Vector<Command>> parse_commands(JsonValue const& value)
{
    if (!value.is_object())
        return invalid("Invalid value for 'commands'."sv);
    Vector<Command> commands;
    TRY(value.as_object().try_for_each_member([&](String const& name, JsonValue const& entry) -> ErrorOr<void> {
        if (!entry.is_object())
            return invalid("Invalid value for 'commands'."sv);
        auto const& object = entry.as_object();
        Command command;
        command.name = name;
        command.description = TRY(optional_string(object, "description"sv, "Invalid value for 'commands[].description'."sv));
        command.global = TRY(optional_bool(object, "global"sv, "Invalid value for 'commands[].global'."sv)).value_or(false);
        if (auto key = object.get("suggested_key"sv); key.has_value()) {
            if (key->is_string()) {
                command.suggested_key.default_key = key->as_string();
            } else if (key->is_object()) {
                auto const& keys = key->as_object();
                auto const error = "Invalid value for 'commands[].suggested_key'."sv;
                command.suggested_key.default_key = TRY(optional_string(keys, "default"sv, error));
                command.suggested_key.mac = TRY(optional_string(keys, "mac"sv, error));
                command.suggested_key.windows = TRY(optional_string(keys, "windows"sv, error));
                command.suggested_key.linux = TRY(optional_string(keys, "linux"sv, error));
                command.suggested_key.chromeos = TRY(optional_string(keys, "chromeos"sv, error));
            } else {
                return invalid("Invalid value for 'commands[].suggested_key'."sv);
            }
        }
        commands.append(move(command));
        return {};
    }));
    return commands;
}

ErrorOr<ExternallyConnectable> parse_externally_connectable(JsonValue const& value)
{
    if (!value.is_object())
        return invalid("Invalid value for 'externally_connectable'."sv);
    auto const& object = value.as_object();
    ExternallyConnectable result;
    result.matches = TRY(optional_string_list(object, "matches"sv, "Invalid value for 'externally_connectable.matches'."sv)).value_or({});
    result.ids = TRY(optional_string_list(object, "ids"sv, "Invalid value for 'externally_connectable.ids'."sv)).value_or({});
    result.accepts_tls_channel_id = TRY(optional_bool(object, "accepts_tls_channel_id"sv, "Invalid value for 'externally_connectable.accepts_tls_channel_id'."sv)).value_or(false);
    return result;
}

ErrorOr<DeclarativeNetRequest> parse_declarative_net_request(JsonValue const& value)
{
    if (!value.is_object())
        return invalid("Invalid value for 'declarative_net_request'."sv);
    DeclarativeNetRequest result;
    auto resources = value.as_object().get("rule_resources"sv);
    if (!resources.has_value())
        return result;
    if (!resources->is_array())
        return invalid("Invalid value for 'declarative_net_request.rule_resources'."sv);

    HashTable<String> seen_ids;
    for (auto const& item : resources->as_array().values()) {
        if (!item.is_object())
            return invalid("Invalid value for 'declarative_net_request.rule_resources[]'."sv);
        auto const& object = item.as_object();
        auto id = TRY(optional_string(object, "id"sv, "Invalid value for 'declarative_net_request.rule_resources[].id'."sv));
        auto enabled = TRY(optional_bool(object, "enabled"sv, "Invalid value for 'declarative_net_request.rule_resources[].enabled'."sv));
        auto path = TRY(optional_string(object, "path"sv, "Invalid value for 'declarative_net_request.rule_resources[].path'."sv));
        if (!id.has_value() || id->is_empty() || id->starts_with('_'))
            return invalid("Invalid value for 'declarative_net_request.rule_resources[].id'."sv);
        if (!enabled.has_value())
            return invalid("Required value 'declarative_net_request.rule_resources[].enabled' is missing."sv);
        if (!path.has_value() || path->is_empty())
            return invalid("Required value 'declarative_net_request.rule_resources[].path' is missing."sv);
        if (seen_ids.set(*id) != HashSetResult::InsertedNewEntry)
            return invalid("Rules file key 'declarative_net_request.rule_resources[].id' has a non-unique value."sv);
        result.rule_resources.append({ .id = id.release_value(), .enabled = *enabled, .path = path.release_value() });
    }
    return result;
}

}

ErrorOr<Manifest> Manifest::parse(StringView input)
{
    if (!Utf8View { input }.validate())
        return Error::from_string_literal("Manifest is not valid UTF-8.");
    auto stripped = TRY(strip_json_comments(input));
    auto json = TRY(JsonParser::parse(stripped));
    if (!json.is_object())
        return Error::from_string_literal("Manifest is not a valid JSON dictionary.");

    Manifest manifest;
    manifest.raw = json.as_object();
    auto const& object = manifest.raw;
    auto& warnings = manifest.warnings;

    // manifest_version
    auto version_value = object.get("manifest_version"sv);
    if (!version_value.has_value())
        return Error::from_string_literal("The 'manifest_version' key must be present and set to 2 or 3 (without quotes).");
    auto manifest_version = exact_integer(*version_value);
    if (!manifest_version.has_value() || *manifest_version < 2 || *manifest_version > 3)
        return Error::from_string_literal("Invalid value for 'manifest_version'. Must be an integer either 2 or 3.");
    manifest.manifest_version = static_cast<u8>(*manifest_version);
    bool is_mv3 = manifest.manifest_version >= 3;

    // name, version
    auto name = TRY(optional_string(object, "name"sv, "Required value 'name' is missing or invalid."sv));
    if (!name.has_value())
        return Error::from_string_literal("Required value 'name' is missing or invalid.");
    manifest.name = TRY(String::from_utf8(name->bytes_as_string_view().trim_whitespace()));
    if (manifest.name.is_empty())
        return Error::from_string_literal("Required value 'name' is missing or invalid.");

    auto version = TRY(optional_string(object, "version"sv, "Required value 'version' is missing or invalid. It must be between 1-4 dot-separated integers each between 0 and 65536."sv));
    if (!version.has_value() || !is_valid_version_string(*version, 4, 65535))
        return Error::from_string_literal("Required value 'version' is missing or invalid. It must be between 1-4 dot-separated integers each between 0 and 65536.");
    manifest.version = version.release_value();

    // Simple strings.
    manifest.short_name = TRY(optional_string(object, "short_name"sv, "Invalid value for 'short_name'."sv));
    manifest.version_name = TRY(optional_string(object, "version_name"sv, "Invalid value for 'version_name'."sv));
    manifest.description = TRY(optional_string(object, "description"sv, "Invalid value for 'description'."sv));

    manifest.default_locale = TRY(optional_string(object, "default_locale"sv, "Invalid value for default locale - locale name must be a string."sv));
    if (manifest.default_locale.has_value() && manifest.default_locale->is_empty())
        return Error::from_string_literal("Invalid value for default locale - locale name must be a string.");

    manifest.key = TRY(optional_string(object, "key"sv, "Invalid value for 'key'."sv));
    if (manifest.key.has_value() && extension_id_from_base64_key(*manifest.key).is_error())
        return Error::from_string_literal("Invalid value for 'key'.");

    manifest.minimum_chrome_version = TRY(optional_string(object, "minimum_chrome_version"sv, "Invalid value for 'minimum_chrome_version'."sv));
    if (manifest.minimum_chrome_version.has_value() && !is_valid_version_string(*manifest.minimum_chrome_version, NumericLimits<size_t>::max(), NumericLimits<u32>::max()))
        return Error::from_string_literal("Invalid value for 'minimum_chrome_version'.");

    // Firefox allows an object here; only keep a string.
    if (auto author = object.get("author"sv); author.has_value() && author->is_string())
        manifest.author = author->as_string();

    if (auto icons = object.get("icons"sv); icons.has_value())
        manifest.icons = TRY(parse_icon_map(*icons, "Invalid value for 'icons'."sv));

    // action / browser_action / page_action
    auto action = object.get("action"sv);
    auto browser_action = object.get("browser_action"sv);
    auto page_action = object.get("page_action"sv);
    if (is_mv3) {
        if (browser_action.has_value() || page_action.has_value())
            warnings.append("'browser_action' and 'page_action' require manifest version 2 or lower."_string);
        if (action.has_value())
            manifest.action = TRY(parse_action(*action));
    } else {
        if (action.has_value())
            warnings.append("'action' requires manifest version 3 or higher."_string);
        if (browser_action.has_value() && page_action.has_value())
            return Error::from_string_literal("Only one of 'browser_action', 'page_action', and 'app' can be specified.");
        if (browser_action.has_value())
            manifest.action = TRY(parse_action(*browser_action));
        else if (page_action.has_value())
            manifest.action = TRY(parse_action(*page_action));
    }

    if (auto background = object.get("background"sv); background.has_value())
        manifest.background = TRY(parse_background(*background, manifest.manifest_version, warnings));

    if (auto content_scripts = object.get("content_scripts"sv); content_scripts.has_value()) {
        if (!content_scripts->is_array())
            return Error::from_string_literal("Invalid value for 'content_scripts'.");
        for (auto const& item : content_scripts->as_array().values())
            manifest.content_scripts.append(TRY(parse_content_script(item)));
    }

    // Permissions.
    auto permissions = TRY(optional_string_list(object, "permissions"sv, "Invalid value for 'permissions'."sv)).value_or({});
    auto optional_permissions = TRY(optional_string_list(object, "optional_permissions"sv, "Invalid value for 'optional_permissions'."sv)).value_or({});
    auto host_permissions = TRY(optional_string_list(object, "host_permissions"sv, "Invalid value for 'host_permissions'."sv)).value_or({});
    auto optional_host_permissions = TRY(optional_string_list(object, "optional_host_permissions"sv, "Invalid value for 'optional_host_permissions'."sv)).value_or({});

    if (!is_mv3 && (object.has("host_permissions"sv) || object.has("optional_host_permissions"sv)))
        warnings.append("'host_permissions' requires manifest version 3 or higher."_string);

    auto split_permissions = [&](Vector<String> const& list, Vector<String>& api, Vector<String>& hosts) {
        for (auto const& permission : list) {
            if (looks_like_host_pattern(permission)) {
                if (is_mv3)
                    warnings.append(MUST(String::formatted("Permission '{}' is unknown or URL pattern is malformed.", permission)));
                else
                    append_unique(hosts, permission);
            } else {
                append_unique(api, permission);
            }
        }
    };
    split_permissions(permissions, manifest.permissions, manifest.host_permissions);
    split_permissions(optional_permissions, manifest.optional_permissions, manifest.optional_host_permissions);
    if (is_mv3) {
        for (auto const& host : host_permissions)
            append_unique(manifest.host_permissions, host);
        for (auto const& host : optional_host_permissions)
            append_unique(manifest.optional_host_permissions, host);
    }

    if (auto resources = object.get("web_accessible_resources"sv); resources.has_value())
        manifest.web_accessible_resources = TRY(parse_web_accessible_resources(*resources, manifest.manifest_version));

    if (auto commands = object.get("commands"sv); commands.has_value())
        manifest.commands = TRY(parse_commands(*commands));

    // content_security_policy: an object in MV3, a string in MV2.
    if (auto csp = object.get("content_security_policy"sv); csp.has_value()) {
        if (is_mv3) {
            if (!csp->is_object())
                return Error::from_string_literal("Invalid value for 'content_security_policy'.");
            manifest.content_security_policy.extension_pages = TRY(optional_string(csp->as_object(), "extension_pages"sv, "Invalid value for 'content_security_policy.extension_pages'."sv));
            manifest.content_security_policy.sandbox = TRY(optional_string(csp->as_object(), "sandbox"sv, "Invalid value for 'content_security_policy.sandbox'."sv));
        } else {
            if (!csp->is_string())
                return Error::from_string_literal("Invalid value for 'content_security_policy'.");
            manifest.content_security_policy.extension_pages = csp->as_string();
        }
    }
    if (auto sandbox = object.get_object("sandbox"sv); sandbox.has_value() && sandbox->has("content_security_policy"sv)) {
        if (is_mv3)
            return Error::from_string_literal("The 'sandbox.content_security_policy' key cannot be used in manifest version 3 and higher.");
        manifest.content_security_policy.sandbox = TRY(optional_string(*sandbox, "content_security_policy"sv, "Invalid value for 'sandbox.content_security_policy'."sv));
    }

    if (auto connectable = object.get("externally_connectable"sv); connectable.has_value())
        manifest.externally_connectable = TRY(parse_externally_connectable(*connectable));

    // Chrome only warns about bad options keys.
    if (auto options_page = object.get("options_page"sv); options_page.has_value()) {
        if (options_page->is_string() && !options_page->as_string().is_empty())
            manifest.options_page = options_page->as_string();
        else
            warnings.append("Invalid value for 'options_page'."_string);
    }
    if (auto options_ui = object.get("options_ui"sv); options_ui.has_value()) {
        auto page = options_ui->is_object() ? options_ui->as_object().get("page"sv) : Optional<JsonValue const&> {};
        auto open_in_tab = options_ui->is_object() ? options_ui->as_object().get("open_in_tab"sv) : Optional<JsonValue const&> {};
        if (!page.has_value() || !page->is_string() || page->as_string().is_empty() || (open_in_tab.has_value() && !open_in_tab->is_bool()))
            warnings.append("Invalid value for 'options_ui'."_string);
        else
            manifest.options_ui = OptionsUI { .page = page->as_string(), .open_in_tab = open_in_tab.has_value() && open_in_tab->as_bool() };
    }

    manifest.devtools_page = TRY(optional_string(object, "devtools_page"sv, "Invalid value for 'devtools_page'."sv));

    manifest.homepage_url = TRY(optional_string(object, "homepage_url"sv, "Invalid value for homepage url."sv));
    if (manifest.homepage_url.has_value()) {
        auto url = URL::Parser::basic_parse(*manifest.homepage_url);
        if (!url.has_value() || (url->scheme() != "http"sv && url->scheme() != "https"sv))
            return Error::from_string_literal("Invalid value for homepage url.");
    }

    if (auto incognito = TRY(optional_string(object, "incognito"sv, "Invalid value for 'incognito'."sv)); incognito.has_value()) {
        if (*incognito == "spanning"sv)
            manifest.incognito = IncognitoMode::Spanning;
        else if (*incognito == "split"sv)
            manifest.incognito = IncognitoMode::Split;
        else if (*incognito == "not_allowed"sv)
            manifest.incognito = IncognitoMode::NotAllowed;
        else
            return Error::from_string_literal("Invalid value for 'incognito'.");
    }

    if (auto dnr = object.get("declarative_net_request"sv); dnr.has_value())
        manifest.declarative_net_request = TRY(parse_declarative_net_request(*dnr));

    if (auto storage = object.get("storage"sv); storage.has_value()) {
        if (!storage->is_object())
            return Error::from_string_literal("Invalid value for 'storage'.");
        manifest.storage = StorageInfo { .managed_schema = TRY(optional_string(storage->as_object(), "managed_schema"sv, "Invalid value for 'storage.managed_schema'."sv)) };
    }

    manifest.offline_enabled = TRY(optional_bool(object, "offline_enabled"sv, "Invalid value for 'offline_enabled'."sv));

    return manifest;
}

StringView Manifest::default_mv3_extension_pages_csp()
{
    return "script-src 'self';"sv;
}

StringView Manifest::default_mv2_extension_pages_csp()
{
    return "script-src 'self' blob: filesystem:; object-src 'self' blob: filesystem:;"sv;
}

StringView Manifest::default_sandbox_csp()
{
    return "sandbox allow-scripts allow-forms allow-popups allow-modals; script-src 'self' 'unsafe-inline' 'unsafe-eval'; child-src 'self';"sv;
}

StringView Manifest::minimum_mv3_extension_pages_csp()
{
    return "script-src 'self' 'wasm-unsafe-eval' 'inline-speculation-rules'; object-src 'self';"sv;
}

String Manifest::extension_pages_csp() const
{
    if (content_security_policy.extension_pages.has_value())
        return *content_security_policy.extension_pages;
    return MUST(String::from_utf8(manifest_version >= 3 ? default_mv3_extension_pages_csp() : default_mv2_extension_pages_csp()));
}

String Manifest::sandbox_csp() const
{
    if (content_security_policy.sandbox.has_value())
        return *content_security_policy.sandbox;
    return MUST(String::from_utf8(default_sandbox_csp()));
}

bool Manifest::has_permission(StringView permission) const
{
    return permissions.contains_slow(permission) || host_permissions.contains_slow(permission);
}

bool Manifest::has_optional_permission(StringView permission) const
{
    return optional_permissions.contains_slow(permission) || optional_host_permissions.contains_slow(permission);
}

Optional<Command const&> Manifest::command(StringView name) const
{
    for (auto const& command : commands) {
        if (command.name == name)
            return command;
    }
    return {};
}

}
