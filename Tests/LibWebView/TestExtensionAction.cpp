/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// chrome.action and chrome.commands: an extension's toolbar button, its popup, and its keyboard commands.

#include "ExtensionTestHarness.h"

#include <LibWebView/ExtensionAPI/Action.h>
#include <LibWebView/ExtensionRuntime.h>

namespace {

constexpr auto extension_id = "gnbeingkbnibjpkaemagelnocnemcghh"sv;

class TestActionDelegate final : public WebView::ExtensionActionDelegate {
public:
    virtual void action_changed(StringView id) override
    {
        if (id == extension_id)
            ++changes;
    }

    virtual ErrorOr<void, String> open_popup(StringView id, URL::URL const& popup, Optional<i64>) override
    {
        if (id == extension_id)
            opened_popups.append(popup.serialize());
        return {};
    }

    size_t changes { 0 };
    Vector<String> opened_popups;
};

bool is_error(String const& value)
{
    return value.bytes_as_string_view().starts_with("error"sv);
}

URL::URL page(StringView step)
{
    return ExtensionTests::url(MUST(String::formatted("chrome-extension://{}/page.html?step={}", extension_id, step)));
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return ExtensionTests::run_extension_tests(arguments, [](ExtensionTests::Harness& harness) {
        using namespace ExtensionTests;
        TestActionDelegate delegate;
        auto& action = WebView::Application::extension_runtime().action();
        action.set_delegate(&delegate);

        MUST(harness.registry.load_unpacked(fixture("action"sv)));

        // The manifest's button, before anything changes it.
        auto initial = action.state_for(extension_id);
        CHECK(initial.has_value());
        if (initial.has_value()) {
            CHECK_EQ(initial->title, "Default title"sv);
            CHECK_EQ(initial->popup.has_value() ? initial->popup->serialize() : String {}, MUST(String::formatted("chrome-extension://{}/popup.html", extension_id)));
            CHECK_EQ(initial->icon_paths.get(16).value_or({}), "icons/16.png"sv);
        }

        // Clicking a button with a popup opens it.
        action.clicked(extension_id, 3);
        CHECK_EQ(delegate.opened_popups.size(), 1u);

        auto report = report_of(harness.view, page("set"sv));
        CHECK_EQ(string_in(report, "default_title"sv), "Default title"sv);
        CHECK_EQ(string_in(report, "default_popup"sv), MUST(String::formatted("chrome-extension://{}/popup.html", extension_id)));
        CHECK_EQ(string_in(report, "title"sv), "Everywhere"sv);
        CHECK_EQ(string_in(report, "tab_title"sv), "Just tab 5"sv);
        CHECK_EQ(string_in(report, "other_tab_title"sv), "Everywhere"sv);
        CHECK_EQ(string_in(report, "badge"sv), "42"sv);
        CHECK_EQ(string_in(report, "badge_color"sv), "[255,0,0,255]"sv);
        CHECK_EQ(string_in(report, "badge_text_color"sv), "[0,0,255,255]"sv);
        CHECK(is_error(string_in(report, "bad_color"sv)));
        CHECK(is_error(string_in(report, "bad_popup"sv)));
        CHECK(is_error(string_in(report, "escaping_popup"sv)));
        CHECK(report.get_bool("enabled"sv).value_or(false));
        CHECK(!report.get_bool("enabled_tab_7"sv).value_or(true));
        CHECK_EQ(string_in(report, "settings"sv), R"({"isOnToolbar":true})"sv);
#if defined(AK_OS_MACOS)
        CHECK_EQ(string_in(report, "commands"sv), R"([["_execute_action","Command+Shift+Y",""],["lock","Ctrl+Shift+K","Lock it"],["unbound","","No key"]])"sv);
#endif

        // What the UI sees, and hears of.
        CHECK(delegate.changes > 0);
        auto everywhere = action.state_for(extension_id);
        auto tab_7 = action.state_for(extension_id, 7);
        CHECK(everywhere.has_value() && tab_7.has_value());
        if (everywhere.has_value() && tab_7.has_value()) {
            CHECK_EQ(everywhere->badge_text, "42"sv);
            CHECK(everywhere->badge_background_color == Color(255, 0, 0, 255));
            CHECK(!everywhere->popup.has_value());
            CHECK(everywhere->enabled);
            CHECK(!tab_7->enabled);
        }

        // Without a popup, the extension hears of the click; a disabled button does nothing.
        action.clicked(extension_id, 7);
        action.clicked(extension_id, 3);
        report = report_of(harness.view, page("clicked"sv));
        CHECK_EQ(report.get_integer<i64>("clicked"sv).value_or(-1), 3);

        // Commands: the extension's own, and _execute_action, which clicks the button.
        action.command(extension_id, "lock"sv, 3);
        report = report_of(harness.view, page("command"sv));
        CHECK_EQ(string_in(report, "command"sv), "lock"sv);
        CHECK_EQ(report.get_integer<i64>("command_tab"sv).value_or(-1), 3);
        action.command(extension_id, "not one of its commands"sv, 3);

        // Another extension's ID, or none at all, does nothing.
        action.clicked("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv, 3);
        CHECK(!action.state_for("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv).has_value());

        action.set_delegate(nullptr);
    });
}
