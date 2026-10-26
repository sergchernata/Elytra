/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// chrome.webRequest (events, and onAuthRequired's answers) and chrome.declarativeNetRequest (static, dynamic and
// session rules), against an HTTP server of the test's own on the loopback interface.

#include "ExtensionTestHarness.h"
#include <AK/Base64.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibWebView/ExtensionAPI/TabsDelegate.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/WebContentPage.h>

namespace ExtensionTests {

namespace {

constexpr auto extension_id = "bkchhgjjoedlfobfgddicpbjfeaipaod"sv;
constexpr auto other_id = "ceabkfomoeahlaocmilomclcmnplaecf"sv;

// A server in this process, so it goes away with the test. Each request gets one response, and the connection closes.
class TestServer {
public:
    struct Request {
        ByteString method;
        ByteString path;
        HashMap<ByteString, ByteString> headers;
    };

    TestServer()
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto socket = m_server->accept();
            if (socket.is_error())
                return;
            MUST(socket.value()->set_blocking(false));
            m_connections.append(make<Connection>(socket.release_value()));
            auto& connection = *m_connections.last();
            connection.socket->on_ready_to_read = [this, &connection] { read_from(connection); };
        };
    }

    u16 port() const { return *m_server->local_port(); }
    ByteString origin() const { return ByteString::formatted("http://127.0.0.1:{}", port()); }
    ByteString alt_origin() const { return ByteString::formatted("http://localhost:{}", port()); }

    Vector<Request> requests;

    size_t count(StringView path_start) const
    {
        size_t count = 0;
        for (auto const& request : requests) {
            if (request.path.starts_with(path_start))
                ++count;
        }
        return count;
    }

private:
    struct Connection {
        AK_ALLOC_WITH_KMALLOC;

        NonnullOwnPtr<Core::TCPSocket> socket;
        ByteBuffer data {};
    };

    void read_from(Connection& connection)
    {
        auto buffer = MUST(ByteBuffer::create_uninitialized(64 * KiB));
        auto received = connection.socket->read_some(buffer);
        if (received.is_error() || received.value().is_empty()) {
            connection.socket->on_ready_to_read = nullptr;
            return;
        }
        connection.data.append(received.value());
        auto data = StringView { connection.data.bytes() };
        auto header_end = data.find("\r\n\r\n"sv);
        if (!header_end.has_value())
            return;

        Request request;
        auto lines = data.substring_view(0, *header_end).split_view("\r\n"sv);
        auto request_line = lines[0].split_view(' ');
        request.method = request_line[0];
        request.path = request_line.size() > 1 ? request_line[1] : "/"sv;
        for (size_t i = 1; i < lines.size(); ++i) {
            auto separator = lines[i].find(':');
            if (!separator.has_value())
                continue;
            request.headers.set(ByteString { lines[i].substring_view(0, *separator) }.to_lowercase(), lines[i].substring_view(*separator + 1).trim_whitespace());
        }
        auto content_length = request.headers.get("content-length"sv).value_or("0"sv).to_number<size_t>().value_or(0);
        if (data.length() - *header_end - 4 < content_length)
            return;
        connection.socket->on_ready_to_read = nullptr;

        auto response = respond_to(request);
        requests.append(move(request));
        MUST(connection.socket->set_blocking(true));
        (void)connection.socket->write_until_depleted(response.bytes());
        connection.socket->close();
    }

