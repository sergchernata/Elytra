/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/QuickSort.h>
#include <AK/Utf8View.h>
#include <LibWebView/ExtensionAPI/ContextMenus.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

NonnullOwnPtr<ExtensionAPI> create_context_menus_api(ExtensionRuntime& runtime)
{
    return make<ContextMenusAPI>(runtime);
}

using Context = ExtensionRuntime::Context;
using Reply = ExtensionRuntime::Reply;

static constexpr size_t maximum_items_per_extension = 1000;
static constexpr size_t maximum_title_length = 4096;
// Chrome shows this many of an extension's items in its button's menu, and ignores the rest.
static constexpr size_t action_menu_top_level_limit = 6;
// And this much of the selection, for %s.
static constexpr size_t maximum_selection_length = 75;

static constexpr Array valid_contexts { "all"sv, "page"sv, "frame"sv, "selection"sv, "link"sv, "editable"sv, "image"sv, "video"sv, "audio"sv, "launcher"sv, "browser_action"sv, "page_action"sv, "action"sv };

static bool is_action_context(StringView context)
{
    return context == "action"sv || context == "browser_action"sv || context == "page_action"sv;
}

// An item ID as the extension gives one: a string or an integer.
static Optional<JsonValue> id_in(JsonValue const& value)
{
    if (value.is_string() && value.as_string().bytes().size() <= 1024)
        return value;
    if (value.is_number()) {
        if (auto integer = value.get_integer<i64>(); integer.has_value())
            return JsonValue { *integer };
    }
    return {};
}

static String key_of(JsonValue const& id)
{
    return id.serialized();
}

