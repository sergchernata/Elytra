/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibURL/Parser.h>
#include <LibWebView/ExtensionAPI/Action.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>

namespace WebView {

NonnullOwnPtr<ExtensionAPI> create_action_api(ExtensionRuntime& runtime)
{
    return make<ActionAPI>(runtime);
}

using Context = ExtensionRuntime::Context;
using Reply = ExtensionRuntime::Reply;

// The details object most action functions take: {tabId?, ...}.
static Optional<JsonObject> details_in(JsonArray const& arguments)
{
    if (arguments.is_empty() || arguments.at(0).is_null())
        return JsonObject {};
    if (!arguments.at(0).is_object())
        return {};
    return arguments.at(0).as_object();
}

// A tab ID, as an argument: a non-negative integer, or nothing.
static ErrorOr<Optional<i64>, String> tab_id_in(JsonValue const& value)
{
    if (value.is_null())
        return Optional<i64> {};
    auto id = value.get_integer<i64>();
    if (!id.has_value() || *id < 0)
        return "Invalid tab ID"_string;
    return Optional<i64> { *id };
}

static ErrorOr<Optional<i64>, String> tab_id_of(JsonObject const& details)
{
    return tab_id_in(details.get("tabId"sv).value_or(JsonValue {}));
}

// A color: a CSS color string, or [red, green, blue, alpha] of 0-255.
static Optional<Color> color_in(JsonValue const& value)
{
    if (value.is_string())
        return Color::from_string(value.as_string());
    if (!value.is_array() || value.as_array().size() != 4)
        return {};
    u8 components[4];
    for (size_t i = 0; i < 4; ++i) {
        auto component = value.as_array().at(i).get_integer<i64>();
        if (!component.has_value() || *component < 0 || *component > 255)
            return {};
        components[i] = static_cast<u8>(*component);
    }
    return Color(components[0], components[1], components[2], components[3]);
}

static JsonArray color_array(Color color)
{
    JsonArray array;
    array.must_append(color.red());
    array.must_append(color.green());
    array.must_append(color.blue());
    array.must_append(color.alpha());
    return array;
}

// A path in the extension, as a manifest or setPopup gives one: relative, or a URL of the extension's own.
static Optional<String> extension_path(StringView extension_id, StringView path)
{
    if (path.is_empty())
        return String {};
    auto own_prefix = MUST(String::formatted("chrome-extension://{}/", extension_id));
    if (path.starts_with(own_prefix))
        path = path.substring_view(own_prefix.bytes().size());
    else if (path.contains(':'))
        return {};
    path = path.trim("/"sv, TrimMode::Left);
    if (path.is_empty() || path.contains('\\') || path.contains('\0'))
        return {};
    for (auto part : path.split_view('/')) {
        if (part == ".."sv || part == "."sv)
            return {};
    }
    return MUST(String::from_utf8(path));
}

ActionAPI::ActionAPI(ExtensionRuntime& extension_runtime)
    : ExtensionAPI(extension_runtime)
{
    // A function for the extension's pages and background, for an extension with a button.
    auto add = [this](StringView name, Function<void(Context const&, JsonArray const&, Reply)> handler) {
        runtime().register_function(name, [this, handler = move(handler)](Context const& context, JsonArray const& arguments, Reply reply) {
            if (context.type == "content_script"sv)
                return reply("Content scripts can't use chrome.action"_string);
            auto const* extension = runtime().registry().find(context.extension_id);
            if (!extension || !extension->manifest().action.has_value())
                return reply("The extension has no action"_string);
            handler(context, arguments, move(reply));
        });
    };

    // Setting: {value key: ..., tabId?}.
    auto add_setter = [this, &add](StringView name, StringView key, Function<ErrorOr<void, String>(Values&, JsonValue const&, StringView extension_id)> set) {
        add(name, [this, key, set = move(set)](Context const& context, JsonArray const& arguments, Reply reply) {
            auto details = details_in(arguments);
            if (!details.has_value())
                return reply("Expected an object of details"_string);
            auto tab_id = tab_id_of(*details);
            if (tab_id.is_error())
                return reply(tab_id.release_error());
            auto& extension_state = state(context.extension_id);
            auto& values = tab_id.value().has_value() ? extension_state.tabs.ensure(*tab_id.value()) : extension_state.global;
            if (auto result = set(values, details->get(key).value_or(JsonValue {}), context.extension_id); result.is_error())
                return reply(result.release_error());
            changed(context.extension_id);
            reply(JsonValue {});
        });
    };

    // Getting: {tabId?}.
    auto add_getter = [this, &add](StringView name, Function<JsonValue(State const&)> get) {
        add(name, [this, get = move(get)](Context const& context, JsonArray const& arguments, Reply reply) {
            auto details = details_in(arguments);
            if (!details.has_value())
                return reply("Expected an object of details"_string);
            auto tab_id = tab_id_of(*details);
            if (tab_id.is_error())
                return reply(tab_id.release_error());
            auto current = state_for(context.extension_id, tab_id.value());
            if (!current.has_value())
                return reply("The extension has no action"_string);
            reply(get(*current));
        });
    };

    add_setter("action.setTitle"sv, "title"sv, [](Values& values, JsonValue const& title, StringView) -> ErrorOr<void, String> {
        if (!title.is_string())
            return "The title must be a string"_string;
        values.title = title.as_string();
        return {};
    });
    add_getter("action.getTitle"sv, [](State const& state) { return JsonValue { state.title }; });

    add_setter("action.setPopup"sv, "popup"sv, [](Values& values, JsonValue const& popup, StringView extension_id) -> ErrorOr<void, String> {
        if (!popup.is_string())
            return "The popup must be a string"_string;
        auto path = extension_path(extension_id, popup.as_string());
        if (!path.has_value())
            return "The popup must be a page of the extension"_string;
        values.popup = path.release_value();
        return {};
    });
    add_getter("action.getPopup"sv, [](State const& state) { return JsonValue { state.popup.has_value() ? state.popup->serialize() : String {} }; });

    add_setter("action.setBadgeText"sv, "text"sv, [](Values& values, JsonValue const& text, StringView) -> ErrorOr<void, String> {
        if (text.is_null()) {
            values.badge_text.clear();
            return {};
        }
        if (!text.is_string())
            return "The badge text must be a string"_string;
        values.badge_text = text.as_string();
        return {};
    });
    add_getter("action.getBadgeText"sv, [](State const& state) { return JsonValue { state.badge_text }; });

    add_setter("action.setBadgeBackgroundColor"sv, "color"sv, [](Values& values, JsonValue const& value, StringView) -> ErrorOr<void, String> {
        auto color = color_in(value);
        if (!color.has_value())
            return "Invalid color"_string;
        values.badge_background_color = *color;
        return {};
    });
    add_getter("action.getBadgeBackgroundColor"sv, [](State const& state) {
        return JsonValue { color_array(state.badge_background_color.value_or(Color(0, 0, 0, 0))) };
    });

    add_setter("action.setBadgeTextColor"sv, "color"sv, [](Values& values, JsonValue const& value, StringView) -> ErrorOr<void, String> {
        auto color = color_in(value);
        if (!color.has_value())
            return "Invalid color"_string;
        values.badge_text_color = *color;
        return {};
    });
    add_getter("action.getBadgeTextColor"sv, [](State const& state) {
        return JsonValue { color_array(state.badge_text_color.value_or(Color(0, 0, 0, 0))) };
    });

    add_setter("action.setIcon"sv, "path"sv, [](Values& values, JsonValue const& path, StringView extension_id) -> ErrorOr<void, String> {
        HashMap<u32, String> paths;
        auto add_path = [&](u32 size, JsonValue const& value) -> ErrorOr<void, String> {
            if (!value.is_string())
                return "Icon paths must be strings"_string;
            auto resolved = extension_path(extension_id, value.as_string());
            if (!resolved.has_value() || resolved->is_empty())
                return "Icons must be files of the extension"_string;
            paths.set(size, resolved.release_value());
            return {};
        };
        if (path.is_string()) {
            TRY(add_path(16, path));
        } else if (path.is_object()) {
            TRY(path.as_object().try_for_each_member([&](String const& size, JsonValue const& value) -> ErrorOr<void, String> {
                auto pixels = size.to_number<u32>();
                if (!pixels.has_value() || *pixels == 0 || *pixels > 1024)
                    return "Icon sizes must be numbers of pixels"_string;
                return add_path(*pixels, value);
            }));
        } else {
            // FIXME: imageData.
            return "Only icon paths are supported"_string;
        }
        values.icon_paths = move(paths);
        return {};
    });

    auto add_enabler = [this, &add](StringView name, bool enabled) {
        add(name, [this, enabled](Context const& context, JsonArray const& arguments, Reply reply) {
            auto tab_id = tab_id_in(arguments.is_empty() ? JsonValue {} : arguments.at(0));
            if (tab_id.is_error())
                return reply(tab_id.release_error());
            auto& extension_state = state(context.extension_id);
            (tab_id.value().has_value() ? extension_state.tabs.ensure(*tab_id.value()) : extension_state.global).enabled = enabled;
            changed(context.extension_id);
            reply(JsonValue {});
        });
    };
    add_enabler("action.enable"sv, true);
    add_enabler("action.disable"sv, false);
    add("action.isEnabled"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        auto tab_id = tab_id_in(arguments.is_empty() ? JsonValue {} : arguments.at(0));
        if (tab_id.is_error())
            return reply(tab_id.release_error());
        reply(JsonValue { state_for(context.extension_id, tab_id.value())->enabled });
    });