    ByteString respond_to(Request const& request)
    {
        auto path = request.path.view();
        auto query_start = path.find('?');
        auto route = query_start.has_value() ? path.substring_view(0, *query_start) : path;
        auto origin = request.headers.get("origin"sv).value_or("*"sv);

        auto response = [](u32 status, StringView content_type, StringView body, StringView extra_headers = {}) {
            return ByteString::formatted("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n{}\r\n{}",
                status, status == 200 ? "OK"sv : status == 302 ? "Found"sv : status == 401 ? "Unauthorized"sv : "Not Found"sv, content_type, body.length(), extra_headers, body);
        };
        auto cors = ByteString::formatted("Access-Control-Allow-Origin: {}\r\nAccess-Control-Allow-Credentials: true\r\nAccess-Control-Expose-Headers: *\r\n", origin);

        if (route == "/echo"sv) {
            JsonObject headers;
            for (auto const& [name, value] : request.headers)
                headers.set(MUST(String::from_byte_string(name)), MUST(String::from_byte_string(value)));
            return response(200, "application/json"sv, headers.serialized(), ByteString::formatted("{}X-Test: yes\r\n", cors));
        }
        if (route == "/redirect"sv)
            return response(302, "text/plain"sv, ""sv, ByteString::formatted("{}Location: /echo?redirected\r\n", cors));
        if (route == "/redirect-page"sv)
            return response(302, "text/plain"sv, ""sv, "Location: /page\r\n"sv);
        if (route == "/page"sv)
            return response(200, "text/html"sv, "<!doctype html><title>page</title>"sv);
        if (route == "/auth"sv) {
            auto expected = ByteString::formatted("Basic {}", MUST(encode_base64("user:pass"sv.bytes())));
            if (request.headers.get("authorization"sv) == expected.view())
                return response(200, "text/html"sv, "<!doctype html><title>authed</title>"sv, cors);
            return response(401, "text/html"sv, "<!doctype html><title>denied</title>"sv, ByteString::formatted("{}WWW-Authenticate: Basic realm=\"Test Realm\"\r\n", cors));
        }
        // A page that fetches /nocors with its query, and puts the status in its title.
        if (route == "/fetch-page"sv) {
            auto page = ByteString::formatted(R"(<!doctype html><title>loading</title><script>
fetch("/nocors" + location.search).then(r => document.title = "fetched:" + r.status, () => document.title = "fetched:error");
</script>)");
            return response(200, "text/html"sv, page);
        }
        if (route == "/nocors"sv)
            return response(200, "application/json"sv, "{\"ok\":true}"sv);
        if (route == "/cors-page"sv) {
            auto page = ByteString::formatted(R"(<!doctype html><title>loading</title><script>
fetch("http://localhost:{}/nocors").then(r => r.json()).then(j => document.title = "cors:" + (j.ok ? "ok" : "bad") + location.search, () => document.title = "cors:error" + location.search);
</script>)",
                port());
            return response(200, "text/html"sv, page);
        }
        return response(404, "text/plain"sv, "not found"sv);
    }

    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Connection>> m_connections;
};

// One window of tabs, each a headless view.
class TestBrowser final : public WebView::ExtensionTabsDelegate {
public:
    explicit TestBrowser(Core::AnonymousBuffer theme)
        : m_theme(move(theme))
    {
    }
    ~TestBrowser() { runtime().set_tabs_delegate(nullptr); }

    static WebView::ExtensionRuntime& runtime() { return WebView::Application::extension_runtime(); }

    void add_tab(WebView::HeadlessWebView& view)
    {
        m_tabs.append({ m_next_id++, &view, nullptr });
    }

    virtual Vector<Window> windows() const override
    {
        return { Window { .id = 1, .focused = true, .incognito = false, .always_on_top = false, .type = "normal"sv, .state = "normal"sv, .bounds = {} } };
    }

    virtual Vector<Tab> tabs() const override
    {
        Vector<Tab> tabs;
        for (size_t i = 0; i < m_tabs.size(); ++i) {
            tabs.append({ .id = m_tabs[i].id, .window_id = 1, .index = static_cast<i64>(i), .view = m_tabs[i].view, .active = i == 0, .highlighted = i == 0,
                .pinned = false, .audible = false, .muted = false, .discarded = false, .incognito = false, .opener_tab_id = {}, .title = {}, .fav_icon_url = {} });
        }
        return tabs;
    }

    virtual Optional<i64> last_focused_window_id() const override { return 1; }

