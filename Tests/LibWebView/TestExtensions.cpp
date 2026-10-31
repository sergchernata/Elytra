/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// chrome-extension:// URLs, the chrome.* API in extension pages, messaging between an extension's contexts, and
// content scripts.

#include "ExtensionTestHarness.h"

#include <LibWebCommon/HTML/HistoryOperation.h>
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebCommon/Page/NavigationTarget.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalNavigation.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

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
    CHECK_EQ(string_in(report, "dynamic"sv), "dynamic"sv);
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
                dynamic: await attempt("dynamic/d.txt"),
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
    // Resources with use_dynamic_url aren't served at the extension's ID to others.
    CHECK_EQ(string_in(report, "dynamic"sv), "blocked"sv);
    CHECK_EQ(string_in(report, "unknown_extension"sv), "blocked"sv);
}

// The browser serves only files inside the extension's folder, even through a link in it.
void links_out_of_the_extension_folder_are_not_followed(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry, ByteString const& directory)
{
    auto extension_directory = ByteString::formatted("{}/linked", directory);
    MUST(copy_recursively(extension_directory, fixture("basic"sv)));
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
    // The background is a service worker to the extension: its clients are the extension's open pages.
    auto clients = report.get_object("clients"sv).value_or({});
    auto urls = clients.get_array("urls"sv).value_or({});
    CHECK_EQ(urls.size(), 1u);
    if (urls.size() == 1)
        CHECK_EQ(urls.at(0).as_string(), "chrome-extension://cdolobjgkcakigekdnhmfeehpdacglmm/page.html"sv);
    CHECK(clients.get_bool("visible"sv).value_or(false));
    CHECK_EQ(string_in(clients, "missing"sv), "undefined"sv);
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

// What a content script hands the page through the DOM, an event's detail or an event handler, doesn't take the page
// into the content script's world (and from there to the extension's chrome.* API), nor the other way around. As in
// Chrome, a value crosses worlds as a structured clone, a node as that world's wrapper of it, and each world has event
// handlers of its own.
void worlds_dont_reach_each_other_through_the_dom(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry, ByteString const& directory)
{
    MUST(registry.load_unpacked(fixture("isolation"sv)));
    registry.set_allows_file_access("ppghcjnaogdmoiaglepddebjbdefbkbf"sv, true);

    auto page = write_temporary_file(directory, "isolation.html"sv, R"(<!doctype html>
        <body>
        <p id="target">target</p>
        <script>
            window.pageSecret = "page";
            const report = { errors: [], rejections: [] };
            const kindOf = value => {
                if (value === null || value === undefined)
                    return String(value);
                if (typeof value !== "object" && typeof value !== "function")
                    return typeof value;
                return value instanceof Object ? "own" : "foreign";
            };
            const escapeThrough = value => {
                try {
                    const fn = value.constructor.constructor;
                    return String(fn("return typeof chrome === 'object' && chrome.runtime ? 'chrome:' + chrome.runtime.id : typeof isolatedMarker")());
                } catch (e) {
                    return "threw";
                }
            };
            let pageClicks = 0;
            const pageHandler = () => { pageClicks++; };
            document.body.onclick = pageHandler;

            document.addEventListener("from-content", event => {
                const detail = event.detail;
                report.detail = kindOf(detail);
                report.detail_value = detail && detail.nested && detail.nested.value;
                report.detail_escape = detail ? escapeThrough(detail) : "none";
            });
            document.addEventListener("from-content-node", event => {
                report.node_detail = kindOf(event.detail);
                report.node_is_target = event.detail === document.getElementById("target");
            });
            window.addEventListener("error", event => {
                report.errors.push(event.message);
                if (event.error)
                    report.error_escape = escapeThrough(event.error);
            });
            window.addEventListener("unhandledrejection", event => {
                report.rejections.push(String(event.reason && event.reason.message));
                report.rejection_escape = escapeThrough(event.promise);
            });
            document.addEventListener("content-ready", () => {
                // The content script set its own onclick: the page still sees, and runs, its own.
                report.onclick = document.body.onclick === pageHandler ? "page" : kindOf(document.body.onclick);
                if (document.body.onclick && document.body.onclick !== pageHandler)
                    report.onclick_escape = escapeThrough(document.body.onclick);
                document.body.click();
                // Clearing the page's handler leaves the content script's alone.
                document.body.onclick = null;
                document.body.click();
                report.page_clicks = pageClicks;

                document.dispatchEvent(new CustomEvent("to-content", { detail: { nested: { value: 7 } } }));
                document.dispatchEvent(new CustomEvent("to-content-function", { detail: { fn() {} } }));
                document.dispatchEvent(new CustomEvent("to-content-node", { detail: document.getElementById("target") }));
                document.dispatchEvent(new CustomEvent("page-done"));
                setTimeout(() => {
                    report.content = JSON.parse(document.documentElement.dataset.content || "{}");
                    document.title = JSON.stringify(report);
                }, 50);
            });
        </script>
    )"sv);

    auto report = report_of(view, url(ByteString::formatted("file://{}", page)));

    // The content script's objects reach the page as the page's own copies, which lead nowhere.
    CHECK_EQ(string_in(report, "detail"sv), "own"sv);
    CHECK_EQ(report.get_integer<i64>("detail_value"sv).value_or(0), 42);
    CHECK_EQ(string_in(report, "detail_escape"sv), "undefined"sv);
    CHECK_EQ(string_in(report, "node_detail"sv), "own"sv);
    CHECK(report.get_bool("node_is_target"sv).value_or(false));

    // Each world has its own onclick.
    CHECK_EQ(string_in(report, "onclick"sv), "page"sv);
    CHECK(!report.has("onclick_escape"sv));
    CHECK_EQ(report.get_integer<i64>("page_clicks"sv).value_or(-1), 1);

    // The content script's errors and rejections stay in its world.
    auto heard = [&](StringView key) { return report.get_array(key).has_value() ? report.get_array(key)->serialized() : "(none)"_string; };
    CHECK_EQ(heard("errors"sv), "[]"sv);
    CHECK_EQ(heard("rejections"sv), "[]"sv);
    CHECK(!report.has("error_escape"sv));
    CHECK(!report.has("rejection_escape"sv));

    // And the other way: what the page hands the content script is the content script's own.
    auto content = report.get_object("content"sv).value_or({});
    CHECK_EQ(string_in(content, "page_onclick"sv), "null"sv);
    CHECK_EQ(string_in(content, "own_onclick"sv), "own"sv);
    CHECK_EQ(content.get_integer<i64>("content_clicks"sv).value_or(-1), 2);
    CHECK_EQ(string_in(content, "page_detail"sv), "own"sv);
    CHECK_EQ(content.get_integer<i64>("page_detail_value"sv).value_or(0), 7);
    CHECK_EQ(string_in(content, "page_detail_escape"sv), "undefined"sv);
    CHECK_EQ(string_in(content, "page_function_detail"sv), "null"sv);
    CHECK_EQ(string_in(content, "page_node_detail"sv), "own"sv);
    CHECK_EQ(string_in(content, "page_node_id"sv), "target"sv);
    CHECK_EQ(string_in(content, "page_global"sv), "undefined"sv);

    // chrome-api.js keeps to the builtins it started with, should something replace them in its world.
    CHECK_EQ(string_in(content, "background_echo"sv), "ping"sv);
    CHECK_EQ(string_in(content, "intercepted"sv), ""sv);

    registry.set_allows_file_access("ppghcjnaogdmoiaglepddebjbdefbkbf"sv, false);
}

// What the engine makes for a content script's code (an observer's records, a promise an API returns, what it throws)
// is the content script's world's: a page that replaced its own world's builtins is never handed the content script's
// callbacks by them, which would lead it to chrome.*.
void worlds_dont_reach_each_other_through_what_the_engine_makes(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry, ByteString const& directory)
{
    registry.set_allows_file_access("ppghcjnaogdmoiaglepddebjbdefbkbf"sv, true);

    auto page = write_temporary_file(directory, "isolation-engine.html"sv, R"(<!doctype html>
        <body>
        <div id="box" style="width: 50px; height: 50px; background: green"></div>
        <iframe name="child" srcdoc="<p>child</p>"></iframe>
        <script>
            const stolen = [];
            const escapeThrough = value => {
                try {
                    const fn = value.constructor.constructor;
                    return String(fn("return typeof chrome === 'object' && chrome.runtime ? 'chrome:' + chrome.runtime.id : typeof isolatedMarker")());
                } catch (e) {
                    return "threw";
                }
            };
            const hook = (object, name, label) => {
                const original = object[name];
                object[name] = function (...args) {
                    // What leads anywhere but this world is the content script's.
                    for (const arg of args) {
                        if ((typeof arg === "function" || (arg && typeof arg === "object")) && escapeThrough(arg) !== "undefined")
                            stolen.push(label + ":" + escapeThrough(arg));
                    }
                    return original.apply(this, args);
                };
            };
            hook(Array.prototype, "forEach", "Array.forEach");
            hook(NodeList.prototype, "forEach", "NodeList.forEach");
            hook(Promise.prototype, "then", "Promise.then");
            hook(Promise.prototype, "catch", "Promise.catch");
            hook(Promise.prototype, "finally", "Promise.finally");
            hook(EventTarget.prototype, "addEventListener", "addEventListener");
            for (const name of ["value", "done", "then", "list", "secret", "why"]) {
                Object.defineProperty(Object.prototype, name, {
                    __proto__: null,
                    set(v) {
                        if (v && (typeof v === "object" || typeof v === "function") && escapeThrough(v) !== "undefined")
                            stolen.push("set " + name + ":" + escapeThrough(v));
                        Object.defineProperty(this, name, { __proto__: null, value: v, writable: true, enumerable: true, configurable: true });
                    },
                    configurable: true,
                });
            }
            const pageController = new AbortController();
            pageController.abort({ why: "page" });
            const report = {};
            document.addEventListener("content-wants-signal", () => {
                document.dispatchEvent(new CustomEvent("page-signal", { detail: pageController.signal }));
            });
            document.addEventListener("content-done", event => {
                const [signal, walker] = event.detail || [];
                try {
                    signal.throwIfAborted();
                    report.abort = "none";
                } catch (e) {
                    report.abort = escapeThrough(e);
                }
                report.filter = walker ? String(walker.filter) : "none";
                report.opener = String(window.opener);
                report.stolen = stolen.join(" ");
                report.timer = document.documentElement.dataset.timer || "none";
                report.content = JSON.parse(document.documentElement.dataset.engine || "{}");
                document.title = JSON.stringify(report);
            });
        </script>
    )"sv);

    auto report = report_of(view, url(ByteString::formatted("file://{}", page)));

    // The page got nothing of the content script's world.
    CHECK_EQ(string_in(report, "stolen"sv), ""sv);
    CHECK_EQ(string_in(report, "abort"sv), "undefined"sv);
    CHECK_EQ(string_in(report, "filter"sv), "null"sv);
    CHECK_EQ(string_in(report, "opener"sv), "null"sv);
    // And the content script's code ran in its world, with what it got its own.
    CHECK_EQ(string_in(report, "timer"sv), "isolated"sv);
    auto content = report.get_object("content"sv).value_or({});
    CHECK_EQ(string_in(content, "mutation"sv), "true,true,true,true,true"sv);
    CHECK_EQ(string_in(content, "resize"sv), "true,true,true"sv);
    CHECK_EQ(string_in(content, "intersection"sv), "true,true,true"sv);
    CHECK_EQ(string_in(content, "performance"sv), "true,true,true,true"sv);
    CHECK_EQ(string_in(content, "animation_frame"sv), "number"sv);
    for (auto name : { "replace"sv, "fonts"sv, "finished"sv, "json"sv, "read"sv })
        CHECK_EQ(string_in(content, name), "true,fulfilled:true"sv);
    CHECK_EQ(string_in(content, "decode"sv), "true,rejected:true"sv);
    for (auto name : { "estimate"sv, "databases"sv })
        CHECK(string_in(content, name).starts_with_bytes("true,"sv) && !string_in(content, name).contains("false"sv));
    CHECK(content.get_bool("json_list"sv).value_or(false));
    CHECK(content.get_bool("read_value"sv).value_or(false));
    CHECK_EQ(string_in(content, "page_abort"sv), "true,page"sv);
    CHECK(content.get_bool("own_filter"sv).value_or(false));
    CHECK_EQ(string_in(content, "frame"sv), "1,true,true,true"sv);
    CHECK_EQ(string_in(content, "opener"sv), "content"sv);

    registry.set_allows_file_access("ppghcjnaogdmoiaglepddebjbdefbkbf"sv, false);
}

// Without the user's say-so, an extension doesn't run in local files.
// An SVG image has a document of its own, in a hidden page. As in Chrome, content scripts don't run in it, even ones
// for all frames and every URL; starting one there used to crash the page's process.
void content_scripts_stay_out_of_svg_images(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry, ByteString const& directory)
{
    registry.set_allows_file_access("iddkbbabicabfpdhkbacpbomdcghloaj"sv, true);
    write_temporary_file(directory, "picture.svg"sv, R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="30"><rect width="40" height="30" fill="green"/></svg>)"sv);
    // Named so the fixture's end.js, which would report in the title, leaves it alone.
    auto page = write_temporary_file(directory, "excluded.html"sv, R"html(<!doctype html><body>
        <img src="picture.svg" onload="setTimeout(() => document.title = JSON.stringify({ width: this.naturalWidth, isolated: typeof window.isolatedValue }), 200)">
    )html"sv);
    auto report = report_of(view, url(ByteString::formatted("file://{}", page)));
    CHECK_EQ(report.get_integer<i64>("width"sv).value_or(-1), 40);
    CHECK_EQ(string_in(report, "isolated"sv), "undefined"sv);
}

void content_scripts_stay_out_of_local_files_unless_allowed(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry, ByteString const& directory)
{
    registry.set_allows_file_access("iddkbbabicabfpdhkbacpbomdcghloaj"sv, false);
    auto page = write_temporary_file(directory, "unscripted.html"sv, R"(<!doctype html><body><script>
        setTimeout(() => { if (!document.title) document.title = JSON.stringify({ untouched: true }); }, 500);
    </script>)"sv);
    auto report = report_of(view, url(ByteString::formatted("file://{}", page)));
    CHECK(report.get_bool("untouched"sv).value_or(false));
}

// Extension pages run under the extension's CSP, and under Chrome's minimum whatever the manifest says.
void extension_pages_have_their_content_security_policy(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry)
{
    MUST(registry.load_unpacked(fixture("csp"sv)));
    auto report = report_of(view, url("chrome-extension://kbhmgdahlmkgoidkomhgedeonohlkhmn/page.html"sv));
    CHECK(!report.get_bool("inline"sv).value_or(true));
    CHECK_EQ(string_in(report, "eval"sv), "blocked"sv);
    CHECK_EQ(string_in(report, "wasm"sv), "compiled"sv);
    CHECK_EQ(string_in(report, "image"sv), "blocked"sv);
    CHECK_EQ(string_in(report, "own_file"sv), "fetched"sv);
    CHECK_EQ(string_in(report, "elsewhere"sv), "blocked"sv);
}

// An extension's pages get processes of their own, which host nothing else: a process that may speak for an extension
// mustn't run what a site made.
void extension_pages_have_processes_of_their_own(WebView::HeadlessWebView& view, ByteString const& directory)
{
    (void)report_of(view, url("chrome-extension://pckljhfmpcocnokpjgdbfakokjjhbfka/page.html"sv));
    auto& extension_process = view.page().client();
    CHECK_EQ(extension_process.hosted_extension().value_or("(none)"_string), "pckljhfmpcocnokpjgdbfakokjjhbfka"sv);

    (void)directory;
    (void)report_of(view, url("data:text/html,<script>document.title = JSON.stringify({ plain: true });</script>"sv));
    CHECK(&view.page().client() != &extension_process);
    CHECK(!view.page().client().hosted_extension().has_value());
}


constexpr auto basic_id = "jdpcojkcbchoigdclemcpcifojljdlhj"sv;

URL::Origin origin_of(StringView string)
{
    return url(string).origin();
}

// Waits for the view's page to put a JSON report in its title, without loading anything.
Optional<JsonObject> next_report(WebView::HeadlessWebView& view, Function<void()> start, AK::Duration timeout)
{
    Optional<String> report;
    view.on_title_change = [&](Utf16String const& title) {
        auto text = title.to_utf8();
        if (text.starts_with('{'))
            report = text;
    };
    start();
    auto deadline = MonotonicTime::now() + timeout;
    auto wake = Core::Timer::create_repeating(AK::Duration::from_milliseconds(100), [] { });
    wake->start();
    Core::EventLoop::current().spin_until([&] { return report.has_value() || MonotonicTime::now() > deadline; });
    wake->stop();
    view.on_title_change = nullptr;
    if (!report.has_value())
        return {};
    auto json = JsonParser::parse(*report);
    VERIFY(!json.is_error() && json.value().is_object());
    return json.value().as_object();
}

// Plays a compromised renderer: it starts a navigation of its tab to the URL, as one without a source document, and
// says it has run the unload check. The navigation then waits for the renderer's navigation params.
Optional<Utf16String> admit_navigation_as_a_renderer(WebView::HeadlessWebView& view, URL::URL const& admitted_url)
{
    auto& stub = static_cast<WebContentClientStub&>(view.page().client());
    auto page_id = view.page().id();
    auto& traversable = view.traversable();
    auto uuid = generate_random_uuid();
    auto navigation_id = Utf16String::from_utf8_without_validation(uuid.bytes());
    Web::HTML::NavigationStartRequest request {
        .navigable_id = traversable.id(),
        .url = admitted_url,
        .document_resource = {},
        .request_referrer = Web::Fetch::Infrastructure::RequestReferrer::Client,
        .request_referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::EmptyString,
        .initiator_origin = URL::Origin::create_opaque(),
        .initiator_base_url = {},
        .navigable_target_name = {},
        .source_snapshot_params = Web::HTML::create_navigation_source_snapshot_without_a_source_document(),
        .target_snapshot_params = {},
        .csp_navigation_type = Web::ContentSecurityPolicy::Directives::NavigationType::Other,
        .history_handling = Web::Bindings::NavigationHistoryBehavior::Replace,
        .user_involvement = Web::HTML::UserNavigationInvolvement::None,
        .navigation_id = navigation_id,
        .classic_history_api_state = {},
        .navigation_api_state = {},
        .navigation_api_key = {},
        .navigation_api_id = {},
        .directive_state_id = {},
        .directive_state_value = {},
    };
    stub.did_request_navigation_start(page_id, traversable.id(), Web::NavigationTarget::TopLevel, admitted_url, navigation_id, move(request));
    if (!traversable.navigation_with_id(navigation_id))
        return {};
    stub.did_complete_navigation_unload_check(page_id, traversable.id(), navigation_id);
    auto navigation = traversable.navigation_with_id(navigation_id);
    if (!navigation || !navigation->state.has<WebView::CanonicalNavigation::CreatingNavigationParams>())
        return {};
    return navigation_id;
}

// ...and reports navigation params of its own making: the response's URL list, a body, and neither headers nor a CSP.
void report_navigation_params_as_a_renderer(WebView::HeadlessWebView& view, Utf16String const& navigation_id, Vector<URL::URL> url_list, StringView body, Optional<URL::URL> redirected_url = {}, Optional<Web::HTML::SessionHistoryDocumentStateDescriptor> replacement_document_state = {})
{
    auto& stub = static_cast<WebContentClientStub&>(view.page().client());
    auto& traversable = view.traversable();
    auto const& final_url = url_list.last();
    Web::HTML::NavigationParamsDescriptor params {
        .id = navigation_id,
        .navigable_id = traversable.id(),
        .request = {},
        .response = {},
        .fetch_timing_info = {},
        .coop_enforcement_result = { .url = final_url, .origin = final_url.origin(), .opener_policy = {} },
        .reserved_environment = {},
        .origin = final_url.origin(),
        .policy_container = {},
        .opener_policy = {},
        .about_base_url = {},
        .agent_cluster_id = {},
        .new_browsing_context_group_id = {},
    };
    params.response.url_list = url_list;
    params.response.status = 200;
    params.response.headers.append({ "Content-Type"sv, "text/html"sv });
    params.response.body = MUST(ByteBuffer::copy(body.bytes()));
    stub.did_finish_navigation_params_creation(view.page().id(), traversable.id(), navigation_id,
        Web::HTML::NavigationPopulationResult {
            .navigation_params = move(params),
            .redirected_url = move(redirected_url),
            .classic_history_api_state = {},
            .replacement_document_state = move(replacement_document_state),
        });
}

constexpr auto forged_body = R"(<!doctype html><script>
    document.title = JSON.stringify({ forged: true, origin: location.origin, chrome: typeof chrome, storage: typeof chrome?.storage?.local });
</script>)"sv;