    add("action.getUserSettings"sv, [](Context const&, JsonArray const&, Reply reply) {
        JsonObject settings;
        settings.set("isOnToolbar"sv, true);
        reply(JsonValue { move(settings) });
    });

    add("action.openPopup"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        Optional<i64> window_id;
        if (auto details = details_in(arguments); details.has_value())
            window_id = details->get_integer<i64>("windowId"sv);
        auto current = state_for(context.extension_id);
        if (!current.has_value() || !current->popup.has_value())
            return reply("The extension has no popup"_string);
        if (!m_delegate)
            return reply("Popups can't be opened here"_string);
        auto result = m_delegate->open_popup(context.extension_id, *current->popup, window_id, ExtensionActionDelegate::ByUser::No);
        if (result.is_error())
            return reply(result.release_error());
        reply(JsonValue {});
    });

    runtime().register_function("commands.getAll"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        if (context.type == "content_script"sv)
            return reply("Content scripts can't use chrome.commands"_string);
        JsonArray commands;
        for (auto const& command : commands_for(context.extension_id)) {
            JsonObject entry;
            entry.set("name"sv, command.name);
            entry.set("shortcut"sv, command.shortcut);
            entry.set("description"sv, command.description);
            commands.must_append(move(entry));
        }
        reply(JsonValue { move(commands) });
    });
}