    virtual ErrorOr<i64, String> create_tab(CreateTab) override
    {
        auto view = WebView::HeadlessWebView::create(m_theme, { 800, 600 });
        view->on_title_change = [](auto const&) { runtime().tabs_changed(); };
        auto id = m_next_id++;
        auto* pointer = view.ptr();
        m_tabs.append({ id, pointer, move(view) });
        return id;
    }
    virtual ErrorOr<void, String> update_tab(i64, UpdateTab) override { return {}; }
    virtual ErrorOr<void, String> move_tab(i64, i64, i64) override { return "Can't move tabs"_string; }
    virtual void close_tab(i64 tab_id) override
    {
        for (size_t i = 0; i < m_tabs.size(); ++i) {
            if (m_tabs[i].id == tab_id) {
                m_closed.append(move(m_tabs[i].owned));
                m_tabs.remove(i);
                break;
            }
        }
        runtime().tabs_changed();
    }
    virtual ErrorOr<i64, String> create_window(CreateWindow) override { return "Can't open windows"_string; }
    virtual ErrorOr<void, String> update_window(i64, UpdateWindow) override { return {}; }
    virtual void close_window(i64) override { }

private:
    struct TestTab {
        i64 id { 0 };
        WebView::HeadlessWebView* view { nullptr };
        OwnPtr<WebView::HeadlessWebView> owned;
    };
    Core::AnonymousBuffer m_theme;
    Vector<TestTab> m_tabs;
    // Closing tabs' views live on, as the call that closed them may have come from them.
    Vector<OwnPtr<WebView::HeadlessWebView>> m_closed;
    i64 m_next_id { 1 };
};

JsonObject run(WebView::HeadlessWebView& view, TestServer const& server, StringView test)
{
    auto report = report_of(view, url(ByteString::formatted("chrome-extension://{}/page.html?test={}&server={}&alt={}", extension_id, test, server.origin(), server.alt_origin())));
    CHECK_EQ(string_in(report, "_"sv), "done"sv);
    return report;
}

i64 integer_in(JsonObject const& report, StringView key)
{
    return report.get_integer<i64>(key).value_or(-999);
}

bool bool_in(JsonObject const& report, StringView key)
{
    return report.get_bool(key).value_or(false);
}

void events_of_a_fetch(WebView::HeadlessWebView& view, TestServer const& server)
{
    auto report = run(view, server, "events"sv);
    CHECK_EQ(string_in(report, "order"sv), "onBeforeRequest,onBeforeSendHeaders,onSendHeaders,onHeadersReceived,onResponseStarted,onCompleted"sv);
    CHECK(bool_in(report, "one_request_id"sv));
    CHECK_EQ(string_in(report, "url"sv), MUST(String::formatted("{}/echo?events", server.origin())));
    CHECK_EQ(string_in(report, "method"sv), "GET"sv);
    CHECK_EQ(string_in(report, "type"sv), "xmlhttprequest"sv);
    // The test's page is in the first tab.
    CHECK_EQ(integer_in(report, "tab"sv), 1);
    CHECK_EQ(integer_in(report, "frame"sv), 0);
    CHECK_EQ(integer_in(report, "parent_frame"sv), -1);
    CHECK_EQ(string_in(report, "initiator"sv), MUST(String::formatted("chrome-extension://{}", extension_id)));
    CHECK(bool_in(report, "time"sv));
    CHECK_EQ(integer_in(report, "status"sv), 200);
    CHECK_EQ(string_in(report, "status_line"sv), "HTTP/1.1 200 OK"sv);
    CHECK_EQ(string_in(report, "x_test"sv), "yes"sv);
    CHECK_EQ(report.get_bool("from_cache"sv), false);
    CHECK_EQ(integer_in(report, "completed_status"sv), 200);
    CHECK(bool_in(report, "sent_has_headers"sv));
    CHECK(bool_in(report, "before_has_no_headers"sv));
    CHECK(bool_in(report, "server_saw"sv));
}

