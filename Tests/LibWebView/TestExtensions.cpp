/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// chrome-extension:// URLs, the chrome.* API in extension pages, messaging between an extension's contexts, and
// content scripts.

#include "ExtensionTestHarness.h"

namespace ExtensionTests {

namespace {

void extension_pages_have_the_extension_as_their_origin(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry)
{
    auto const* extension = MUST(registry.load_unpacked(fixture("basic"sv)));
    CHECK_EQ(extension->id(), "jdpcojkcbchoigdclemcpcifojljdlhj"sv);

    auto report = report_of(view, url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/page.html"sv));
    CHECK_EQ(string_in(report, "origin"sv), "chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj"sv);
    CHECK(report.get_bool("secure"sv).value_or(false));
    CHECK_EQ(string_in(report, "module"sv), "chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/page.js"sv);
    CHECK_EQ(string_in(report, "local_storage"sv), "v"sv);
    CHECK_EQ(string_in(report, "fetch"sv), "private"sv);
}

// A web page may load what the extension makes web accessible, and nothing else.
void web_pages_reach_only_web_accessible_resources(WebView::HeadlessWebView& view, ByteString const& directory)
{
    auto page = write_temporary_file(directory, "web.html"sv, R"(<!doctype html><script>
        const base = "chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/";
        async function attempt(path) {
            try {
                return (await (await fetch(base + path)).text()).trim();
            } catch (e) {
                return "blocked";
            }
        }
        (async () => {
            document.title = JSON.stringify({
                origin: location.origin,
                shared: await attempt("public/shared.txt"),
                private: await attempt("private.txt"),
                unknown_extension: await (async () => {
                    try { await fetch("chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/public/shared.txt"); return "loaded"; }
                    catch (e) { return "blocked"; }
                })(),
            });
        })();
    </script>)"sv);

    auto report = report_of(view, url(ByteString::formatted("file://{}", page)));
    CHECK_EQ(string_in(report, "shared"sv), "shared"sv);
    CHECK_EQ(string_in(report, "private"sv), "blocked"sv);
    CHECK_EQ(string_in(report, "unknown_extension"sv), "blocked"sv);
}

// The browser serves only files inside the extension's folder, even through a link in it.
void links_out_of_the_extension_folder_are_not_followed(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry, ByteString const& directory)
{
    auto extension_directory = ByteString::formatted("{}/linked", directory);
    MUST(FileSystem::copy_file_or_directory(extension_directory, fixture("basic"sv), FileSystem::RecursionMode::Allowed));
    auto secret = write_temporary_file(directory, "secret.txt"sv, "secret"sv);
    MUST(FileSystem::remove(ByteString::formatted("{}/private.txt", extension_directory), FileSystem::RecursionMode::Disallowed));
    MUST(Core::System::symlink(secret, ByteString::formatted("{}/private.txt", extension_directory)));

    // Same key, same ID: this copy replaces the first.
    MUST(registry.load_unpacked(extension_directory));

    auto report = report_of(view, url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/page.html"sv));
    CHECK(string_in(report, "fetch"sv) != "secret"sv);
    CHECK_EQ(string_in(report, "origin"sv), "chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj"sv);

    MUST(registry.load_unpacked(fixture("basic"sv)));
}

void extension_pages_get_the_chrome_api(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry)
{
    MUST(registry.load_unpacked(fixture("api"sv)));
    auto report = report_of(view, url("chrome-extension://pckljhfmpcocnokpjgdbfakokjjhbfka/page.html"sv));

    CHECK(report.get_bool("bridge_hidden"sv).value_or(false));
    CHECK_EQ(string_in(report, "id"sv), "pckljhfmpcocnokpjgdbfakokjjhbfka"sv);
    CHECK_EQ(string_in(report, "url"sv), "chrome-extension://pckljhfmpcocnokpjgdbfakokjjhbfka/popup/index.html"sv);
    CHECK_EQ(string_in(report, "manifest_version"sv), "2.5"sv);
    CHECK_EQ(string_in(report, "message"sv), "Hello, Ada! You have 3 new $ messages."sv);
    CHECK_EQ(string_in(report, "missing_message"sv), ""sv);
#if defined(AK_OS_MACOS)
    CHECK_EQ(string_in(report, "platform"sv), "mac"sv);
#endif
    CHECK_EQ(string_in(report, "storage"sv), R"({"answer":42,"list":[1,2]})"sv);
    CHECK_EQ(report.get_integer<i64>("callback_style"sv).value_or(0), 42);
    CHECK_EQ(string_in(report, "unknown_api"sv), "rejected"sv);
    CHECK_EQ(string_in(report, "last_error"sv), "set"sv);
}

// The background runs as the extension is installed, and talks with the extension's pages.
void extension_contexts_message_each_other(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry)
{
    MUST(registry.load_unpacked(fixture("messaging"sv)));
    auto report = report_of(view, url("chrome-extension://cdolobjgkcakigekdnhmfeehpdacglmm/page.html"sv));

    CHECK_EQ(string_in(report, "installed"sv), "install"sv);
    CHECK_EQ(string_in(report, "background_url"sv), "chrome-extension://cdolobjgkcakigekdnhmfeehpdacglmm/_generated_background_page.html"sv);

    auto echo = report.get_object("echo"sv).value_or({});
    CHECK_EQ(echo.get_integer<i64>("echo"sv).value_or(0), 7);
    auto sender = echo.get_object("sender"sv).value_or({});
    CHECK_EQ(string_in(sender, "id"sv), "cdolobjgkcakigekdnhmfeehpdacglmm"sv);
    CHECK_EQ(string_in(sender, "url"sv), "chrome-extension://cdolobjgkcakigekdnhmfeehpdacglmm/page.html"sv);
    CHECK_EQ(string_in(sender, "origin"sv), "chrome-extension://cdolobjgkcakigekdnhmfeehpdacglmm"sv);

    CHECK(report.get_object("later"sv).value_or({}).get_bool("later"sv).value_or(false));
    CHECK(report.get_object("promise"sv).value_or({}).get_bool("promised"sv).value_or(false));
    CHECK_EQ(string_in(report, "unanswered"sv), "undefined"sv);
    CHECK_EQ(string_in(report, "callback"sv), "cb"sv);
    CHECK_EQ(string_in(report, "port"sv), R"([{"name":"lane","got":1},{"name":"lane","got":"two"}])"sv);
}

// Content scripts run in an isolated world of their extension's: they share the page's DOM, but not its globals.
void content_scripts_run_in_their_own_world(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry, ByteString const& directory)
{
    MUST(registry.load_unpacked(fixture("content"sv)));
    registry.set_allows_file_access("iddkbbabicabfpdhkbacpbomdcghloaj"sv, true);

    auto page = write_temporary_file(directory, "content.html"sv, R"(<!doctype html>
        <script>
            window.pageSecret = 1;
        </script>
        <body>
        <p id="secret">dom-text</p>
        <div id="host"></div>
        <script>
            document.getElementById("host").attachShadow({ mode: "closed" }).textContent = "closed-text";
            document.body.dataset.page = JSON.stringify({ chrome: typeof chrome, isolated: typeof window.isolatedValue, main: typeof window.fromMainWorld, bridge: typeof __extensionBridge });
        </script>
    )"sv);

    auto report = report_of(view, url(ByteString::formatted("file://{}", page)));
    CHECK_EQ(string_in(report, "same_world"sv), "isolated"sv);
    CHECK_EQ(string_in(report, "ready_state_at_start"sv), "loading"sv);
    CHECK_EQ(report.get_integer<i64>("page_scripts_before_start"sv).value_or(-1), 0);
    CHECK_EQ(string_in(report, "ready_state_at_end"sv), "interactive"sv);
    CHECK_EQ(string_in(report, "page_global"sv), "undefined"sv);
    CHECK_EQ(string_in(report, "main_world_global"sv), "undefined"sv);
    CHECK_EQ(string_in(report, "dom_text"sv), "dom-text"sv);
    CHECK_EQ(string_in(report, "page_view"sv), R"({"chrome":"undefined","isolated":"undefined","main":"string","bridge":"undefined"})"sv);
    CHECK_EQ(string_in(report, "id"sv), "iddkbbabicabfpdhkbacpbomdcghloaj"sv);
    CHECK_EQ(string_in(report, "has_tabs_api"sv), "undefined"sv);
    CHECK_EQ(string_in(report, "closed_shadow"sv), "closed-text"sv);

    auto background = report.get_object("background"sv).value_or({});
    CHECK_EQ(string_in(background, "heard"sv), "hi"sv);
    CHECK_EQ(string_in(background, "tab"sv), "number"sv);
    CHECK_EQ(background.get_integer<i64>("frameId"sv).value_or(-1), 0);
    CHECK_EQ(string_in(background, "url"sv), MUST(String::formatted("file://{}", page)));
}

// Without the user's say-so, an extension doesn't run in local files.
void content_scripts_stay_out_of_local_files_unless_allowed(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry, ByteString const& directory)
{
    registry.set_allows_file_access("iddkbbabicabfpdhkbacpbomdcghloaj"sv, false);
    auto page = write_temporary_file(directory, "unscripted.html"sv, R"(<!doctype html><body><script>
        setTimeout(() => { if (!document.title) document.title = JSON.stringify({ untouched: true }); }, 500);
    </script>)"sv);
    auto report = report_of(view, url(ByteString::formatted("file://{}", page)));
    CHECK(report.get_bool("untouched"sv).value_or(false));
}

}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return ExtensionTests::run_extension_tests(arguments, [](ExtensionTests::Harness& harness) {
        using namespace ExtensionTests;
        extension_pages_have_the_extension_as_their_origin(harness.view, harness.registry);
        web_pages_reach_only_web_accessible_resources(harness.view, harness.directory);
        links_out_of_the_extension_folder_are_not_followed(harness.view, harness.registry, harness.directory);
        extension_pages_get_the_chrome_api(harness.view, harness.registry);
        extension_contexts_message_each_other(harness.view, harness.registry);
        content_scripts_run_in_their_own_world(harness.view, harness.registry, harness.directory);
        content_scripts_stay_out_of_local_files_unless_allowed(harness.view, harness.registry, harness.directory);
    });
}
