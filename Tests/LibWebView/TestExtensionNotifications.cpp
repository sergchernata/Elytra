/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// chrome.notifications: what extensions show, what the UI is asked to show, and what the user does with it.

#include "ExtensionTestHarness.h"

#include <AK/QuickSort.h>

#include <LibWebView/ExtensionAPI/Notifications.h>
#include <LibWebView/ExtensionRuntime.h>

namespace {

constexpr auto extension_id = "cjjdgkffnjjdgpaeceflnbkbmobofbcd"sv;
constexpr auto unpermitted_id = "ecpkkeepjfeflknlcnoijhbpgeapadma"sv;

class TestNotificationsDelegate final : public WebView::ExtensionNotificationsDelegate {
public:
    virtual void show_notification(WebView::ExtensionNotification const& notification) override
    {
        shown.append(notification);
    }

    virtual void close_notification(StringView extension, StringView id) override
    {
        closed.append(MUST(String::formatted("{}/{}", extension, id)));
    }

    Optional<WebView::ExtensionNotification const&> last_shown(StringView id) const
    {
        for (size_t i = shown.size(); i > 0; --i) {
            if (shown[i - 1].id == id)
                return shown[i - 1];
        }
        return {};
    }

    Vector<WebView::ExtensionNotification> shown;
    Vector<String> closed;
};

bool is_error(String const& value)
{
    return value.bytes_as_string_view().starts_with("error"sv);
}

URL::URL page(StringView id, StringView step)
{
    return ExtensionTests::url(MUST(String::formatted("chrome-extension://{}/page.html?step={}", id, step)));
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return ExtensionTests::run_extension_tests(arguments, [](ExtensionTests::Harness& harness) {
        using namespace ExtensionTests;
        TestNotificationsDelegate delegate;
        auto& notifications = WebView::Application::extension_runtime().notifications();
        notifications.set_delegate(&delegate);

        harness.registry.set_extensions_directory(ByteString::formatted("{}/Extensions", harness.directory));
        MUST(harness.registry.install_unpacked(fixture("notifications"sv)));
        MUST(harness.registry.load_unpacked(fixture("notifications_nopermission"sv)));
        auto icon = MUST(MUST(Core::File::open(ByteString::formatted("{}/icon.png", fixture("notifications"sv)), Core::File::OpenMode::Read))->read_until_eof());

        auto report = report_of(harness.view, page(extension_id, "create"sv));
        CHECK_EQ(string_in(report, "named"sv), "n1"sv);
        auto generated = string_in(report, "generated"sv);
        CHECK(!generated.is_empty() && !is_error(generated) && generated != "(missing)"sv);
        auto generated_empty = string_in(report, "generated_empty"sv);
        CHECK(!generated_empty.is_empty() && !is_error(generated_empty) && generated_empty != generated);
        CHECK_EQ(string_in(report, "callback"sv), "cb"sv);
        CHECK_EQ(string_in(report, "data_icon"sv), "data"sv);
        CHECK_EQ(string_in(report, "blob_icon"sv), "blob"sv);
        CHECK_EQ(string_in(report, "own_url_icon"sv), "own"sv);
        CHECK_EQ(string_in(report, "list"sv), "list"sv);
        CHECK_EQ(string_in(report, "progress"sv), "progress"sv);
        for (auto key : { "remote_icon"sv, "escaping_icon"sv, "missing_icon"sv, "other_extension_icon"sv, "no_title"sv, "no_type"sv, "bad_type"sv, "bad_title"sv, "bad_options"sv, "bad_id"sv, "bad_progress"sv, "items_on_basic"sv, "image_without_url"sv, "bad_buttons"sv, "updated_bad"sv }) {
            if (!is_error(string_in(report, key)))
                warnln("{} is \"{}\"", key, string_in(report, key));
            CHECK(is_error(string_in(report, key)));
        }
        CHECK(report.get_bool("updated"sv).value_or(false));
        CHECK(!report.get_bool("updated_missing"sv).value_or(true));
        CHECK(report.get_bool("cleared"sv).value_or(false));
        CHECK(!report.get_bool("cleared_missing"sv).value_or(true));
        CHECK_EQ(string_in(report, "all"sv), MUST(String::join(","sv, [&] {
            Vector<String> ids { "blob"_string, "data"_string, "list"_string, "n1"_string, "own"_string, "progress"_string, generated, generated_empty };
            quick_sort(ids);
            return ids;
        }())));
        CHECK(report.get_bool("all_values"sv).value_or(false));
        CHECK_EQ(string_in(report, "level"sv), "granted"sv);

        // What the UI was asked to show.
        auto n1 = delegate.last_shown("n1"sv);
        CHECK(n1.has_value());
        if (n1.has_value()) {
            CHECK_EQ(n1->extension_id, extension_id);
            CHECK_EQ(n1->type, "basic"sv);
            CHECK_EQ(n1->title, "Saved again"sv);
            CHECK_EQ(n1->message, "The item was saved."sv);
            CHECK_EQ(n1->context_message, "1Password"sv);
            CHECK(n1->icon == icon);
            CHECK_EQ(n1->priority, 2);
            CHECK(n1->event_time == 1700000000000.0);
            CHECK_EQ(n1->buttons.size(), 2u);
            if (n1->buttons.size() == 2) {
                CHECK_EQ(n1->buttons[0], "Open"sv);
                CHECK_EQ(n1->buttons[1], "Dismiss"sv);
            }
            CHECK(n1->require_interaction);
            CHECK(n1->silent);
        }
        for (auto id : { "data"sv, "blob"sv, "own"sv }) {
            auto shown = delegate.last_shown(id);
            CHECK(shown.has_value() && shown->icon == icon);
        }
        auto list = delegate.last_shown("list"sv);
        CHECK(list.has_value() && list->items.size() == 2 && list->items[1].title == "Two"sv && list->items[1].message == "Second"sv);
        auto progress = delegate.last_shown("progress"sv);
        CHECK(progress.has_value() && progress->progress == 42);
        CHECK(!delegate.last_shown("remote"sv).has_value());
        CHECK(delegate.closed.contains_slow(MUST(String::formatted("{}/cb", extension_id))));
        CHECK(notifications.find(extension_id, "n1"sv).has_value());
        CHECK(!notifications.find(extension_id, "cb"sv).has_value());
        CHECK(!notifications.find(unpermitted_id, "n1"sv).has_value());

        // Creating with an ID in use replaces the notification.
        report = report_of(harness.view, page(extension_id, "replace"sv));
        CHECK_EQ(string_in(report, "replaced"sv), "n1"sv);
        CHECK(notifications.find(extension_id, "n1"sv).has_value() && notifications.find(extension_id, "n1"sv)->title == "Replaced"sv);

        // What the user does, the extension hears of.
        notifications.clicked(extension_id, "n1"sv);
        notifications.button_clicked(extension_id, "n1"sv, 1);
        notifications.button_clicked(extension_id, "n1"sv, 5); // No such button.
        notifications.clicked(extension_id, "nothing"sv);
        notifications.clicked(unpermitted_id, "n1"sv);
        notifications.clicked("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv, "n1"sv);
        notifications.closed(extension_id, "n1"sv, true);
        notifications.closed(extension_id, "data"sv, false);
        notifications.closed(extension_id, "n1"sv, true); // Already gone.
        report = report_of(harness.view, page(extension_id, "closed"sv));
        CHECK_EQ(string_in(report, "clicked"sv), R"(["n1"])"sv);
        CHECK_EQ(string_in(report, "button_clicked"sv), R"([["n1",1]])"sv);
        // clear() closes without the user: byUser is false.
        CHECK_EQ(string_in(report, "closed"sv), R"([["cb",false],["n1",true],["data",false]])"sv);
        CHECK(!notifications.find(extension_id, "n1"sv).has_value());

        report = report_of(harness.view, page(extension_id, "all"sv));
        auto remaining = string_in(report, "all"sv);
        CHECK(!remaining.bytes_as_string_view().contains("n1"sv));

        // Without the permission.
        report = report_of(harness.view, page(unpermitted_id, "none"sv));
        CHECK(is_error(string_in(report, "create"sv)));
        CHECK(is_error(string_in(report, "all"sv)));
        CHECK(is_error(string_in(report, "level"sv)));
        CHECK(notifications.notifications_of(unpermitted_id).is_empty());

        // An extension that goes takes its notifications with it.
        delegate.closed.clear();
        MUST(harness.registry.set_enabled(extension_id, false));
        CHECK(notifications.notifications_of(extension_id).is_empty());
        CHECK(delegate.closed.contains_slow(MUST(String::formatted("{}/blob", extension_id))));

        notifications.set_delegate(nullptr);
    });
}