void redirects_are_followed_with_one_request_id(WebView::HeadlessWebView& view, TestServer const& server)
{
    auto report = run(view, server, "redirect"sv);
    CHECK_EQ(string_in(report, "final"sv), MUST(String::formatted("{}/echo?redirected", server.origin())));
    CHECK_EQ(integer_in(report, "redirect_status"sv), 302);
    CHECK_EQ(string_in(report, "redirect_url"sv), MUST(String::formatted("{}/echo?redirected", server.origin())));
    CHECK_EQ(string_in(report, "order"sv), "onBeforeRequest@/redirect,onBeforeSendHeaders@/redirect,onSendHeaders@/redirect,onHeadersReceived@/redirect,onBeforeRedirect@/redirect,"
                                           "onBeforeRequest@/echo,onBeforeSendHeaders@/echo,onSendHeaders@/echo,onHeadersReceived@/echo,onResponseStarted@/echo,onCompleted@/echo"sv);
    CHECK(bool_in(report, "one_request_id"sv));
}

void a_tabs_main_frame_is_reported_by_tab(WebView::HeadlessWebView& view, TestServer const& server)
{
    auto report = run(view, server, "tab"sv);
    CHECK_EQ(integer_in(report, "status"sv), 200);
    CHECK_EQ(string_in(report, "type"sv), "main_frame"sv);
    CHECK_EQ(integer_in(report, "frame"sv), 0);
    CHECK_EQ(integer_in(report, "parent_frame"sv), -1);
    CHECK(bool_in(report, "right_tab"sv));
    CHECK_EQ(string_in(report, "url"sv), MUST(String::formatted("{}/page", server.origin())));
    CHECK_EQ(string_in(report, "redirects"sv), "302>/page:main_frame:true"sv);
}

void listeners_hear_only_what_their_filters_match(WebView::HeadlessWebView& view, TestServer const& server)
{
    auto report = run(view, server, "filters"sv);
    CHECK_EQ(integer_in(report, "all"sv), 1);
    CHECK_EQ(integer_in(report, "image"sv), 0);
    CHECK_EQ(integer_in(report, "other_host"sv), 0);
    CHECK_EQ(integer_in(report, "other_tab"sv), 0);
    CHECK_EQ(integer_in(report, "window"sv), 0);
    CHECK_EQ(integer_in(report, "too_many"sv), 0);
}

void listeners_need_valid_filters(WebView::HeadlessWebView& view, TestServer const& server)
{
    auto report = run(view, server, "misuse"sv);
    for (auto key : { "no_filter"sv, "bad_urls"sv, "bad_extra"sv, "bad_type"sv })
        CHECK(string_in(report, key).starts_with_bytes("error: "sv));
    CHECK(!bool_in(report, "has_after_misuse"sv));
    CHECK_EQ(string_in(report, "handler_behavior_changed"sv), "undefined"sv);
}

void blocked_requests_are_errors(WebView::HeadlessWebView& view, TestServer const& server)
{
    auto report = run(view, server, "errors"sv);
    CHECK(string_in(report, "blocked"sv).starts_with_bytes("error: "sv));
    CHECK_EQ(string_in(report, "error"sv), "net::ERR_BLOCKED_BY_CLIENT"sv);
    CHECK_EQ(string_in(report, "blocked_order"sv), "onBeforeRequest,onErrorOccurred"sv);
    CHECK_EQ(string_in(report, "after_unblock"sv), "200"sv);
    CHECK_EQ(server.count("/echo?blocked"sv), 0u);
}

void auth_listeners_can_cancel(WebView::HeadlessWebView& view, TestServer const& server)
{
    auto report = run(view, server, "auth_cancel"sv);
    CHECK_EQ(string_in(report, "result"sv), "401"sv);
    CHECK_EQ(integer_in(report, "asked"sv), 1);
    CHECK_EQ(string_in(report, "details"sv), "401:basic:Test Realm:false:127.0.0.1:true:GET"sv);
}