ContextMenusAPI::ContextMenusAPI(ExtensionRuntime& extension_runtime)
    : ExtensionAPI(extension_runtime)
{
    // For the extension's pages and background, with the "contextMenus" permission.
    auto add = [this](StringView name, Function<void(Context const&, JsonArray const&, Reply)> handler) {
        runtime().register_function(name, [this, handler = move(handler)](Context const& context, JsonArray const& arguments, Reply reply) {
            if (context.type == "content_script"sv)
                return reply("Content scripts can't use chrome.contextMenus"_string);
            auto const* extension = runtime().registry().find(context.extension_id);
            if (!extension || !extension->manifest().has_permission("contextMenus"sv))
                return reply("chrome.contextMenus needs the \"contextMenus\" permission"_string);
            handler(context, arguments, move(reply));
        });
    };

    // Applies create's or update's properties to an item; on failure, the item is as it was.
    auto apply = [this](StringView extension_id, Item& item, JsonObject const& properties) -> ErrorOr<void, String> {
        Item updated = item;

        if (auto type = properties.get("type"sv); type.has_value() && !type->is_null()) {
            if (!type->is_string() || !first_is_one_of(type->as_string(), "normal"sv, "checkbox"sv, "radio"sv, "separator"sv))
                return "Invalid item type"_string;
            updated.type = type->as_string();
        }
        if (auto title = properties.get("title"sv); title.has_value() && !title->is_null()) {
            if (!title->is_string())
                return "The title must be a string"_string;
            if (title->as_string().bytes().size() > maximum_title_length)
                return "The title is too long"_string;
            updated.title = title->as_string();
        }
        auto boolean = [&](StringView key, bool& target) -> ErrorOr<void, String> {
            auto value = properties.get(key);
            if (!value.has_value() || value->is_null())
                return {};
            if (!value->is_bool())
                return MUST(String::formatted("{} must be a boolean", key));
            target = value->as_bool();
            return {};
        };
        TRY(boolean("enabled"sv, updated.enabled));
        TRY(boolean("visible"sv, updated.visible));
        bool checked_given = properties.get("checked"sv).has_value() && !properties.get("checked"sv)->is_null();
        TRY(boolean("checked"sv, updated.checked));

        if (auto contexts = properties.get("contexts"sv); contexts.has_value() && !contexts->is_null()) {
            if (!contexts->is_array() || contexts->as_array().is_empty())
                return "contexts must be a list of contexts"_string;
            Vector<String> list;
            for (auto const& context : contexts->as_array().values()) {
                if (!context.is_string() || !any_of(valid_contexts, [&](auto valid) { return context.as_string() == valid; }))
                    return "Invalid context"_string;
                if (!list.contains_slow(context.as_string()))
                    list.append(context.as_string());
            }
            updated.contexts = move(list);
        }

        auto patterns = [&](StringView key, Optional<WebExtensions::MatchPatternSet>& target) -> ErrorOr<void, String> {
            auto value = properties.get(key);
            if (!value.has_value())
                return {};
            if (value->is_null()) {
                target.clear();
                return {};
            }
            if (!value->is_array() || value->as_array().size() > 1000)
                return MUST(String::formatted("{} must be a list of match patterns", key));
            Vector<String> strings;
            for (auto const& pattern : value->as_array().values()) {
                if (!pattern.is_string())
                    return MUST(String::formatted("{} must be a list of match patterns", key));
                strings.append(pattern.as_string());
            }
            auto set = WebExtensions::MatchPatternSet::parse(strings);
            if (set.is_error())
                return MUST(String::formatted("Invalid url pattern in {}", key));
            target = set.release_value();
            return {};
        };
        TRY(patterns("documentUrlPatterns"sv, updated.document_url_patterns));
        TRY(patterns("targetUrlPatterns"sv, updated.target_url_patterns));

        if (auto parent = properties.get("parentId"sv); parent.has_value()) {
            if (parent->is_null()) {
                updated.parent_key.clear();
            } else {
                auto parent_id = id_in(*parent);
                if (!parent_id.has_value())
                    return "Invalid parentId"_string;
                auto parent_key = key_of(*parent_id);
                auto const* parent_item = find_item(extension_id, parent_key);
                if (!parent_item)
                    return MUST(String::formatted("Cannot find menu item with id {}", parent_id->serialized()));
                if (parent_item->type != "normal"sv)
                    return "Parent items must have type \"normal\""_string;
                // Not the item itself, nor one of its own.
                for (auto const* ancestor = parent_item; ancestor;) {
                    if (ancestor->key == updated.key)
                        return "Cannot set an item's parent to itself or one of its children"_string;
                    ancestor = ancestor->parent_key.has_value() ? find_item(extension_id, *ancestor->parent_key) : nullptr;
                }
                updated.parent_key = parent_key;
            }
        }

        if (updated.type != "separator"sv && updated.title.is_empty())
            return "Non-separator items must have a title"_string;
        if (checked_given && updated.type != "checkbox"sv && updated.type != "radio"sv)
            return "Only items with type \"radio\" or \"checkbox\" can be checked"_string;
        if (updated.type != "checkbox"sv && updated.type != "radio"sv)
            updated.checked = false;

        item = move(updated);
        return {};
    };

    add("contextMenus.create"sv, [this, apply](Context const& context, JsonArray const& arguments, Reply reply) {
        if (arguments.is_empty() || !arguments.at(0).is_object())
            return reply("Expected the item's properties"_string);
        auto const& properties = arguments.at(0).as_object();
        auto& extension_items = items_of(context.extension_id);
        if (extension_items.items.size() >= maximum_items_per_extension)
            return reply("Too many menu items"_string);

        Item item;
        if (auto given = properties.get("id"sv); given.has_value() && !given->is_null()) {
            auto id = id_in(*given);
            if (!id.has_value())
                return reply("The id must be a string or an integer"_string);
            item.id = id.release_value();
        } else {
            // Chrome makes up an integer.
            do {
                item.id = JsonValue { extension_items.next_generated_id++ };
            } while (find_item(context.extension_id, key_of(item.id)));
        }
        item.key = key_of(item.id);
        if (find_item(context.extension_id, item.key))
            return reply(MUST(String::formatted("Cannot create item with duplicate id {}", item.id.is_string() ? item.id.as_string() : item.id.serialized())));
        item.contexts.append("page"_string);

        if (auto result = apply(context.extension_id, item, properties); result.is_error())
            return reply(result.release_error());
        auto key = item.key;
        auto checked = item.checked;
        auto id = item.id;
        extension_items.items.append(move(item));
        fix_radio_group(extension_items, key, checked);
        reply(id);
    });

    add("contextMenus.update"sv, [this, apply](Context const& context, JsonArray const& arguments, Reply reply) {
        if (arguments.size() != 2 || !arguments.at(1).is_object())
            return reply("Expected an item's ID and properties"_string);
        auto id = id_in(arguments.at(0));
        if (!id.has_value())
            return reply("The id must be a string or an integer"_string);
        auto* item = find_item(context.extension_id, key_of(*id));
        if (!item)
            return reply(MUST(String::formatted("Cannot find menu item with id {}", id->is_string() ? id->as_string() : id->serialized())));
        if (auto result = apply(context.extension_id, *item, arguments.at(1).as_object()); result.is_error())
            return reply(result.release_error());
        bool checked_given = arguments.at(1).as_object().get("checked"sv).has_value();
        fix_radio_group(items_of(context.extension_id), key_of(*id), checked_given && item->checked);
        reply(JsonValue {});
    });

    add("contextMenus.remove"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        if (arguments.size() != 1)
            return reply("Expected an item's ID"_string);
        auto id = id_in(arguments.at(0));
        if (!id.has_value())
            return reply("The id must be a string or an integer"_string);
        auto key = key_of(*id);
        if (!find_item(context.extension_id, key))
            return reply(MUST(String::formatted("Cannot find menu item with id {}", id->is_string() ? id->as_string() : id->serialized())));
        remove_with_descendants(items_of(context.extension_id), key);
        reply(JsonValue {});
    });

    add("contextMenus.removeAll"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        if (auto items = m_items.find(context.extension_id); items != m_items.end())
            items->value.items.clear();
        reply(JsonValue {});
    });
}