ActionAPI::ExtensionState& ActionAPI::state(StringView extension_id)
{
    return m_states.ensure(MUST(String::from_utf8(extension_id)));
}

void ActionAPI::changed(StringView extension_id)
{
    if (m_delegate)
        m_delegate->action_changed(extension_id);
}

void ActionAPI::extensions_changed()
{
    // A replaced extension starts again from its manifest.
    m_states.remove_all_matching([&](auto const& id, auto const&) { return !runtime().registry().find(id); });
    runtime().registry().for_each_extension([&](Extension const& extension) {
        changed(extension.id());
    });
}

Optional<ActionAPI::State> ActionAPI::state_for(StringView extension_id, Optional<i64> tab_id) const
{
    auto const* extension = runtime().registry().find(extension_id);
    if (!extension || !extension->manifest().action.has_value())
        return {};
    auto const& defaults = *extension->manifest().action;

    State state;
    state.title = defaults.default_title.value_or(extension->manifest().name);
    Optional<String> popup = defaults.default_popup;
    for (auto const& [size, path] : defaults.default_icon)
        state.icon_paths.set(size, path);
    if (state.icon_paths.is_empty()) {
        for (auto const& [size, path] : extension->manifest().icons)
            state.icon_paths.set(size, path);
    }

    auto apply = [&](Values const& values) {
        if (values.title.has_value())
            state.title = *values.title;
        if (values.popup.has_value())
            popup = *values.popup;
        if (values.badge_text.has_value())
            state.badge_text = *values.badge_text;
        if (values.badge_background_color.has_value())
            state.badge_background_color = values.badge_background_color;
        if (values.badge_text_color.has_value())
            state.badge_text_color = values.badge_text_color;
        if (values.enabled.has_value())
            state.enabled = *values.enabled;
        if (values.icon_paths.has_value())
            state.icon_paths = *values.icon_paths;
    };
    if (auto it = m_states.find(extension_id); it != m_states.end()) {
        apply(it->value.global);
        if (tab_id.has_value()) {
            if (auto tab = it->value.tabs.find(*tab_id); tab != it->value.tabs.end())
                apply(tab->value);
        }
    }

    if (popup.has_value() && !popup->is_empty()) {
        auto path = extension_path(extension_id, *popup);
        if (path.has_value() && !path->is_empty())
            state.popup = URL::Parser::basic_parse(MUST(String::formatted("chrome-extension://{}/{}", extension_id, *path)));
    }
    return state;
}