void auth_listeners_can_answer_later(WebView::HeadlessWebView& view, TestServer const& server)
{
    auto report = run(view, server, "auth_async"sv);
    CHECK_EQ(string_in(report, "result"sv), "200:<!doctype html><title>authed</title>"sv);
    CHECK_EQ(integer_in(report, "asked"sv), 1);
    CHECK_EQ(string_in(report, "again"sv), "200:<!doctype html><title>authed</title>"sv);
    CHECK_EQ(integer_in(report, "asked_again"sv), 1);
}

void auth_listeners_answer_for_tabs(WebView::HeadlessWebView& view, TestServer const& server)
{
    auto report = run(view, server, "auth_navigation"sv);
    CHECK_EQ(string_in(report, "title"sv), "authed"sv);
    CHECK_EQ(string_in(report, "type"sv), "main_frame"sv);
}

void static_rulesets_apply_and_switch(WebView::HeadlessWebView& view, TestServer const& server)
{
    auto report = run(view, server, "static_rules"sv);
    CHECK_EQ(string_in(report, "user_agent"sv), "removed"sv);
    CHECK_EQ(string_in(report, "x_dnr"sv), "static"sv);
    CHECK_EQ(string_in(report, "enabled"sv), "ruleset_1"sv);
    CHECK_EQ(integer_in(report, "available"sv), 29999);
    CHECK_EQ(string_in(report, "before_enable"sv), "200"sv);
    CHECK(string_in(report, "after_enable"sv).starts_with_bytes("error: "sv));
    CHECK_EQ(string_in(report, "enabled_after"sv), "ruleset_1,ruleset_2"sv);
    CHECK_EQ(integer_in(report, "available_after"sv), 29998);
    for (auto key : { "enable_unknown"sv, "bad_options"sv, "bad_ids"sv })
        CHECK(string_in(report, key).starts_with_bytes("error: "sv));
    CHECK_EQ(string_in(report, "enabled_after_errors"sv), "ruleset_1,ruleset_2"sv);
    CHECK_EQ(string_in(report, "after_disable"sv), "200"sv);
    CHECK_EQ(string_in(report, "enabled_end"sv), "ruleset_1"sv);
}

void dynamic_rules_apply_and_are_kept(WebView::HeadlessWebView& view, TestServer const& server, ByteString const& storage)
{
    auto report = run(view, server, "dynamic_rules"sv);
    CHECK_EQ(string_in(report, "ids"sv), "1,2,3,4"sv);
    CHECK_EQ(string_in(report, "filtered"sv), "2"sv);
    CHECK_EQ(integer_in(report, "priority_default"sv), 1);
    CHECK(string_in(report, "blocked"sv).starts_with_bytes("error: "sv));
    CHECK_EQ(string_in(report, "allowed"sv), "200"sv);
    CHECK_EQ(string_in(report, "redirected"sv), "true:?redirected-by-rule"sv);
    CHECK_EQ(string_in(report, "response_header"sv), "dynamic"sv);
    CHECK_EQ(string_in(report, "request_header"sv), "dynamic"sv);
    for (auto key : { "duplicate"sv, "invalid"sv, "tab_ids"sv, "no_options"sv, "bad_options"sv, "regex_missing"sv })
        CHECK(string_in(report, key).starts_with_bytes("error: "sv));
    CHECK_EQ(string_in(report, "ids_after_errors"sv), "1,2,3,4"sv);
    CHECK_EQ(string_in(report, "ids_after_remove"sv), "2"sv);
    CHECK_EQ(string_in(report, "unblocked"sv), "200"sv);
    // The other extension blocks "other-blocked", but not for this one's requests.
    CHECK_EQ(string_in(report, "other_blocked"sv), "200"sv);
    CHECK_EQ(string_in(report, "regex_ok"sv), R"({"isSupported":true})"sv);
    CHECK_EQ(string_in(report, "regex_bad"sv), R"({"isSupported":false,"reason":"syntaxError"})"sv);
    CHECK_EQ(string_in(report, "constants"sv), "30000:_dynamic"sv);
    CHECK_EQ(server.count("/echo?dyn-block"sv), 2u); // The allowed one and the one after the rule went.

    auto saved = Core::File::open(ByteString::formatted("{}/{}/declarative_net_request.json", storage, extension_id), Core::File::OpenMode::Read);
    CHECK(!saved.is_error());
    if (!saved.is_error()) {
        auto contents = MUST(saved.value()->read_until_eof());
        CHECK(StringView { contents }.contains("redirected-by-rule"sv));
        CHECK(!StringView { contents }.contains("dyn-block"sv));
    }
}