ContextMenusAPI::ExtensionItems& ContextMenusAPI::items_of(StringView extension_id)
{
    return m_items.ensure(MUST(String::from_utf8(extension_id)), [&] {
        ExtensionItems items;
        if (auto const* extension = runtime().registry().find(extension_id))
            items.version = extension->manifest().version;
        return items;
    });
}

ContextMenusAPI::Item* ContextMenusAPI::find_item(StringView extension_id, StringView key)
{
    auto items = m_items.find(extension_id);
    if (items == m_items.end())
        return nullptr;
    for (auto& item : items->value.items) {
        if (item.key == key)
            return &item;
    }
    return nullptr;
}

ContextMenusAPI::Item const* ContextMenusAPI::find_item(StringView extension_id, StringView key) const
{
    return const_cast<ContextMenusAPI&>(*this).find_item(extension_id, key);
}

void ContextMenusAPI::remove_with_descendants(ExtensionItems& extension_items, StringView key)
{
    Vector<String> children;
    for (auto const& item : extension_items.items) {
        if (item.parent_key == key)
            children.append(item.key);
    }
    for (auto const& child : children)
        remove_with_descendants(extension_items, child);
    extension_items.items.remove_first_matching([&](auto const& item) { return item.key == key; });
}

// Chrome's radio groups: runs of radio items next to each other, among the items of one parent. One of each is checked.
void ContextMenusAPI::fix_radio_group(ExtensionItems& extension_items, StringView key, bool checked_now)
{
    Item* changed = nullptr;
    for (auto& item : extension_items.items) {
        if (item.key == key)
            changed = &item;
    }
    if (!changed)
        return;
    auto parent = changed->parent_key;

    // The parent's items, in order, split into runs of radio items.
    Vector<Vector<Item*>> groups;
    bool in_group = false;
    for (auto& item : extension_items.items) {
        if (item.parent_key != parent)
            continue;
        if (item.type != "radio"sv) {
            in_group = false;
            continue;
        }
        if (!in_group)
            groups.append({});
        in_group = true;
        groups.last().append(&item);
    }
    for (auto& group : groups) {
        if (checked_now && group.contains_slow(changed)) {
            for (auto* item : group)
                item->checked = item == changed;
            continue;
        }
        size_t checked_count = 0;
        for (auto* item : group)
            checked_count += item->checked ? 1 : 0;
        if (checked_count == 0) {
            group.first()->checked = true;
        } else if (checked_count > 1) {
            bool seen = false;
            for (auto* item : group) {
                if (item->checked && seen)
                    item->checked = false;
                seen = seen || item->checked;
            }
        }
    }
}

static bool patterns_match(Optional<WebExtensions::MatchPatternSet> const& patterns, URL::URL const& url)
{
    return !patterns.has_value() || patterns->matches(url);
}

