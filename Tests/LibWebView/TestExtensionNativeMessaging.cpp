/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Native messaging: runtime.connectNative and runtime.sendNativeMessage, with the fake hosts in
// Tests/LibWebExtensions/Fixtures/native-hosts, found only through manifests in the test's own directories.

#include "ExtensionTestHarness.h"
#include <LibWebView/ExtensionRuntime.h>

namespace ExtensionTests {

namespace {

constexpr auto extension_id = "jnbfcijbnjlihnaknfkiebdjkigpfmga"sv;
constexpr auto unpermitted_extension_id = "dkbioccblhhnpffngbnjofbhjnhhiehk"sv;

constexpr auto not_found_error = "Specified native messaging host not found."sv;
constexpr auto forbidden_error = "Access to the specified native messaging host is forbidden."sv;
constexpr auto exited_error = "Native host has exited."sv;
constexpr auto io_error = "Error when communicating with the native messaging host."sv;
constexpr auto invalid_name_error = "Invalid native messaging host name specified."sv;
constexpr auto failed_to_start_error = "Failed to start native messaging host."sv;

ByteString native_host(StringView name)
{
    auto source_directory = Core::Environment::get("LADYBIRD_SOURCE_DIR"sv);
    VERIFY(source_directory.has_value());
    return ByteString::formatted("{}/Tests/LibWebExtensions/Fixtures/native-hosts/{}", *source_directory, name);
}

void write_host_manifest(ByteString const& directory, StringView name, StringView path, StringView allowed_extension)
{
    auto json = ByteString::formatted(R"({{"name": "{}", "description": "Test host", "path": "{}", "type": "stdio", "allowed_origins": ["chrome-extension://{}/"]}})",
        name, path, allowed_extension);
    write_temporary_file(directory, ByteString::formatted("{}.json", name), json);
}

JsonObject run_case(WebView::HeadlessWebView& view, StringView which, StringView note = {})
{
    return report_of(view, url(ByteString::formatted("chrome-extension://{}/page.html?case={}&note={}", extension_id, which, note)));
}

String error_of(JsonObject const& report)
{
    return string_in(report, "error"sv);
}

bool wait_for_file(ByteString const& path)
{
    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(10);
    Core::EventLoop::current().spin_until([&] { return FileSystem::exists(path) || MonotonicTime::now() > deadline; });
    return FileSystem::exists(path);
}

void ports_talk_to_the_host(WebView::HeadlessWebView& view)
{
    auto report = run_case(view, "echo"sv);
    auto info = report.get_object("info"sv).value_or({});
    auto argv = info.get_array("argv"sv).value_or({});
    CHECK_EQ(argv.size(), 2u);
    if (argv.size() == 2) {
        CHECK_EQ(argv.at(0).as_string(), MUST(String::from_byte_string(native_host("echo-host.py"sv))));
        CHECK_EQ(argv.at(1).as_string(), MUST(String::formatted("chrome-extension://{}/", extension_id)));
    }
    // The host runs in its own directory.
    auto real_host_directory = MUST(FileSystem::real_path(LexicalPath(native_host("echo-host.py"sv)).dirname()));
    CHECK_EQ(string_in(info, "cwd"sv), MUST(String::from_byte_string(real_host_directory)));
    CHECK_EQ(report.get_object("echo"sv).value_or({}).get("echo"sv).value_or(JsonValue {}).serialized(), R"({"hello":[1,"two"]})"sv);
    CHECK_EQ(error_of(report), "(missing)"sv);
}

void hosts_are_found_in_every_directory_in_order(WebView::HeadlessWebView& view)
{
    auto report = run_case(view, "second_directory"sv);
    CHECK_EQ(report.get_object("echo"sv).value_or({}).get("echo"sv).value_or(JsonValue {}).serialized(), R"({"hi":1})"sv);
}

void many_messages_keep_their_order(WebView::HeadlessWebView& view)
{
    auto report = run_case(view, "order"sv);
    CHECK_EQ(report.get_integer<i64>("received"sv).value_or(0), 600);
    CHECK(report.get_bool("in_order"sv).value_or(false));
}

void ports_close_with_chromes_errors(WebView::HeadlessWebView& view)
{
    CHECK_EQ(error_of(run_case(view, "exits"sv)), exited_error);
    CHECK_EQ(error_of(run_case(view, "exits_later"sv)), exited_error);
    CHECK_EQ(error_of(run_case(view, "garbage"sv)), io_error);
    CHECK_EQ(error_of(run_case(view, "oversized"sv)), io_error);
    CHECK_EQ(error_of(run_case(view, "forbidden"sv)), forbidden_error);
    CHECK_EQ(error_of(run_case(view, "missing"sv)), not_found_error);
    CHECK_EQ(error_of(run_case(view, "invalid_name"sv)), invalid_name_error);
    CHECK_EQ(error_of(run_case(view, "broken"sv)), failed_to_start_error);
}

void hosts_end_with_their_port(WebView::HeadlessWebView& view, ByteString const& directory)
{
    auto note = ByteString::formatted("{}/disconnected", directory);
    auto report = run_case(view, "disconnect_ends_host"sv, note);
    CHECK(report.get_object("noted"sv).value_or({}).get_bool("noted"sv).value_or(false));
    CHECK(wait_for_file(note));

    // Leaving the page ends its hosts too.
    auto left = ByteString::formatted("{}/left", directory);
    report = run_case(view, "leaving_ends_host"sv, left);
    CHECK(report.get_object("noted"sv).value_or({}).get_bool("noted"sv).value_or(false));
    report_of(view, url(ByteString::formatted("chrome-extension://{}/page.html?case=missing", extension_id)));
    CHECK(wait_for_file(left));
}

void one_message_to_a_host(WebView::HeadlessWebView& view, ByteString const& directory)
{
    auto note = ByteString::formatted("{}/sent", directory);
    auto report = run_case(view, "send_native_message"sv, note);
    CHECK_EQ(report.get_object("echo"sv).value_or({}).get("echo"sv).value_or(JsonValue {}).serialized(), R"({"a":1})"sv);
    CHECK(report.get_object("ended"sv).value_or({}).get_bool("noted"sv).value_or(false));
    // Once it has answered, the host is ended.
    CHECK(wait_for_file(note));
    CHECK_EQ(string_in(report, "missing"sv), MUST(String::formatted("error: {}", not_found_error)));
    CHECK_EQ(string_in(report, "exits"sv), MUST(String::formatted("error: {}", exited_error)));
    CHECK_EQ(string_in(report, "not_a_name"sv), MUST(String::formatted("error: {}", invalid_name_error)));
    CHECK_EQ(string_in(report, "callback"sv), MUST(String::formatted("undefined: {}", forbidden_error)));
}

void content_scripts_have_no_native_messaging(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry, ByteString const& directory)
{
    registry.set_allows_file_access(extension_id, true);
    auto page = write_temporary_file(directory, "native-content.html"sv, "<!doctype html><body>page</body>"sv);
    auto report = report_of(view, url(ByteString::formatted("file://{}", page)));
    CHECK_EQ(string_in(report, "connect_native"sv), "undefined"sv);
    CHECK_EQ(string_in(report, "send_native_message"sv), "undefined"sv);
}

void extensions_without_the_permission_have_no_native_messaging(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry)
{
    MUST(registry.load_unpacked(fixture("native-unpermitted"sv)));
    auto report = report_of(view, url(ByteString::formatted("chrome-extension://{}/page.html", unpermitted_extension_id)));
    CHECK_EQ(string_in(report, "connect_native"sv), "undefined"sv);
    CHECK_EQ(string_in(report, "send_native_message"sv), "undefined"sv);
}

}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return ExtensionTests::run_extension_tests(arguments, [](ExtensionTests::Harness& harness) {
        using namespace ExtensionTests;

        // Only the test's own directories are searched, the first as the browser's own.
        auto first = ByteString::formatted("{}/hosts", harness.directory);
        auto second = ByteString::formatted("{}/more-hosts", harness.directory);
        MUST(Core::Directory::create(first, Core::Directory::CreateDirectories::Yes));
        MUST(Core::Directory::create(second, Core::Directory::CreateDirectories::Yes));
        WebView::Application::extension_runtime().set_native_host_directories({ first, second });

        auto echo = native_host("echo-host.py"sv);
        write_host_manifest(first, "com.ladybird.echo"sv, echo, extension_id);
        // Shadowed by the first directory's.
        write_host_manifest(second, "com.ladybird.echo"sv, native_host("exit-host.sh"sv), extension_id);
        write_host_manifest(second, "com.ladybird.second"sv, echo, extension_id);
        write_host_manifest(first, "com.ladybird.exit"sv, native_host("exit-host.sh"sv), extension_id);
        write_host_manifest(first, "com.ladybird.forbidden"sv, echo, unpermitted_extension_id);
        write_host_manifest(first, "com.ladybird.broken"sv, "/nonexistent/native-host"sv, extension_id);

        MUST(harness.registry.load_unpacked(fixture("native"sv)));

        ports_talk_to_the_host(harness.view);
        hosts_are_found_in_every_directory_in_order(harness.view);
        many_messages_keep_their_order(harness.view);
        ports_close_with_chromes_errors(harness.view);
        hosts_end_with_their_port(harness.view, harness.directory);
        one_message_to_a_host(harness.view, harness.directory);
        content_scripts_have_no_native_messaging(harness.view, harness.registry, harness.directory);
        extensions_without_the_permission_have_no_native_messaging(harness.view, harness.registry);
    });
}