void session_rules_change_responses(WebView::HeadlessWebView& view, TestServer const& server)
{
    auto report = run(view, server, "session_rules"sv);
    CHECK_EQ(string_in(report, "before"sv), "cors:error?before"sv);
    CHECK_EQ(string_in(report, "rules"sv), "1000000"sv);
    CHECK_EQ(string_in(report, "with_rule"sv), "cors:ok?with-rule"sv);
    CHECK_EQ(string_in(report, "tab_rule"sv), "cors:error?blocked-in-tab"sv);
    CHECK_EQ(integer_in(report, "rules_end"sv), 0);
}

// Loads the URL and waits for a title with the prefix (none after the timeout).
Optional<String> title_of(WebView::HeadlessWebView& view, URL::URL const& url, StringView prefix, AK::Duration timeout = AK::Duration::from_seconds(10))
{
    Optional<String> title;
    view.on_title_change = [&](Utf16String const& changed) {
        auto text = changed.to_utf8();
        if (text.starts_with_bytes(prefix))
            title = text;
    };
    view.load(url);
    auto deadline = MonotonicTime::now() + timeout;
    auto wake = Core::Timer::create_repeating(AK::Duration::from_milliseconds(50), [] { });
    wake->start();
    Core::EventLoop::current().spin_until([&] { return title.has_value() || MonotonicTime::now() > deadline; });
    wake->stop();
    view.on_title_change = nullptr;
    return title;
}

void spin_for(AK::Duration duration)
{
    auto deadline = MonotonicTime::now() + duration;
    auto wake = Core::Timer::create_repeating(AK::Duration::from_milliseconds(20), [] { });
    wake->start();
    Core::EventLoop::current().spin_until([&] { return MonotonicTime::now() > deadline; });
    wake->stop();
}

// Chrome keeps extensions out of incognito unless the user lets each in, which Elytra has no setting for: private
// windows get no webRequest events, no declarativeNetRequest rules and no extension pages.
void private_windows_have_no_extensions(Core::AnonymousBuffer const& theme, WebView::HeadlessWebView& view, TestServer const& server)
{
    // The other extension blocks "other-blocked" for every page but its own.
    CHECK_EQ(title_of(view, url(ByteString::formatted("{}/fetch-page?other-blocked-normal", server.origin())), "fetched:"sv).value_or("(none)"_string), "fetched:error"sv);

    auto private_view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::Yes);
    CHECK_EQ(title_of(*private_view, url(ByteString::formatted("{}/fetch-page?other-blocked-private", server.origin())), "fetched:"sv).value_or("(none)"_string), "fetched:200"sv);
    CHECK_EQ(server.count("/nocors?other-blocked-private"sv), 1u);
    CHECK_EQ(server.count("/nocors?other-blocked-normal"sv), 0u);

    // An extension's page doesn't open there.
    auto report = title_of(*private_view, url(ByteString::formatted("chrome-extension://{}/page.html", other_id)), "{"sv, AK::Duration::from_seconds(3));
    CHECK(!report.has_value());
    auto const& origin = private_view->page().traversable().active_document().origin();
    CHECK(origin.is_opaque() || origin.scheme() != "chrome-extension"sv);
    CHECK(!WebView::web_request_config_for_testing(private_view->page()).contains("\"rules\""sv));
}