static JsonValue tab_object(Optional<i64> tab_id)
{
    if (!tab_id.has_value())
        return JsonValue {};
    JsonObject tab;
    tab.set("id"sv, *tab_id);
    return tab;
}

void ActionAPI::clicked(StringView extension_id, Optional<i64> tab_id, Optional<i64> window_id)
{
    auto current = state_for(extension_id, tab_id);
    if (!current.has_value() || !current->enabled)
        return;
    if (current->popup.has_value()) {
        if (m_delegate)
            (void)m_delegate->open_popup(extension_id, *current->popup, window_id, ExtensionActionDelegate::ByUser::Yes);
        return;
    }
    runtime().dispatch_event(extension_id, "action.onClicked"sv, JsonArray { Vector<JsonValue> { tab_object(tab_id) } });
}

// A suggested key as Chrome accepts it: Ctrl, Alt, Command or MacCtrl (and maybe Shift) with a key, or a media key
// alone. Anything else gets no key.
static bool is_valid_shortcut(StringView shortcut)
{
    if (shortcut.is_empty())
        return false;
    auto parts = shortcut.split_view('+');
    auto key = parts.take_last();
    if (key.starts_with("Media"sv))
        return parts.is_empty() && key.is_one_of("MediaNextTrack"sv, "MediaPrevTrack"sv, "MediaPlayPause"sv, "MediaStop"sv);

    bool is_letter_or_digit = key.length() == 1 && (is_ascii_upper_alpha(key[0]) || is_ascii_digit(key[0]));
    if (!is_letter_or_digit && !key.is_one_of("Comma"sv, "Period"sv, "Home"sv, "End"sv, "PageUp"sv, "PageDown"sv, "Space"sv, "Insert"sv, "Delete"sv, "Up"sv, "Down"sv, "Left"sv, "Right"sv))
        return false;

    bool has_primary_modifier = false;
    bool has_ctrl = false;
    bool has_alt = false;
    HashTable<StringView> seen;
    for (auto modifier : parts) {
        if (seen.set(modifier) != HashSetResult::InsertedNewEntry)
            return false;
        if (modifier == "Ctrl"sv || modifier == "Command"sv)
            has_ctrl = true;
        else if (modifier == "Alt"sv)
            has_alt = true;
        else if (modifier != "MacCtrl"sv && modifier != "Shift"sv)
            return false;
        if (modifier != "Shift"sv)
            has_primary_modifier = true;
    }
    // Chrome refuses Ctrl+Alt, which some keyboards use to type characters.
    return has_primary_modifier && !(has_ctrl && has_alt);
}

// Chrome gives keys to at most four of an extension's commands.
static constexpr size_t MAXIMUM_COMMANDS_WITH_KEYS = 4;

Vector<ActionAPI::Command> ActionAPI::commands_for(StringView extension_id) const
{
    Vector<Command> commands;
    auto const* extension = runtime().registry().find(extension_id);
    if (!extension)
        return commands;
#if defined(AK_OS_MACOS)
    auto platform = "mac"sv;
#elif defined(AK_OS_WINDOWS)
    auto platform = "windows"sv;
#else
    auto platform = "linux"sv;
#endif
    size_t with_keys = 0;
    for (auto const& command : extension->manifest().commands) {
        auto shortcut = command.suggested_key.for_platform(platform).value_or({});
        if (!is_valid_shortcut(shortcut) || with_keys == MAXIMUM_COMMANDS_WITH_KEYS)
            shortcut = {};
        else
            ++with_keys;
        commands.append({
            .name = command.name,
            .shortcut = move(shortcut),
            .description = command.description.value_or({}),
        });
    }
    return commands;
}

void ActionAPI::command(StringView extension_id, StringView name, Optional<i64> tab_id, Optional<i64> window_id)
{
    if (!any_of(commands_for(extension_id), [&](auto const& command) { return command.name == name; }))
        return;
    // Chrome's _execute_action works the button; it isn't a command the extension hears of.
    if (name == "_execute_action"sv)
        return clicked(extension_id, tab_id, window_id);
    runtime().dispatch_event(extension_id, "commands.onCommand"sv, JsonArray { Vector<JsonValue> { JsonValue { MUST(String::from_utf8(name)) }, tab_object(tab_id) } });
}

}