// Chrome's matching of an item's contexts and patterns with what was clicked.
bool ContextMenusAPI::matches(Item const& item, Target const& target) const
{
    if (!item.visible)
        return false;

    auto has = [&](StringView context) { return item.contexts.contains_slow(context); };

    if (target.action_of_extension.has_value())
        return has("all"sv) || any_of(item.contexts, [](auto const& context) { return is_action_context(context); });

    auto const& document_url = target.frame_url.has_value() ? *target.frame_url : target.page_url;
    if (!patterns_match(item.document_url_patterns, document_url))
        return false;

    bool has_selection = target.selection_text.has_value() && !target.selection_text->is_empty();
    bool has_link = target.link_url.has_value();
    auto media_type = target.media_type.value_or({});

    if (has("all"sv) || (has_selection && has("selection"sv)) || (target.editable && has("editable"sv)))
        return true;
    if (has_link && has("link"sv) && patterns_match(item.target_url_patterns, *target.link_url))
        return true;
    if (!media_type.is_empty() && first_is_one_of(media_type, "image"sv, "video"sv, "audio"sv) && has(media_type)) {
        if (!target.src_url.has_value() || patterns_match(item.target_url_patterns, *target.src_url))
            return true;
    }
    // The page is the least specific context: only when none of the others apply (a frame is a page, too).
    if (!has_link && !has_selection && !target.editable && media_type.is_empty() && has("page"sv))
        return true;
    if (target.frame_url.has_value() && has("frame"sv))
        return true;
    return false;
}

static String title_for(String const& title, Optional<String> const& selection)
{
    if (!title.contains("%s"sv))
        return title;
    auto text = selection.value_or({});
    Utf8View view { text };
    if (view.length() > maximum_selection_length) {
        StringBuilder truncated;
        size_t count = 0;
        for (auto code_point : view) {
            if (count++ == maximum_selection_length)
                break;
            truncated.append_code_point(code_point);
        }
        truncated.append("…"sv);
        text = truncated.to_string_without_validation();
    }
    return MUST(title.replace("%s"sv, text, ReplaceMode::All));
}

ContextMenusAPI::MenuItem ContextMenusAPI::menu_item_for(ExtensionItems const& extension_items, Item const& item, Target const& target) const
{
    MenuItem menu_item;
    menu_item.key = item.key;
    menu_item.title = title_for(item.title, target.selection_text);
    menu_item.type = item.type;
    menu_item.checked = item.checked;
    menu_item.enabled = item.enabled;
    for (auto const& child : extension_items.items) {
        if (child.parent_key == item.key && matches(child, target))
            menu_item.children.append(menu_item_for(extension_items, child, target));
    }
    return menu_item;
}

static void set_extension_id(ContextMenusAPI::MenuItem& item, String const& extension_id)
{
    item.extension_id = extension_id;
    for (auto& child : item.children)
        set_extension_id(child, extension_id);
}

Vector<ContextMenusAPI::MenuItem> ContextMenusAPI::items_for(Target const& target) const
{
    struct ExtensionMenu {
        String name;
        Vector<MenuItem> items;
    };
    Vector<ExtensionMenu> menus;

    for (auto const& [extension_id, extension_items] : m_items) {
        if (target.action_of_extension.has_value() && *target.action_of_extension != extension_id)
            continue;
        auto const* extension = runtime().registry().find(extension_id);
        if (!extension || !extension->manifest().has_permission("contextMenus"sv))
            continue;

        ExtensionMenu menu;
        menu.name = extension->messages().substitute_manifest_messages(extension->manifest().name);
        for (auto const& item : extension_items.items) {
            if (item.parent_key.has_value() || !matches(item, target))
                continue;
            auto menu_item = menu_item_for(extension_items, item, target);
            set_extension_id(menu_item, extension_id);
            menu.items.append(move(menu_item));
        }
        if (menu.items.is_empty())
            continue;

        if (target.action_of_extension.has_value()) {
            if (menu.items.size() > action_menu_top_level_limit)
                menu.items.shrink(action_menu_top_level_limit);
        } else if (menu.items.size() > 1) {
            // More than one: under the extension's name.
            MenuItem parent;
            parent.extension_id = extension_id;
            parent.title = menu.name;
            parent.type = "normal"_string;
            parent.children = move(menu.items);
            menu.items = { move(parent) };
        }
        menus.append(move(menu));
    }

    quick_sort(menus, [](auto const& a, auto const& b) { return a.name < b.name; });
    Vector<MenuItem> result;
    for (auto& menu : menus)
        result.extend(move(menu.items));
    return result;
}