String forged_details(StringView request_url, StringView request_id, StringView type, Optional<StringView> initiator)
{
    JsonObject details;
    details.set("url"sv, request_url);
    details.set("requestId"sv, request_id);
    details.set("method"sv, "GET"sv);
    details.set("type"sv, type);
    if (initiator.has_value())
        details.set("initiator"sv, *initiator);
    return details.serialized();
}

void forge_event(WebView::WebContentPage& page, StringView event, String const& details)
{
    auto id = page.traversable().id();
    TestBrowser::runtime().message_received(page, 0, MUST(String::formatted(R"({{"type":"web_request_event","event":"webRequest.{}","details":{},"navigable":{{"ns":{},"id":{}}}}})", event, details, id.namespace_id, id.local_id)));
}

void forge_challenge(WebView::WebContentPage& page, u64 request, String const& details)
{
    auto id = page.traversable().id();
    TestBrowser::runtime().message_received(page, 0, MUST(String::formatted(R"({{"type":"web_request_auth","request":{},"details":{},"navigable":{{"ns":{},"id":{}}}}})", request, details, id.namespace_id, id.local_id)));
}

// A page process reports requests: the browser hears only of those its documents could make, and never asks extensions
// for credentials to another site than its documents'.
void forged_reports_are_refused(Core::AnonymousBuffer const& theme, WebView::HeadlessWebView& view, TestServer const& server)
{
    auto origin = MUST(String::from_byte_string(server.origin()));
    auto alt = MUST(String::from_byte_string(server.alt_origin()));
    CHECK_EQ(title_of(view, url(ByteString::formatted("{}/page", alt)), "page"sv).value_or("(none)"_string), "page"sv);
    auto& page = view.page();
    auto secret = MUST(String::formatted("{}/forged-secret", origin));

    // A challenge from a site the page has no document of, with or without an initiator.
    forge_challenge(page, 9001, forged_details(secret, "1"sv, "xmlhttprequest"sv, {}));
    forge_challenge(page, 9002, forged_details(secret, "2"sv, "xmlhttprequest"sv, origin));
    forge_challenge(page, 9003, forged_details(secret, "3"sv, "main_frame"sv, {}));
    // Events for an initiator the page doesn't host, without one for a subresource, and for a navigation it isn't making.
    forge_event(page, "onBeforeRequest"sv, forged_details(MUST(String::formatted("{}/forged-unhosted-initiator", origin)), "4"sv, "xmlhttprequest"sv, origin));
    forge_event(page, "onBeforeRequest"sv, forged_details(MUST(String::formatted("{}/forged-no-initiator", origin)), "5"sv, "xmlhttprequest"sv, {}));
    forge_event(page, "onBeforeRequest"sv, forged_details(MUST(String::formatted("{}/forged-main-frame", origin)), "6"sv, "main_frame"sv, {}));

    // Request IDs are the browser's: two pages' "77" are two requests.
    CHECK_EQ(title_of(view, url(ByteString::formatted("{}/page", origin)), "page"sv).value_or("(none)"_string), "page"sv);
    auto second = WebView::HeadlessWebView::create(theme, { 800, 600 });
    CHECK_EQ(title_of(*second, url(ByteString::formatted("{}/page", origin)), "page"sv).value_or("(none)"_string), "page"sv);
    forge_event(view.page(), "onBeforeRequest"sv, forged_details(MUST(String::formatted("{}/rid-a", origin)), "77"sv, "xmlhttprequest"sv, origin));
    forge_event(second->page(), "onBeforeRequest"sv, forged_details(MUST(String::formatted("{}/rid-b", origin)), "77"sv, "xmlhttprequest"sv, origin));
    spin_for(AK::Duration::from_milliseconds(300));

    // What a web page is told of the rules names no extension, and no host permissions an extension's rules don't need.
    auto config = WebView::web_request_config_for_testing(view.page());
    CHECK(config.contains("\"rules\""sv));
    CHECK(!config.contains(extension_id));
    CHECK(!config.contains(other_id));
    CHECK_EQ(config.count("hostPermissions"sv), 1u);
}