bool active_document_is_the_extensions(WebView::HeadlessWebView& view, StringView id)
{
    auto const& origin = view.traversable().active_document().origin();
    return !origin.is_opaque() && origin.scheme() == "chrome-extension"sv && origin.host().serialize() == id;
}

// A renderer reports the URLs its fetch reached, but a document of a site's making mustn't become an extension's page:
// the browser takes an extension's page from the extension's files, with the extension's CSP, or makes none.
void renderers_cannot_make_their_documents_extension_pages(WebView::HeadlessWebView& view, ByteString const& directory)
{
    auto web_page = write_temporary_file(directory, "attacker.html"sv, R"(<!doctype html><script>document.title = JSON.stringify({ attacker: true });</script>)"sv);

    // A navigation to a site, which the renderer says ended at a page of the extension.
    (void)report_of(view, url(ByteString::formatted("file://{}", web_page)));
    auto navigation_id = admit_navigation_as_a_renderer(view, url("https://evil.example/"sv));
    CHECK(navigation_id.has_value());
    if (navigation_id.has_value()) {
        auto report = next_report(view, [&] {
            report_navigation_params_as_a_renderer(view, *navigation_id, { url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/page.html"sv) }, forged_body);
        },
            AK::Duration::from_seconds(3));
        CHECK(!report.has_value() || !report->get_bool("forged"sv).value_or(false));
        CHECK(!active_document_is_the_extensions(view, basic_id));
        CHECK(view.page().client().hosted_extension().value_or(String {}) != basic_id);
    }

    // The same, through a made-up redirect.
    (void)report_of(view, url(ByteString::formatted("file://{}", web_page)));
    navigation_id = admit_navigation_as_a_renderer(view, url("https://evil.example/"sv));
    CHECK(navigation_id.has_value());
    if (navigation_id.has_value()) {
        auto report = next_report(view, [&] {
            report_navigation_params_as_a_renderer(view, *navigation_id, { url("https://evil.example/"sv), url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/page.html"sv) }, forged_body);
        },
            AK::Duration::from_seconds(3));
        CHECK(!report.has_value() || !report->get_bool("forged"sv).value_or(false));
        CHECK(!active_document_is_the_extensions(view, basic_id));
    }

    // A navigation to a web accessible page of the extension, which the renderer gives a body and policies of its own.
    (void)report_of(view, url(ByteString::formatted("file://{}", web_page)));
    navigation_id = admit_navigation_as_a_renderer(view, url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/public/war.html"sv));
    CHECK(navigation_id.has_value());
    if (navigation_id.has_value()) {
        auto report = next_report(view, [&] {
            report_navigation_params_as_a_renderer(view, *navigation_id, { url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/public/war.html"sv) }, forged_body);
        },
            AK::Duration::from_seconds(10));
        CHECK(report.has_value());
        if (report.has_value()) {
            CHECK(!report->get_bool("forged"sv).value_or(false));
            CHECK(report->get_bool("real"sv).value_or(false));
            CHECK_EQ(string_in(*report, "origin"sv), "chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj"sv);
            CHECK_EQ(string_in(*report, "eval"sv), "blocked"sv);
        }
    }

    // ...or the body of another of its pages, which the page's navigation never named.
    (void)report_of(view, url(ByteString::formatted("file://{}", web_page)));
    navigation_id = admit_navigation_as_a_renderer(view, url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/public/war.html"sv));
    CHECK(navigation_id.has_value());
    if (navigation_id.has_value()) {
        auto report = next_report(view, [&] {
            report_navigation_params_as_a_renderer(view, *navigation_id, { url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/page.html"sv) }, forged_body);
        },
            AK::Duration::from_seconds(3));
        CHECK(!report.has_value() || (!report->get_bool("forged"sv).value_or(false) && !report->has("module"sv)));
        CHECK(!active_document_is_the_extensions(view, basic_id));
    }

    // A navigation to a page that isn't web accessible, from a renderer: the browser refuses it.
    (void)report_of(view, url(ByteString::formatted("file://{}", web_page)));
    navigation_id = admit_navigation_as_a_renderer(view, url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/page.html"sv));
    CHECK(navigation_id.has_value());
    if (navigation_id.has_value()) {
        auto report = next_report(view, [&] {
            report_navigation_params_as_a_renderer(view, *navigation_id, { url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/page.html"sv) }, forged_body);
        },
            AK::Duration::from_seconds(3));
        CHECK(!report.has_value() || (!report->get_bool("forged"sv).value_or(false) && !report->has("module"sv)));
        CHECK(!active_document_is_the_extensions(view, basic_id));
    }
}

// The renderer populating a navigation to an extension's file doesn't get to say whom it reads the file for.
void renderers_populating_extension_navigations_get_only_what_their_initiator_may(WebView::HeadlessWebView& view, ByteString const& directory)
{
    auto web_page = write_temporary_file(directory, "populating.html"sv, R"(<!doctype html><script>document.title = JSON.stringify({ web: true });</script>)"sv);
    (void)report_of(view, url(ByteString::formatted("file://{}", web_page)));

    auto navigation_id = admit_navigation_as_a_renderer(view, url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/private.txt"sv));
    CHECK(navigation_id.has_value());
    auto file = view.page().extension_file_for_renderer(basic_id, "/private.txt"sv, {});
    CHECK(file.is_error());
    if (!file.is_error()) {
        auto contents = MUST(MUST(Core::File::adopt_fd(file.value().take_fd(), Core::File::OpenMode::Read))->read_until_eof());
        CHECK(!StringView { contents.bytes() }.contains("private"sv));
    }
    file = view.page().extension_file_for_renderer(basic_id, "/private.txt"sv, origin_of("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/"sv));
    CHECK(file.is_error());

    // What the navigation's initiator may load, the renderer may fetch, though the browser reads the document itself.
    (void)report_of(view, url(ByteString::formatted("file://{}", web_page)));
    navigation_id = admit_navigation_as_a_renderer(view, url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/public/shared.txt"sv));
    CHECK(navigation_id.has_value());
    CHECK(!view.page().extension_file_for_renderer(basic_id, "/public/shared.txt"sv, {}).is_error());
    (void)report_of(view, url(ByteString::formatted("file://{}", web_page)));
}

// A renderer names the origin it loads an extension's file for, but the browser knows which documents it hosts.
void renderers_cannot_claim_another_requester(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry, ByteString const& directory)
{
    MUST(registry.load_unpacked(fixture("api"sv)));
    auto web_page = write_temporary_file(directory, "claims.html"sv, R"(<!doctype html><script>document.title = JSON.stringify({ web: true });</script>)"sv);
    (void)report_of(view, url(ByteString::formatted("file://{}", web_page)));

    // Another extension that may load the file, and a site that may.
    CHECK(view.page().extension_file_for_renderer(basic_id, "/friends/f.txt"sv, origin_of("chrome-extension://pckljhfmpcocnokpjgdbfakokjjhbfka/"sv)).is_error());
    CHECK(view.page().extension_file_for_renderer(basic_id, "/friends/f.txt"sv, origin_of("https://friend.example/"sv)).is_error());
    CHECK(view.page().extension_file_for_renderer(basic_id, "/private.txt"sv, origin_of("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/"sv)).is_error());
    CHECK(!view.page().extension_file_for_renderer(basic_id, "/public/shared.txt"sv, origin_of("https://elsewhere.example/"sv)).is_error());

    // The other extension's own page may.
    (void)report_of(view, url("chrome-extension://pckljhfmpcocnokpjgdbfakokjjhbfka/page.html"sv));
    CHECK(!view.page().extension_file_for_renderer(basic_id, "/friends/f.txt"sv, origin_of("chrome-extension://pckljhfmpcocnokpjgdbfakokjjhbfka/"sv)).is_error());
    CHECK(view.page().extension_file_for_renderer(basic_id, "/private.txt"sv, origin_of("chrome-extension://pckljhfmpcocnokpjgdbfakokjjhbfka/"sv)).is_error());
    CHECK(view.page().extension_file_for_renderer(basic_id, "/friends/f.txt"sv, origin_of("https://friend.example/"sv)).is_error());
}

// Web pages embed what an extension makes web accessible to them, and nothing else.
void web_pages_embed_only_web_accessible_pages(WebView::HeadlessWebView& view, ByteString const& directory)
{
    auto page = write_temporary_file(directory, "embedding.html"sv, R"(<!doctype html><body><script>
        const heard = [];
        window.addEventListener("message", event => heard.push(JSON.parse(event.data)));
        for (const path of ["public/war.html", "page.html"]) {
            const frame = document.createElement("iframe");
            frame.src = "chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/" + path;
            document.body.appendChild(frame);
        }
        setTimeout(() => { document.title = JSON.stringify({ heard }); }, 3000);
    </script>)"sv);
    auto report = report_of(view, url(ByteString::formatted("file://{}", page)));

    // Only the web accessible page became the extension's.
    Vector<URL::URL> extension_documents;
    view.traversable().for_each_in_inclusive_subtree([&](WebView::CanonicalNavigable const& navigable) {
        auto const& origin = navigable.active_document().origin();
        if (!origin.is_opaque() && origin.scheme() == "chrome-extension"sv)
            extension_documents.append(navigable.active_document().creation_url());
        return IterationDecision::Continue;
    });
    CHECK_EQ(extension_documents.size(), 1u);
    if (extension_documents.size() == 1)
        CHECK_EQ(extension_documents.first().serialize(), "chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/public/war.html"sv);

    auto heard = report.get_array("heard"sv);
    CHECK(heard.has_value());
    if (!heard.has_value())
        return;
    CHECK_EQ(heard->size(), 1u);
    if (heard->size() != 1)
        return;
    auto const& frame = heard->at(0).as_object();
    CHECK(frame.get_bool("real"sv).value_or(false));
    CHECK_EQ(string_in(frame, "origin"sv), "chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj"sv);
    CHECK_EQ(string_in(frame, "chrome"sv), "object"sv);
    CHECK_EQ(string_in(frame, "eval"sv), "blocked"sv);
}

// An about:blank document takes its initiator's origin. A renderer that says a redirect gave the navigation a document
// state with an extension's (or another site's) initiator, and ended at about:blank, doesn't make its own document one
// of that origin.
void renderers_cannot_make_their_documents_any_origin_through_about_blank(WebView::HeadlessWebView& view, ByteString const& directory)
{
    auto web_page = write_temporary_file(directory, "blank-attacker.html"sv, R"(<!doctype html><script>document.title = JSON.stringify({ attacker: true });</script>)"sv);
    for (auto forged_initiator : { "chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/"sv, "https://victim.example/"sv }) {
        (void)report_of(view, url(ByteString::formatted("file://{}", web_page)));
        auto navigation_id = admit_navigation_as_a_renderer(view, url("https://evil.example/"sv));
        CHECK(navigation_id.has_value());
        if (!navigation_id.has_value())
            continue;
        Web::HTML::SessionHistoryDocumentStateDescriptor forged {};
        forged.id = WebView::Application::the().allocate_ui_process_cross_process_id();
        forged.initiator_origin = origin_of(forged_initiator);
        forged.origin = origin_of(forged_initiator);
        auto report = next_report(view, [&] {
            report_navigation_params_as_a_renderer(view, *navigation_id, { url("https://evil.example/"sv), URL::about_blank() }, forged_body, URL::about_blank(), forged);
        },
            AK::Duration::from_seconds(3));
        CHECK(!report.has_value() || !report->get_bool("forged"sv).value_or(false));
        CHECK(!view.traversable().active_document().origin().is_same_origin(origin_of(forged_initiator)));
        CHECK(view.page().client().hosted_extension().value_or(String {}) != basic_id);
    }
}

// A link the user clicks is the page's navigation, which a renderer can't pass off as the browser's own: that would
// read any of an extension's files for it.
void renderers_cannot_click_through_to_extension_pages(WebView::HeadlessWebView& view, ByteString const& directory)
{
    auto web_page = write_temporary_file(directory, "clicking.html"sv, R"(<!doctype html><script>document.title = JSON.stringify({ web: true });</script>)"sv);
    (void)report_of(view, url(ByteString::formatted("file://{}", web_page)));
    auto& stub = static_cast<WebContentClientStub&>(view.page().client());
    auto uuid = generate_random_uuid();
    Web::HTML::PreparedNavigationDescriptor navigation {
        .url = url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/page.html"sv),
        .document_resource = {},
        .history_handling = Web::Bindings::NavigationHistoryBehavior::Auto,
        .navigation_api_state = {},
        .referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::EmptyString,
        .user_involvement = Web::HTML::UserNavigationInvolvement::Activation,
        .navigation_id = Utf16String::from_utf8_without_validation(uuid.bytes()),
        .initial_insertion = Web::HTML::InitialInsertion::No,
        .csp_navigation_type = Web::ContentSecurityPolicy::Directives::NavigationType::Other,
        .source_snapshot_params = Web::HTML::create_navigation_source_snapshot_without_a_source_document(),
        .initiator_origin_snapshot = URL::Origin::create_opaque(),
        .initiator_base_url_snapshot = URL::about_blank(),
    };
    auto report = next_report(view, [&] { stub.did_click_link(view.page().id(), move(navigation), {}, 0); }, AK::Duration::from_seconds(5));
    CHECK(!report.has_value() || !report->has("module"sv));
    CHECK(!active_document_is_the_extensions(view, basic_id));
}

// A renderer reports the URL a pushState() or fragment navigation gives its entry, which can't change its origin. One
// naming an extension's file mustn't show it as the tab's, nor have the browser read the file for a reload.
void renderers_cannot_rewrite_their_entry_to_an_extension_url()
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto view = WebView::HeadlessWebView::create(MUST(Gfx::load_system_theme(theme_path.string())), { 800, 600 });
    // The view's initial about:blank document is in place once its process is.
    auto wake = Core::Timer::create_repeating(AK::Duration::from_milliseconds(100), [] { });
    wake->start();
    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(2);
    Core::EventLoop::current().spin_until([&] { return MonotonicTime::now() > deadline; });

    // The initial about:blank entry of a top-level traversable, which has no initiator.
    auto& traversable = view->traversable();
    auto const& active_entry = *traversable.active_session_history_entry();
    auto& stub = static_cast<WebContentClientStub&>(view->page().client());
    auto secret_url = url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/private.txt"sv);
    Web::HTML::SameDocumentNavigationEntry entry {};
    entry.url = secret_url;
    entry.document_state_id = active_entry.document_state->id;
    entry.navigation_api_key = active_entry.navigation_api_key;
    entry.navigation_api_id = active_entry.navigation_api_id;
    stub.request_history_operation(view->page().id(), WebView::Application::the().allocate_ui_process_cross_process_id(),
        Web::FinalizeSameDocumentNavigationHistoryOperationParameters {
            .navigable_id = traversable.id(),
            .target_entry = move(entry),
            .entry_to_replace = active_entry.identity(),
            .previous_entry_persisted_state = {},
            .history_handling = Web::HTML::HistoryHandlingBehavior::Replace,
            .user_involvement = Web::HTML::UserNavigationInvolvement::None,
        });
    deadline = MonotonicTime::now() + AK::Duration::from_seconds(1);
    Core::EventLoop::current().spin_until([&] { return MonotonicTime::now() > deadline; });
    CHECK(traversable.active_session_history_entry()->url != secret_url);
    CHECK(view->url() != secret_url);

    view->reload();
    deadline = MonotonicTime::now() + AK::Duration::from_seconds(3);
    Core::EventLoop::current().spin_until([&] { return MonotonicTime::now() > deadline; });
    wake->stop();
    CHECK(!active_document_is_the_extensions(*view, basic_id));
}

// A navigation to an extension's file that isn't a document downloads the file the browser read.
void extension_files_download_as_they_are(WebView::HeadlessWebView& view, ByteString const& directory)
{
    auto downloads_directory = ByteString::formatted("{}/NavigationDownloads", directory);
    MUST(Core::Directory::create(downloads_directory, Core::Directory::CreateDirectories::Yes));
    MUST(Core::Environment::set("XDG_DOWNLOAD_DIR"sv, downloads_directory, Core::Environment::Overwrite::Yes));
    auto downloaded = ByteString::formatted("{}/download.bin", downloads_directory);
    auto expected = MUST(MUST(Core::File::open(ByteString::formatted("{}/download.bin", fixture("basic"sv)), Core::File::OpenMode::Read))->read_until_eof());

    view.load(url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/download.bin"sv));
    auto contents_of_download = [&]() -> Optional<ByteBuffer> {
        if (!FileSystem::exists(downloaded))
            return {};
        auto file = Core::File::open(downloaded, Core::File::OpenMode::Read);
        if (file.is_error())
            return {};
        auto contents = file.value()->read_until_eof();
        if (contents.is_error())
            return {};
        return contents.release_value();
    };
    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(10);
    auto wake = Core::Timer::create_repeating(AK::Duration::from_milliseconds(100), [] { });
    wake->start();
    Core::EventLoop::current().spin_until([&] {
        auto contents = contents_of_download();
        return (contents.has_value() && contents->size() == expected.size()) || MonotonicTime::now() > deadline;
    });
    wake->stop();
    auto contents = contents_of_download();
    CHECK(contents.has_value());
    if (contents.has_value())
        CHECK_EQ(ByteString { contents->bytes() }, ByteString { expected.bytes() });
}

constexpr auto navigation_extension_id = "imcggofbbffpikbjijfkocdomhplgdmk"sv;

// A page the manifest lists in sandbox.pages runs under the sandbox CSP, with an opaque origin and no extension API.
void sandboxed_pages_have_an_opaque_origin(WebView::HeadlessWebView& view, WebView::ExtensionRegistry& registry)
{
    MUST(registry.load_unpacked(fixture("navigation"sv)));
    auto report = report_of(view, url("chrome-extension://imcggofbbffpikbjijfkocdomhplgdmk/sandboxed/page.html"sv));
    CHECK_EQ(string_in(report, "origin"sv), "null"sv);
    CHECK_EQ(string_in(report, "eval"sv), "ran"sv);
    CHECK_EQ(string_in(report, "storage"sv), "undefined"sv);
    CHECK(!active_document_is_the_extensions(view, navigation_extension_id));
}

// An MV2 manifest's CSP is a string, which the extension's pages run under.
void manifest_v2_policies_apply(WebView::HeadlessWebView& view)
{
    // The extension sandboxed_pages_have_an_opaque_origin() loaded.
    auto report = report_of(view, url("chrome-extension://imcggofbbffpikbjijfkocdomhplgdmk/page.html"sv));
    CHECK_EQ(string_in(report, "origin"sv), "chrome-extension://imcggofbbffpikbjijfkocdomhplgdmk"sv);
    CHECK_EQ(string_in(report, "page_fetch"sv), "blocked"sv);
    CHECK_EQ(string_in(report, "own_fetch"sv), "loaded"sv);
}

// Back and forward come to an extension's page again.
void extension_pages_come_back_through_history(WebView::HeadlessWebView& view)
{
    auto first = report_of(view, url("chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj/page.html"sv));
    CHECK_EQ(string_in(first, "origin"sv), "chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj"sv);
    (void)report_of(view, url("data:text/html,<script>document.title = JSON.stringify({ plain: true });</script>"sv));

    auto report = next_report(view, [&] { view.traverse_the_history_by_delta(-1); }, AK::Duration::from_seconds(10));
    CHECK(report.has_value());
    if (report.has_value())
        CHECK_EQ(string_in(*report, "origin"sv), "chrome-extension://jdpcojkcbchoigdclemcpcifojljdlhj"sv);
    CHECK(active_document_is_the_extensions(view, basic_id));
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
        content_scripts_stay_out_of_svg_images(harness.view, harness.registry, harness.directory);
        content_scripts_stay_out_of_local_files_unless_allowed(harness.view, harness.registry, harness.directory);
        worlds_dont_reach_each_other_through_the_dom(harness.view, harness.registry, harness.directory);
        worlds_dont_reach_each_other_through_what_the_engine_makes(harness.view, harness.registry, harness.directory);
        extension_pages_have_their_content_security_policy(harness.view, harness.registry);
        extension_pages_have_processes_of_their_own(harness.view, harness.directory);
        renderers_cannot_make_their_documents_extension_pages(harness.view, harness.directory);
        renderers_populating_extension_navigations_get_only_what_their_initiator_may(harness.view, harness.directory);
        renderers_cannot_claim_another_requester(harness.view, harness.registry, harness.directory);
        web_pages_embed_only_web_accessible_pages(harness.view, harness.directory);
        extension_pages_come_back_through_history(harness.view);
        renderers_cannot_make_their_documents_any_origin_through_about_blank(harness.view, harness.directory);
        renderers_cannot_click_through_to_extension_pages(harness.view, harness.directory);
        extension_files_download_as_they_are(harness.view, harness.directory);
        sandboxed_pages_have_an_opaque_origin(harness.view, harness.registry);
        manifest_v2_policies_apply(harness.view);
        renderers_cannot_rewrite_their_entry_to_an_extension_url();
    });
}
