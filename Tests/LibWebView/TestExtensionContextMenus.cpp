/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// chrome.contextMenus: the items extensions add, which of them the page's context menu and the extension button's show,
// and what the extension hears when the user chooses one.

#include "ExtensionTestHarness.h"

#include <LibWebView/ExtensionAPI/ContextMenus.h>
#include <LibWebView/ExtensionRuntime.h>

namespace {

using MenuItem = WebView::ContextMenusAPI::MenuItem;
using Target = WebView::ContextMenusAPI::Target;

constexpr auto extension_id = "oaalcbgdgmkcfbnnfmmdmaefclfiioip"sv;
constexpr auto single_id = "febgjfinninpgpdmhklnhklacjhagjfi"sv;
constexpr auto unpermitted_id = "leagapeclaegemdakjldpbnflmdnogma"sv;

bool is_error(String const& value)
{
    return value.bytes_as_string_view().starts_with("error"sv);
}

URL::URL page(StringView id, StringView step)
{
    return ExtensionTests::url(MUST(String::formatted("chrome-extension://{}/page.html?step={}", id, step)));
}

MenuItem const* find(Vector<MenuItem> const& items, StringView title)
{
    for (auto const& item : items) {
        if (item.title == title)
            return &item;
        if (auto const* child = find(item.children, title))
            return child;
    }
    return nullptr;
}

// The titles of the items, with their children in brackets: "A, B [C, D]".
String outline(Vector<MenuItem> const& items)
{
    StringBuilder builder;
    for (auto const& item : items) {
        if (!builder.is_empty())
            builder.append(", "sv);
        builder.append(item.type == "separator"sv ? "---"sv : item.title.bytes_as_string_view());
        if (item.type == "checkbox"sv || item.type == "radio"sv)
            builder.append(item.checked ? "(x)"sv : "( )"sv);
        if (!item.enabled)
            builder.append("(disabled)"sv);
        if (!item.children.is_empty())
            builder.appendff(" [{}]", outline(item.children));
    }
    return builder.to_string_without_validation();
}

Vector<MenuItem> items_of(Vector<MenuItem> const& items, StringView id)
{
    Vector<MenuItem> result;
    for (auto const& item : items) {
        if (item.extension_id == id)
            result.append(item);
    }
    return result;
}

Target page_target(StringView url)
{
    Target target;
    target.page_url = ExtensionTests::url(url);
    return target;
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return ExtensionTests::run_extension_tests(arguments, [](ExtensionTests::Harness& harness) {
        using namespace ExtensionTests;
        auto& menus = WebView::Application::extension_runtime().context_menus();

        MUST(harness.registry.load_unpacked(fixture("menus"sv)));
        MUST(harness.registry.load_unpacked(fixture("menus_single"sv)));
        MUST(harness.registry.load_unpacked(fixture("menus_nopermission"sv)));

        auto report = report_of(harness.view, page(extension_id, "create"sv));
        CHECK_EQ(string_in(report, "remove_all"sv), "removed"sv);
        // create() returns the ID it was given.
        CHECK_EQ(string_in(report, "settings"sv), "created \"settings\""sv);
        CHECK_EQ(string_in(report, "image"sv), "created 7"sv);
        for (auto key : { "lock"sv, "save"sv, "save_login"sv, "flag"sv, "selection"sv, "link"sv, "document_patterns"sv, "editable"sv, "hidden"sv, "disabled"sv, "radios"sv, "r1"sv, "r2"sv, "r3"sv, "separator"sv, "to_remove"sv })
            CHECK(string_in(report, key).starts_with_bytes("created"sv));
        CHECK_EQ(string_in(report, "generated"sv), "created undefined"sv);
        for (auto key : { "duplicate"sv, "missing_parent"sv, "bad_context"sv, "bad_type"sv, "no_title"sv, "bad_title"sv, "bad_pattern"sv, "bad_id"sv, "bad_properties"sv, "separator_parent"sv, "update_missing"sv, "update_cycle"sv, "update_self"sv, "update_bad"sv, "remove_missing"sv }) {
            if (!is_error(string_in(report, key)))
                warnln("{} is \"{}\"", key, string_in(report, key));
            CHECK(is_error(string_in(report, key)));
        }
        CHECK_EQ(string_in(report, "update"sv), "updated"sv);
        CHECK_EQ(string_in(report, "remove"sv), "removed"sv);

        // The other extension creates its item as its background starts.
        auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(10);
        auto wake = Core::Timer::create_repeating(AK::Duration::from_milliseconds(50), [] { });
        wake->start();
        Core::EventLoop::current().spin_until([&] { return menus.item_count(single_id) > 0 || MonotonicTime::now() > deadline; });
        wake->stop();

        // The single item of an extension shows by itself; another extension's many under its name.
        auto on_page = menus.items_for(page_target("https://example.com/page"sv));
        CHECK_EQ(outline(items_of(on_page, single_id)), "The only item"sv);
        auto own = items_of(on_page, extension_id);
        CHECK_EQ(outline(own), "Menus test extension [Lock now, Save [Login, Flag(x)], Disabled(disabled), Radios [R1( ), R2( ), R3(x)], ---, No ID]"sv);
        CHECK(items_of(on_page, unpermitted_id).is_empty());
        CHECK(own.size() == 1 && own[0].key.is_empty());

        // What was right-clicked picks the items.
        auto selection = page_target("https://example.com/page"sv);
        selection.selection_text = "hello"_string;
        auto with_selection = items_of(menus.items_for(selection), extension_id);
        CHECK(find(with_selection, "Search for “hello”"sv));
        CHECK(!find(on_page, "Search for “hello”"sv));
        // Chrome's "page" is for none of the others: a selection isn't the page.
        auto link = page_target("https://example.com/page"sv);
        link.link_url = url("https://example.com/target"sv);
        CHECK(find(items_of(menus.items_for(link), extension_id), "Link item"sv));
        link.link_url = url("https://example.net/target"sv);
        CHECK(!find(items_of(menus.items_for(link), extension_id), "Link item"sv));
        auto image = page_target("https://example.com/page"sv);
        image.src_url = url("https://example.com/picture.png"sv);
        image.media_type = "image"_string;
        CHECK(find(items_of(menus.items_for(image), extension_id), "Image item"sv));
        auto editable = page_target("https://example.com/page"sv);
        editable.editable = true;
        CHECK(find(items_of(menus.items_for(editable), extension_id), "Editable item"sv));
        CHECK(find(items_of(menus.items_for(page_target("https://example.org/"sv)), extension_id), "Only example.org"sv));

        // The extension's button shows its "action" and "all" items, without a parent, and six at most (as Chrome).
        auto action = page_target("https://example.com/page"sv);
        action.action_of_extension = MUST(String::from_utf8(extension_id));
        auto action_items = menus.items_for(action);
        CHECK_EQ(outline(action_items), "Settings, Lock now, Save [Login, Flag(x)], Disabled(disabled), Radios [R1( ), R2( ), R3(x)], ---"sv);

        // Choosing items.
        auto key_of = [&](Vector<MenuItem> const& items, StringView title) {
            auto const* item = find(items, title);
            CHECK(item);
            return item ? item->key : String {};
        };
        auto page_click = page_target("https://example.com/page"sv);
        menus.clicked(extension_id, key_of(own, "Flag"sv), page_click);
        menus.clicked(extension_id, key_of(own, "R1"sv), page_click);
        menus.clicked(extension_id, key_of(own, "Disabled"sv), page_click); // Disabled: nothing.
        menus.clicked(extension_id, key_of(with_selection, "Search for “hello”"sv), selection);
        menus.clicked(extension_id, key_of(action_items, "Settings"sv), action);
        menus.clicked(extension_id, "\"hidden\""sv, page_click);             // Not shown: nothing.
        menus.clicked(extension_id, "\"nothing\""sv, page_click);            // Not an item.
        menus.clicked(single_id, key_of(own, "Lock now"sv), page_click);     // Another extension's.
        menus.clicked("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv, "\"lock\""sv, page_click);
        menus.clicked(extension_id, key_of(own, "Lock now"sv), link);         // A link target.

        report = report_of(harness.view, page(extension_id, "5"sv));
        auto clicks = JsonParser::parse(string_in(report, "clicks"sv));
        CHECK(!clicks.is_error() && clicks.value().is_array());
        if (!clicks.is_error() && clicks.value().is_array()) {
            auto const& array = clicks.value().as_array();
            CHECK_EQ(array.size(), 5u);
            auto click = [&](size_t index) { return index < array.size() && array.at(index).is_object() ? array.at(index).as_object() : JsonObject {}; };
            CHECK_EQ(string_in(click(0), "menuItemId"sv), "flag"sv);
            CHECK_EQ(string_in(click(0), "parentMenuItemId"sv), "save"sv);
            CHECK_EQ(string_in(click(0), "pageUrl"sv), "https://example.com/page"sv);
            CHECK(click(0).get_bool("wasChecked"sv) == true);
            CHECK(click(0).get_bool("checked"sv) == false);
            CHECK(click(0).get_bool("editable"sv) == false);
            CHECK(click(0).get_integer<i64>("frameId"sv) == 0);
            CHECK_EQ(string_in(click(1), "menuItemId"sv), "r1"sv);
            CHECK(click(1).get_bool("wasChecked"sv) == false);
            CHECK(click(1).get_bool("checked"sv) == true);
            CHECK_EQ(string_in(click(2), "selectionText"sv), "hello"sv);
            CHECK(!click(2).has("wasChecked"sv));
            CHECK_EQ(string_in(click(3), "menuItemId"sv), "settings"sv);
            CHECK_EQ(string_in(click(4), "menuItemId"sv), "lock"sv);
            CHECK_EQ(string_in(click(4), "linkUrl"sv), "https://example.net/target"sv);
        }

        // The check marks follow the clicks; a radio item checks itself and unchecks its group.
        CHECK_EQ(outline(items_of(menus.items_for(page_target("https://example.com/page"sv)), extension_id)), "Menus test extension [Lock now, Save [Login, Flag( )], Disabled(disabled), Radios [R1(x), R2( ), R3( )], ---, No ID]"sv);

        // Without the permission.
        report = report_of(harness.view, page(unpermitted_id, "none"sv));
        CHECK(is_error(string_in(report, "create"sv)));
        CHECK(is_error(string_in(report, "remove_all"sv)));
        CHECK_EQ(menus.item_count(unpermitted_id), 0u);

        // removeAll takes only the extension's own.
        report = report_of(harness.view, page(extension_id, "remove_all"sv));
        CHECK_EQ(menus.item_count(extension_id), 0u);
        CHECK_EQ(menus.item_count(single_id), 1u);
    });
}