// The other extension has webRequest without webRequestAuthProvider, and host access to 127.0.0.1 only.
// As in Chrome, an extension's own pages read what its host permissions cover without CORS (1Password's servers send
// no CORS headers); anywhere else CORS still holds.
void extension_pages_read_their_hosts_without_cors(WebView::HeadlessWebView& view, TestServer& server)
{
    auto report = report_of(view, url(ByteString::formatted("chrome-extension://{}/page.html?server={}&alt={}", other_id, server.origin(), server.alt_origin())));
    CHECK_EQ(string_in(report, "permitted"sv), "read"sv);
    CHECK_EQ(string_in(report, "not_permitted"sv), "blocked"sv);
}

void other_extensions_see_only_their_share(WebView::HeadlessWebView& view, TestServer const& server)
{
    auto report = report_of(view, url(ByteString::formatted("chrome-extension://{}/page.html", other_id)));
    CHECK_EQ(string_in(report, "_"sv), "done"sv);
    auto urls = string_in(report, "urls"sv);
    // Tabs' pages on 127.0.0.1, but nothing the first extension fetched itself, nor anything on localhost.
    CHECK(urls.contains(MUST(String::formatted("{}/redirect-page", server.origin()))));
    CHECK(!urls.contains("/echo"sv));
    CHECK(!urls.contains("localhost"sv));
    CHECK(!urls.contains("private"sv));
    CHECK(!urls.contains("forged"sv));
    CHECK(urls.contains("other-blocked-normal"sv));
    auto ids_string = string_in(report, "ids"sv);
    auto ids = ids_string.bytes_as_string_view().split_view(',');
    CHECK_EQ(ids.size(), 2u);
    if (ids.size() == 2) {
        CHECK(ids[0] != ids[1]);
        CHECK(ids[0] != "77"sv && ids[1] != "77"sv);
    }
    CHECK_EQ(integer_in(report, "auth"sv), 0);
    CHECK_EQ(string_in(report, "dynamic"sv), "1"sv);
    CHECK_EQ(integer_in(report, "session"sv), 0);
}

}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return ExtensionTests::run_extension_tests(arguments, [](ExtensionTests::Harness& harness) {
        using namespace ExtensionTests;
        auto storage = ByteString::formatted("{}/storage", harness.directory);
        WebView::Application::extension_runtime().set_storage_directory(storage);

        TestServer server;
        auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
        auto theme = MUST(Gfx::load_system_theme(theme_path.string()));
        TestBrowser browser(theme);
        browser.add_tab(harness.view);
        TestBrowser::runtime().set_tabs_delegate(&browser);

        MUST(harness.registry.load_unpacked(fixture("webrequest_other"sv)));
        MUST(harness.registry.load_unpacked(fixture("webrequest"sv)));
        // The other extension's background listens from the start.
        auto other_started = report_of(harness.view, url(ByteString::formatted("chrome-extension://{}/page.html", other_id)));
        CHECK_EQ(string_in(other_started, "dynamic"sv), "1"sv);

        events_of_a_fetch(harness.view, server);
        redirects_are_followed_with_one_request_id(harness.view, server);
        a_tabs_main_frame_is_reported_by_tab(harness.view, server);
        listeners_hear_only_what_their_filters_match(harness.view, server);
        listeners_need_valid_filters(harness.view, server);
        blocked_requests_are_errors(harness.view, server);
        auth_listeners_can_cancel(harness.view, server);
        auth_listeners_can_answer_later(harness.view, server);
        auth_listeners_answer_for_tabs(harness.view, server);
        static_rulesets_apply_and_switch(harness.view, server);
        dynamic_rules_apply_and_are_kept(harness.view, server, storage);
        session_rules_change_responses(harness.view, server);
        private_windows_have_no_extensions(theme, harness.view, server);
        forged_reports_are_refused(theme, harness.view, server);
        other_extensions_see_only_their_share(harness.view, server);
        extension_pages_read_their_hosts_without_cors(harness.view, server);
    });
}