void ContextMenusAPI::clicked(StringView extension_id, StringView key, Target const& target)
{
    auto items = m_items.find(extension_id);
    if (items == m_items.end())
        return;
    auto* item = find_item(extension_id, key);
    if (!item || !item->enabled || item->type == "separator"sv)
        return;

    // Only what the menu showed: the item and its parents match the target.
    if (target.action_of_extension.has_value() && *target.action_of_extension != extension_id)
        return;
    for (auto const* shown = item; shown;) {
        if (!matches(*shown, target))
            return;
        shown = shown->parent_key.has_value() ? find_item(extension_id, *shown->parent_key) : nullptr;
    }

    JsonObject info;
    info.set("menuItemId"sv, item->id);
    if (item->parent_key.has_value()) {
        if (auto const* parent = find_item(extension_id, *item->parent_key))
            info.set("parentMenuItemId"sv, parent->id);
    }
    if (target.media_type.has_value())
        info.set("mediaType"sv, *target.media_type);
    if (target.link_url.has_value())
        info.set("linkUrl"sv, target.link_url->serialize());
    if (target.src_url.has_value())
        info.set("srcUrl"sv, target.src_url->serialize());
    if (!target.page_url.scheme().is_empty())
        info.set("pageUrl"sv, target.page_url.serialize());
    if (target.frame_url.has_value())
        info.set("frameUrl"sv, target.frame_url->serialize());
    info.set("frameId"sv, 0);
    if (target.selection_text.has_value() && !target.selection_text->is_empty())
        info.set("selectionText"sv, *target.selection_text);
    info.set("editable"sv, target.editable);

    if (item->type == "checkbox"sv || item->type == "radio"sv) {
        bool was_checked = item->checked;
        info.set("wasChecked"sv, was_checked);
        if (item->type == "checkbox"sv) {
            item->checked = !was_checked;
        } else {
            item->checked = true;
            fix_radio_group(items->value, key, true);
        }
        info.set("checked"sv, item->checked);
    }

    auto dispatch = [this, extension_id = MUST(String::from_utf8(extension_id))](JsonArray arguments) {
        if (runtime().background_is_starting(extension_id)) {
            runtime().wait_for_background(extension_id, [this, extension_id, arguments = move(arguments)]() mutable {
                runtime().dispatch_event(extension_id, "contextMenus.onClicked"sv, move(arguments));
            });
            return;
        }
        runtime().dispatch_event(extension_id, "contextMenus.onClicked"sv, move(arguments));
    };

    if (!target.tab_id.has_value())
        return dispatch(JsonArray { Vector<JsonValue> { move(info) } });

    // The tab, as chrome.tabs describes it to the extension.
    Context context;
    context.extension_id = MUST(String::from_utf8(extension_id));
    context.type = "background"_string;
    JsonArray arguments;
    arguments.must_append(*target.tab_id);
    runtime().call_function("tabs.get"sv, context, arguments, [dispatch = move(dispatch), info = move(info)](ErrorOr<JsonValue, String> tab) mutable {
        JsonArray event_arguments;
        event_arguments.must_append(move(info));
        if (!tab.is_error() && tab.value().is_object())
            event_arguments.must_append(tab.release_value());
        dispatch(move(event_arguments));
    });
}

size_t ContextMenusAPI::item_count(StringView extension_id) const
{
    auto items = m_items.find(extension_id);
    return items == m_items.end() ? 0 : items->value.items.size();
}

void ContextMenusAPI::extensions_changed()
{
    // A removed extension's items go; one replaced by a new version creates its items again (as in Chrome).
    m_items.remove_all_matching([&](auto const& extension_id, auto const& items) {
        auto const* extension = runtime().registry().find(extension_id);
        return !extension || extension->manifest().version != items.version;
    });
}

}
