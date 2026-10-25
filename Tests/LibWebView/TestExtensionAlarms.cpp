/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// chrome.alarms, chrome.idle and chrome.i18n.

#include "ExtensionTestHarness.h"
#include <LibCore/Timer.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/Settings.h>

namespace ExtensionTests {

namespace {

constexpr auto alarms_id = "bcdjfomfdcckdhidihnlfllidbiloicc"sv;
constexpr auto other_id = "ececgkkonmpohljkliklppllfmajalij"sv;
constexpr auto no_permission_id = "haedeajgedkohcokncbeiclhadcfalog"sv;

JsonObject run(WebView::HeadlessWebView& view, StringView id, StringView test)
{
    return report_of(view, url(ByteString::formatted("chrome-extension://{}/page.html?test={}", id, test)));
}

void wait(AK::Duration duration)
{
    bool done = false;
    auto timer = Core::Timer::create_single_shot(duration, [&] { done = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return done; });
}

bool is_error(JsonObject const& report, StringView key)
{
    return string_in(report, key).starts_with_bytes("error: "sv);
}

void alarms_wait_at_least_thirty_seconds_by_default(WebView::HeadlessWebView& view)
{
    auto report = run(view, alarms_id, "default_minimum"sv);
    CHECK_EQ(string_in(report, "_"sv), "done"sv);
    auto delay = report.get_double_with_precision_loss("delay"sv).value_or(0);
    CHECK(delay >= 29'900 && delay <= 31'000);
    CHECK(report.get_bool("cleared"sv).value_or(false));
}

void alarms_are_made_found_replaced_and_cleared(WebView::HeadlessWebView& view)
{
    auto report = run(view, alarms_id, "basics"sv);
    CHECK_EQ(string_in(report, "_"sv), "done"sv);
    CHECK_EQ(string_in(report, "created"sv), "undefined"sv);
    CHECK_EQ(string_in(report, "a_name"sv), "a"sv);
    CHECK_EQ(report.get_integer<i64>("a_delay"sv).value_or(0), 600);
    CHECK(!report.get_bool("a_has_period"sv).value_or(true));
    CHECK_EQ(string_in(report, "unnamed"sv), "\"\":2"sv);
    CHECK_EQ(string_in(report, "periodic_only"sv), "5:5"sv);
    CHECK_EQ(report.get_integer<i64>("replaced_delay"sv).value_or(0), 20);
    CHECK_EQ(string_in(report, "names"sv), ",a,p"sv);
    CHECK_EQ(string_in(report, "missing"sv), "undefined"sv);
    CHECK(!report.get_bool("clear_missing"sv).value_or(true));
    CHECK(report.get_bool("clear_a"sv).value_or(false));
    CHECK_EQ(string_in(report, "after_clear"sv), "undefined"sv);
    for (auto key : { "both_times"sv, "no_time"sv, "no_info"sv, "bad_info"sv, "bad_name"sv, "bad_delay"sv, "bad_period"sv, "bad_get"sv, "bad_clear"sv })
        CHECK(is_error(report, key));
    CHECK_EQ(string_in(report, "x_exists"sv), "undefined"sv);
    CHECK(report.get_bool("callback"sv).value_or(false));
    CHECK(report.get_bool("clear_all"sv).value_or(false));
    CHECK_EQ(report.get_integer<i64>("all_after_clear_all"sv).value_or(-1), 0);
}

void alarms_fire_to_the_page_and_the_background(WebView::HeadlessWebView& view)
{
    auto report = run(view, alarms_id, "firing"sv);
    CHECK_EQ(string_in(report, "_"sv), "done"sv);
    CHECK(report.get_bool("elapsed_enough"sv).value_or(false));
    CHECK_EQ(report.get_integer<i64>("once_heard"sv).value_or(0), 1);
    CHECK_EQ(report.get_integer<i64>("when_heard"sv).value_or(0), 1);
    CHECK(report.get_bool("repeats"sv).value_or(false));
    CHECK_EQ(string_in(report, "repeat_event"sv), "repeat:0.003:number"sv);
    CHECK_EQ(string_in(report, "once_after"sv), "undefined"sv);
    CHECK_EQ(string_in(report, "repeat_after"sv), "0.003"sv);
    CHECK_EQ(string_in(report, "background_heard"sv), "once,when,repeat"sv);
    CHECK(report.get_bool("quiet_after_clear"sv).value_or(false));
}

// Alarms are kept with the profile, and each extension sees only its own.
void alarms_are_kept_and_kept_apart(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry, ByteString const& storage)
{
    auto report = run(view, alarms_id, "keep"sv);
    CHECK_EQ(string_in(report, "names"sv), "kept"sv);
    auto saved = Core::File::open(ByteString::formatted("{}/{}/alarms.json", storage, alarms_id), Core::File::OpenMode::Read);
    CHECK(!saved.is_error());
    if (!saved.is_error()) {
        auto contents = MUST(saved.value()->read_until_eof());
        CHECK(StringView { contents }.contains("\"kept\""sv));
    }

    // Another extension, with alarms from an earlier run: one still to come, one missed while the browser was closed.
    MUST(Core::Directory::create(ByteString::formatted("{}/{}", storage, other_id), Core::Directory::CreateDirectories::Yes));
    auto later = UnixDateTime::now().milliseconds_since_epoch() + 3'600'000;
    write_temporary_file(ByteString::formatted("{}/{}", storage, other_id), "alarms.json"sv,
        ByteString::formatted(R"([{{"name":"saved","scheduledTime":{},"periodInMinutes":60}},{{"name":"missed","scheduledTime":1000}},{{"name":5}}])", later));
    MUST(registry.load_unpacked(fixture("alarms_other"sv)));

    report = run(view, other_id, "other"sv);
    CHECK_EQ(string_in(report, "fired"sv), "missed"sv);
    CHECK_EQ(string_in(report, "names"sv), "saved"sv);
    CHECK_EQ(string_in(report, "saved"sv), MUST(String::formatted("{}:60", later)));
    CHECK(!report.get_bool("clear_others"sv).value_or(true));
    CHECK_EQ(string_in(report, "get_others"sv), "undefined"sv);

    report = run(view, alarms_id, "keep"sv);
    CHECK_EQ(string_in(report, "names"sv), "kept"sv);
}

void alarms_and_idle_need_their_permissions(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry)
{
    MUST(registry.load_unpacked(fixture("alarms_nopermission"sv)));
    auto report = run(view, no_permission_id, "none"sv);
    for (auto key : { "create"sv, "get_all"sv, "clear_all"sv, "query"sv, "interval"sv })
        CHECK(is_error(report, key));
    CHECK_EQ(string_in(report, "accept"sv), "true"sv);
}

struct FakeIdle {
    AK::Duration idle_time;
    bool locked { false };
    size_t queries { 0 };
};

void idle_reports_the_users_state(WebView::HeadlessWebView& view, FakeIdle& idle)
{
    auto report = run(view, alarms_id, "idle_query"sv);
    CHECK_EQ(string_in(report, "_"sv), "done"sv);
    CHECK_EQ(string_in(report, "at_15"sv), "active"sv);
    CHECK_EQ(string_in(report, "at_60"sv), "active"sv);
    CHECK_EQ(string_in(report, "at_1"sv), "active"sv);
    CHECK(is_error(report, "bad_query"sv));
    CHECK(is_error(report, "no_query"sv));
    CHECK(is_error(report, "bad_interval"sv));
    CHECK_EQ(report.get_integer<i64>("auto_lock"sv).value_or(-1), 0);
    CHECK_EQ(string_in(report, "constant"sv), "locked"sv);

    idle.idle_time = AK::Duration::from_seconds(20);
    report = run(view, alarms_id, "idle_query"sv);
    CHECK_EQ(string_in(report, "at_15"sv), "idle"sv);
    CHECK_EQ(string_in(report, "at_60"sv), "active"sv);
    // Below 15 seconds counts as 15.
    CHECK_EQ(string_in(report, "at_1"sv), "idle"sv);

    idle.locked = true;
    report = run(view, alarms_id, "idle_query"sv);
    CHECK_EQ(string_in(report, "at_15"sv), "locked"sv);
    CHECK_EQ(string_in(report, "at_60"sv), "locked"sv);
    idle.locked = false;
    idle.idle_time = {};

    // Nobody listens to idle.onStateChanged, so nobody asks.
    auto queries = idle.queries;
    wait(AK::Duration::from_milliseconds(300));
    CHECK_EQ(idle.queries, queries);
}

void idle_state_changes_reach_listeners(WebView::HeadlessWebView& view, FakeIdle& idle)
{
    auto report = run(view, alarms_id, "idle_listen"sv);
    CHECK(report.get_bool("listening"sv).value_or(false));
    wait(AK::Duration::from_milliseconds(200));
    idle.idle_time = AK::Duration::from_seconds(20);
    wait(AK::Duration::from_milliseconds(300));
    idle.locked = true;
    wait(AK::Duration::from_milliseconds(300));
    idle.locked = false;
    idle.idle_time = {};
    wait(AK::Duration::from_milliseconds(300));
    report = run(view, alarms_id, "idle_events"sv);
    CHECK_EQ(string_in(report, "states"sv), "idle,locked,active"sv);

    report = run(view, alarms_id, "idle_stop"sv);
    CHECK(!report.get_bool("listening"sv).value_or(true));
    wait(AK::Duration::from_milliseconds(100));
    auto queries = idle.queries;
    wait(AK::Duration::from_milliseconds(300));
    CHECK_EQ(idle.queries, queries);
}

void i18n_knows_the_browsers_languages(WebView::HeadlessWebView& view)
{
    WebView::Application::settings().set_languages({ "fr-CA"_string, "en"_string });
    auto report = run(view, alarms_id, "i18n"sv);
    CHECK_EQ(string_in(report, "_"sv), "done"sv);
    CHECK_EQ(string_in(report, "accept"sv), "fr-CA,en"sv);
    CHECK_EQ(string_in(report, "japanese"sv), R"({"isReliable":false,"languages":[{"language":"ja","percentage":100}]})"sv);
    CHECK_EQ(string_in(report, "english"sv), R"({"isReliable":false,"languages":[]})"sv);
    CHECK(is_error(report, "bad_detect"sv));
    CHECK_EQ(report.get_integer<i64>("callback"sv).value_or(0), 2);
}

}

// An optional permission the user grants counts as much as one the manifest asks for.
static void granted_optional_permissions_count(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry)
{
    MUST(registry.load_unpacked(fixture("alarms_optional"sv)));
    auto& runtime = WebView::Application::extension_runtime();
    runtime.request_permissions_prompt = [](auto const&, auto, auto, Function<void(bool)> on_answer) { on_answer(true); };
    auto report = report_of(view, url("chrome-extension://iocffhmcedpajhakfjjdkjoieimjgmhf/page.html"sv));
    runtime.request_permissions_prompt = nullptr;
    CHECK(is_error(report, "before"sv));
    CHECK(report.get_bool("granted"sv).value_or(false));
    CHECK_EQ(string_in(report, "after"sv), "a"sv);
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return ExtensionTests::run_extension_tests(arguments, [](ExtensionTests::Harness& harness) {
        using namespace ExtensionTests;
        auto storage = ByteString::formatted("{}/storage", harness.directory);
        WebView::Application::extension_runtime().set_storage_directory(storage);

        MUST(harness.registry.load_unpacked(fixture("alarms"sv)));
        alarms_wait_at_least_thirty_seconds_by_default(harness.view);

        WebView::set_minimum_alarm_delay_for_testing(AK::Duration::from_milliseconds(50));
        alarms_are_made_found_replaced_and_cleared(harness.view);
        alarms_fire_to_the_page_and_the_background(harness.view);
        alarms_are_kept_and_kept_apart(harness.view, harness.registry, storage);
        alarms_and_idle_need_their_permissions(harness.view, harness.registry);

        FakeIdle idle;
        WebView::set_idle_state_source_for_testing([&idle] {
            ++idle.queries;
            return WebView::IdleState { .idle_time = idle.idle_time, .screen_locked = idle.locked };
        }, AK::Duration::from_milliseconds(50));
        idle_reports_the_users_state(harness.view, idle);
        idle_state_changes_reach_listeners(harness.view, idle);

        i18n_knows_the_browsers_languages(harness.view);
        granted_optional_permissions_count(harness.view, harness.registry);
    });
}
