/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// chrome.tabs, chrome.windows, chrome.scripting and chrome.webNavigation, in a browser of headless views.

#include "ExtensionTestHarness.h"
#include <LibWebView/ExtensionAPI/TabsDelegate.h>

namespace ExtensionTests {

namespace {

constexpr auto tabs_extension_id = "dkcmhdciailofeligafglofekchdlkge"sv;
constexpr auto limited_extension_id = "oimncacppbknbejommchgipbbnaipekh"sv;

// The UI the runtime would have: windows of tabs, each a headless view.
class FakeBrowser final : public WebView::ExtensionTabsDelegate {
public:
    struct FakeTab {
        i64 id { 0 };
        WebView::HeadlessWebView* view { nullptr };
        OwnPtr<WebView::HeadlessWebView> owned_view;
        bool pinned { false };
        bool muted { false };
        Optional<i64> opener;
    };
    struct FakeWindow {
        i64 id { 0 };
        StringView type { "normal"sv };
        Gfx::IntRect bounds { 0, 0, 800, 600 };
        Vector<FakeTab> tabs;
        size_t active { 0 };
    };

    explicit FakeBrowser(Core::AnonymousBuffer theme)
        : m_theme(move(theme))
    {
    }

    ~FakeBrowser()
    {
        runtime().set_tabs_delegate(nullptr);
    }

    static WebView::ExtensionRuntime& runtime() { return WebView::Application::extension_runtime(); }

    i64 add_window(StringView type = "normal"sv)
    {
        auto id = m_next_id++;
        m_windows.append({ .id = id, .type = type, .bounds = { 0, 0, 800, 600 }, .tabs = {}, .active = 0 });
        if (!m_focused.has_value())
            m_focused = id;
        return id;
    }

    // A tab of the harness's own view.
    i64 add_tab(i64 window_id, WebView::HeadlessWebView& view)
    {
        auto id = m_next_id++;
        auto& window = *find_window(window_id);
        window.tabs.append({ .id = id, .view = &view, .owned_view = nullptr, .pinned = false, .muted = false, .opener = {} });
        watch(view);
        return id;
    }

    // A tab with a view of its own, showing the URL once it's loaded.
    i64 add_loaded_tab(i64 window_id, URL::URL const& url)
    {
        auto view = new_view();
        auto& view_reference = *view;
        auto id = m_next_id++;
        find_window(window_id)->tabs.append({ .id = id, .view = view.ptr(), .owned_view = move(view), .pinned = false, .muted = false, .opener = {} });
        bool loaded = false;
        view_reference.on_load_finish = [&](auto const&) { loaded = true; };
        view_reference.load(url);
        Core::EventLoop::current().spin_until([&] { return loaded; });
        view_reference.on_load_finish = nullptr;
        // Wait for the title too.
        Core::EventLoop::current().spin_until([&] { return !view_reference.title().is_empty(); });
        return id;
    }

    FakeTab* find_tab(i64 id, FakeWindow** window_of_tab = nullptr)
    {
        for (auto& window : m_windows) {
            for (auto& tab : window.tabs) {
                if (tab.id == id) {
                    if (window_of_tab)
                        *window_of_tab = &window;
                    return &tab;
                }
            }
        }
        return nullptr;
    }
    FakeWindow* find_window(i64 id)
    {
        for (auto& window : m_windows) {
            if (window.id == id)
                return &window;
        }
        return nullptr;
    }

    void focus(i64 window_id)
    {
        m_focused = window_id;
        m_last_focused = window_id;
    }
    void set_pinned(i64 tab_id, bool pinned) { find_tab(tab_id)->pinned = pinned; }

    virtual Vector<Window> windows() const override
    {
        Vector<Window> windows;
        for (auto const& window : m_windows)
            windows.append({ .id = window.id, .focused = m_focused == window.id, .incognito = false, .always_on_top = false, .type = window.type, .state = "normal"sv, .bounds = window.bounds });
        return windows;
    }

    virtual Vector<Tab> tabs() const override
    {
        Vector<Tab> tabs;
        for (auto const& window : m_windows) {
            for (size_t index = 0; index < window.tabs.size(); ++index) {
                auto const& tab = window.tabs[index];
                tabs.append({
                    .id = tab.id,
                    .window_id = window.id,
                    .index = static_cast<i64>(index),
                    .view = tab.view,
                    .active = index == window.active,
                    .highlighted = index == window.active,
                    .pinned = tab.pinned,
                    .audible = false,
                    .muted = tab.muted,
                    .discarded = false,
                    .incognito = false,
                    .opener_tab_id = tab.opener,
                    .title = {},
                    .fav_icon_url = {},
                });
            }
        }
        return tabs;
    }

    virtual Optional<i64> last_focused_window_id() const override { return m_last_focused.has_value() ? m_last_focused : m_focused; }

    virtual ErrorOr<i64, String> create_tab(CreateTab options) override
    {
        auto window_id = options.window_id.value_or(m_focused.value_or(m_windows.first().id));
        auto* window = find_window(window_id);
        if (!window)
            return "No such window"_string;
        auto id = m_next_id++;
        auto view = new_view();
        auto index = min(static_cast<size_t>(options.index.value_or(window->tabs.size())), window->tabs.size());
        window->tabs.insert(index, { .id = id, .view = view.ptr(), .owned_view = move(view), .pinned = options.pinned, .muted = false, .opener = options.opener_tab_id });
        if (index <= window->active && window->tabs.size() > 1)
            ++window->active;
        if (options.active)
            window->active = index;
        return id;
    }

    virtual ErrorOr<void, String> update_tab(i64 tab_id, UpdateTab update) override
    {
        FakeWindow* window = nullptr;
        auto* tab = find_tab(tab_id, &window);
        if (!tab)
            return "No such tab"_string;
        if (update.pinned.has_value())
            tab->pinned = *update.pinned;
        if (update.muted.has_value())
            tab->muted = *update.muted;
        if (update.active == true) {
            for (size_t index = 0; index < window->tabs.size(); ++index) {
                if (window->tabs[index].id == tab_id)
                    window->active = index;
            }
        }
        return {};
    }

    virtual ErrorOr<void, String> move_tab(i64 tab_id, i64 window_id, i64 index) override
    {
        FakeWindow* from = nullptr;
        if (!find_tab(tab_id, &from))
            return "No such tab"_string;
        auto* to = find_window(window_id);
        if (!to)
            return "No such window"_string;
        auto active_id = from->tabs[from->active].id;
        Optional<FakeTab> moving;
        for (size_t i = 0; i < from->tabs.size(); ++i) {
            if (from->tabs[i].id == tab_id) {
                moving = from->tabs.take(i);
                break;
            }
        }
        auto position = index < 0 ? to->tabs.size() : min(static_cast<size_t>(index), to->tabs.size());
        to->tabs.insert(position, moving.release_value());
        for (auto* window : { from, to }) {
            for (size_t i = 0; i < window->tabs.size(); ++i) {
                if (window->tabs[i].id == active_id)
                    window->active = i;
            }
            window->active = min(window->active, window->tabs.is_empty() ? 0 : window->tabs.size() - 1);
        }
        return {};
    }

    virtual void close_tab(i64 tab_id) override
    {
        FakeWindow* window = nullptr;
        if (!find_tab(tab_id, &window))
            return;
        for (size_t i = 0; i < window->tabs.size(); ++i) {
            if (window->tabs[i].id == tab_id) {
                auto tab = window->tabs.take(i);
                if (window->active > i || window->active >= window->tabs.size())
                    window->active = window->active == 0 ? 0 : window->active - 1;
                m_closed.append(move(tab.owned_view));
                break;
            }
        }
        if (window->tabs.is_empty())
            close_window(window->id);
        runtime().tabs_changed();
    }

    virtual ErrorOr<i64, String> create_window(CreateWindow options) override
    {
        auto id = add_window(options.type);
        if (options.bounds.has_value())
            find_window(id)->bounds = *options.bounds;
        auto count = max<size_t>(1, options.urls.size());
        for (size_t i = 0; i < count; ++i)
            MUST(create_tab({ .window_id = id, .index = {}, .url = {}, .active = i == 0, .pinned = false, .opener_tab_id = {} }));
        if (options.focused)
            focus(id);
        return id;
    }

    virtual ErrorOr<void, String> update_window(i64 window_id, UpdateWindow update) override
    {
        auto* window = find_window(window_id);
        if (!window)
            return "No such window"_string;
        if (update.focused == true)
            focus(window_id);
        if (update.left.has_value())
            window->bounds.set_x(*update.left);
        if (update.width.has_value())
            window->bounds.set_width(*update.width);
        return {};
    }

    virtual void close_window(i64 window_id) override
    {
        for (size_t i = 0; i < m_windows.size(); ++i) {
            if (m_windows[i].id != window_id)
                continue;
            auto window = m_windows.take(i);
            for (auto& tab : window.tabs)
                m_closed.append(move(tab.owned_view));
            if (m_focused == window_id)
                m_focused = m_windows.is_empty() ? Optional<i64> {} : m_windows.first().id;
            break;
        }
        runtime().tabs_changed();
    }

private:
    NonnullOwnPtr<WebView::HeadlessWebView> new_view()
    {
        auto view = WebView::HeadlessWebView::create(m_theme, { 800, 600 });
        watch(*view);
        return view;
    }

    // What a UI does: it tells the runtime of title changes.
    static void watch(WebView::HeadlessWebView& view)
    {
        view.on_title_change = [](auto const&) { runtime().tabs_changed(); };
    }

    Core::AnonymousBuffer m_theme;
    Vector<FakeWindow> m_windows;
    Optional<i64> m_focused;
    Optional<i64> m_last_focused;
    i64 m_next_id { 1 };
    // Views of closed tabs: a UI drops them later, as a closing tab's view may be what asked for it.
    Vector<OwnPtr<WebView::HeadlessWebView>> m_closed;
};

// Loads a case of the extension's test page in the view (a tab) and waits for its report, telling the runtime of the
// title changes on the way, as the UI would.
JsonObject run_case(WebView::HeadlessWebView& view, StringView extension_id, StringView query)
{
    Optional<String> report;
    view.on_title_change = [&](Utf16String const& title) {
        FakeBrowser::runtime().tabs_changed();
        auto text = title.to_utf8();
        if (text.starts_with('{'))
            report = text;
    };
    auto page_url = url(ByteString::formatted("chrome-extension://{}/test.html?{}", extension_id, query));
    view.load(page_url);

    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(30);
    Core::EventLoop::current().spin_until([&] { return report.has_value() || MonotonicTime::now() > deadline; });
    view.on_title_change = [](auto const&) { FakeBrowser::runtime().tabs_changed(); };

    if (!report.has_value()) {
        warnln("\033[31;1mFAIL\033[0m: no report from {}", page_url);
        ++s_failures;
        return {};
    }
    auto json = JsonParser::parse(*report);
    VERIFY(!json.is_error() && json.value().is_object());
    auto object = json.value().as_object();
    if (auto error = object.get_string("error"sv); error.has_value()) {
        warnln("\033[31;1mFAIL\033[0m: {} failed: {}", query, *error);
        ++s_failures;
    }
    return object;
}

String json_of(JsonObject const& report, StringView key)
{
    return report.get(key).value_or(JsonValue { "(missing)"_string }).serialized();
}

struct Pages {
    URL::URL bee;
    URL::URL sea;
    String directory_url;
};

Pages write_pages(ByteString const& directory)
{
    write_temporary_file(directory, "frame.html"sv, "<!doctype html><title>Frame</title><body>frame"sv);
    auto bee = write_temporary_file(directory, "bee.html"sv, "<!doctype html><title>Bee</title><script>window.pageGlobal = 'page';</script><body>bee<iframe src=\"frame.html\"></iframe>"sv);
    auto sea = write_temporary_file(directory, "sea.html"sv, "<!doctype html><title>Sea</title><body>sea"sv);
    write_temporary_file(directory, "dee.html"sv, "<!doctype html><title>Dee</title><body>dee"sv);
    write_temporary_file(directory, "nav.html"sv, "<!doctype html><title>Nav</title><body>nav<iframe src=\"frame.html\"></iframe>"sv);
    return {
        url(ByteString::formatted("file://{}", bee)),
        url(ByteString::formatted("file://{}", sea)),
        MUST(String::formatted("file://{}/", directory)),
    };
}

void tabs_and_windows_are_described(WebView::HeadlessWebView& view)
{
    auto report = run_case(view, tabs_extension_id, "case=describe"sv);
    CHECK_EQ(report.get_integer<i64>("count"sv).value_or(0), 3);
    CHECK(report.get_bool("ids_unique"sv).value_or(false));
    CHECK_EQ(json_of(report, "self"sv), MUST(String::formatted(R"({{"url":"chrome-extension://{}/test.html?case=describe","index":0,"active":true,"same_window_as_bee":true}})", tabs_extension_id)));
    CHECK_EQ(json_of(report, "bee"sv), R"({"title":"Bee","index":1,"active":false,"pinned":true,"status":"complete","incognito":false,"highlighted":false,"muted":false,"favicon":null})"sv);
    CHECK_EQ(json_of(report, "sea"sv), R"({"title":"Sea","active":true,"other_window":true})"sv);
    CHECK_EQ(report.get_integer<i64>("active"sv).value_or(0), 2);
    CHECK(report.get_bool("current"sv).value_or(false));
    CHECK(report.get_bool("last_focused"sv).value_or(false));
    CHECK_EQ(json_of(report, "not_current"sv), R"(["Sea"])"sv);
    CHECK_EQ(report.get_integer<i64>("tabs_in_current_window"sv).value_or(0), 2);
    CHECK_EQ(json_of(report, "url_pattern"sv), R"(["Bee"])"sv);
    CHECK_EQ(json_of(report, "url_patterns"sv), R"(["Sea"])"sv);
    CHECK(report.get_bool("own_page"sv).value_or(false));
    CHECK_EQ(json_of(report, "title_glob"sv), R"(["Bee"])"sv);
    CHECK_EQ(json_of(report, "pinned"sv), R"(["Bee"])"sv);
    CHECK_EQ(json_of(report, "popup"sv), R"(["Sea"])"sv);
    CHECK_EQ(json_of(report, "index"sv), R"(["Bee"])"sv);
    CHECK_EQ(json_of(report, "complete_popup"sv), R"(["Sea"])"sv);
    CHECK_EQ(report.get_integer<i64>("audible"sv).value_or(-1), 0);
    CHECK_EQ(string_in(report, "bad_pattern"sv), "Invalid url pattern 'not a pattern'"sv);
    CHECK(string_in(report, "bad_query"sv) != "resolved"sv);
    CHECK(string_in(report, "bad_window"sv) != "resolved"sv);
    CHECK_EQ(string_in(report, "get"sv), "Bee"sv);
    CHECK_EQ(string_in(report, "get_missing"sv), "No tab with id: 99999."sv);
    CHECK(string_in(report, "get_bad"sv) != "resolved"sv);
    CHECK(string_in(report, "get_none"sv) != "resolved"sv);
    CHECK_EQ(json_of(report, "windows"sv), R"([{"type":"normal","focused":true,"tabs":2,"incognito":false,"state":"normal"},{"type":"popup","focused":false,"tabs":1,"incognito":false,"state":"normal"}])"sv);
    CHECK(report.get_bool("unpopulated"sv).value_or(false));
    CHECK_EQ(report.get_integer<i64>("popup_only"sv).value_or(0), 1);
    CHECK(report.get_bool("window_current"sv).value_or(false));
    CHECK(report.get_bool("get_current"sv).value_or(false));
    CHECK(report.get_bool("last_focused_window"sv).value_or(false));
    CHECK_EQ(string_in(report, "window_missing"sv), "No window with id: 999."sv);
    CHECK(string_in(report, "window_bad"sv) != "resolved"sv);
    CHECK_EQ(json_of(report, "window_populated"sv), R"(["Sea"])"sv);
    CHECK_EQ(json_of(report, "window_bounds"sv), R"({"left":0,"top":0,"width":800,"height":600})"sv);
    CHECK_EQ(report.get_integer<i64>("callback_style"sv).value_or(0), 1);
}

void tabs_change_and_the_extension_hears_of_it(WebView::HeadlessWebView& view, Pages const& pages)
{
    auto report = run_case(view, tabs_extension_id, MUST(String::formatted("case=change&dir={}", pages.directory_url)));
    CHECK_EQ(json_of(report, "created"sv), R"({"pending":"dee.html","active":false,"same_window":true,"index":2})"sv);
    CHECK_EQ(json_of(report, "loaded"sv), R"({"url":"dee.html","title":"Dee","status":"complete"})"sv);
    CHECK(report.get_bool("pinned"sv).value_or(false));
    CHECK_EQ(report.get_integer<i64>("moved_index"sv).value_or(-1), 0);
    CHECK(string_in(report, "removed_gone"sv).starts_with_bytes("No tab with id: "sv));
    CHECK_EQ(json_of(report, "window"sv), R"({"type":"popup","tabs":1,"focused":true,"pending":"sea.html"})"sv);

    auto events = json_of(report, "events"sv);
    for (auto expected : {
             R"(["created","updated url=dee.html)"sv,
             R"("updated title=Dee")"sv,
             R"("updated status=complete")"sv,
             R"("updated pinned=true")"sv,
             R"("activated","updated status=loading")"sv,
             R"("updated status=loading","updated url=sea.html)"sv,
             R"("updated title=Sea")"sv,
             R"("removed")"sv,
             R"("window created","created")"sv,
             R"("focus")"sv,
             R"("removed closing")"sv,
             R"("window removed")"sv,
         }) {
        if (!events.contains(expected)) {
            warnln("\033[31;1mFAIL\033[0m: events {} don't have {}", events, expected);
            ++s_failures;
        }
    }
    CHECK(report.get_bool("focus_back_to_self"sv).value_or(false));
    CHECK(string_in(report, "bad_create"sv) != "resolved"sv);
    CHECK(string_in(report, "javascript_url"sv) != "resolved"sv);
    CHECK_EQ(string_in(report, "update_missing"sv), "No tab with id: 99999."sv);
    CHECK(string_in(report, "remove_bad"sv) != "resolved"sv);
    CHECK_EQ(string_in(report, "remove_missing"sv), "No tab with id: 99999."sv);
    CHECK_EQ(string_in(report, "window_remove_missing"sv), "No window with id: 99999."sv);
}

void messages_reach_content_scripts_by_frame(WebView::HeadlessWebView& view)
{
    auto report = run_case(view, tabs_extension_id, "case=messages"sv);
    CHECK_EQ(json_of(report, "top"sv), R"({"file":"bee.html","top":true,"sender_id":"dkcmhdciailofeligafglofekchdlkge","sender_url":true,"sender_has_tab":true})"sv);
    CHECK_EQ(json_of(report, "inner"sv), R"({"file":"frame.html","top":false})"sv);
    CHECK_EQ(string_in(report, "by_document"sv), "frame.html"sv);
    CHECK(json_of(report, "later"sv).contains(R"("later":true)"sv));
    CHECK(report.get_bool("anyone"sv).value_or(false));
    CHECK_EQ(string_in(report, "welcomed"sv), "welcome"sv);
    CHECK_EQ(json_of(report, "home"sv), R"({"file":"frame.html","tab":true,"tab_url":"bee.html","tab_title":"Bee","frame":true,"url":"frame.html","document":true,"origin":"null"})"sv);
    CHECK_EQ(json_of(report, "echo"sv), R"({"echo":"ping","name":"lane","top":true})"sv);
    CHECK_EQ(string_in(report, "callback_style"sv), "bee.html"sv);
    CHECK_EQ(string_in(report, "nobody"sv), "Could not establish connection. Receiving end does not exist."sv);
    CHECK_EQ(string_in(report, "no_frame"sv), "Could not establish connection. Receiving end does not exist."sv);
    CHECK_EQ(string_in(report, "no_tab"sv), "No tab with id: 99999."sv);
    CHECK(string_in(report, "bad_tab"sv) != "resolved"sv);
}

void scripts_run_in_tabs(WebView::HeadlessWebView& view)
{
    auto report = run_case(view, tabs_extension_id, "case=scripting"sv);
    CHECK_EQ(json_of(report, "sum"sv), R"({"result":"3Bee","frame":0,"document":"string","count":1})"sv);
    CHECK_EQ(json_of(report, "everywhere"sv), R"(["top:Bee","child:Frame"])"sv);
    CHECK_EQ(json_of(report, "in_child"sv), R"(["Frame"])"sv);
    CHECK_EQ(json_of(report, "by_document"sv), R"(["Frame"])"sv);
    CHECK_EQ(string_in(report, "isolated"sv), "undefined"sv);
    CHECK_EQ(string_in(report, "main"sv), "string"sv);
    CHECK_EQ(string_in(report, "chrome_in_isolated"sv), "function"sv);
    CHECK_EQ(json_of(report, "file"sv), R"({"injected":"Bee","isolated":"undefined"})"sv);
    CHECK_EQ(report.get_integer<i64>("promise"sv).value_or(0), 7);
    CHECK(report.get("nothing"sv).has_value() && report.get("nothing"sv)->is_null());
    CHECK(report.get("thrown"sv).has_value() && report.get("thrown"sv)->is_null());
    CHECK_EQ(string_in(report, "styled"sv), "rgb(1, 2, 3)"sv);
    CHECK_EQ(string_in(report, "unstyled"sv), "rgb(0, 0, 0)"sv);
    CHECK_EQ(string_in(report, "styled_by_file"sv), "rgb(4, 5, 6)"sv);
    CHECK(string_in(report, "restricted"sv).starts_with_bytes("Cannot access contents of the page"sv));
    CHECK(string_in(report, "no_target"sv) != "resolved"sv);
    CHECK(string_in(report, "no_code"sv) != "resolved"sv);
    CHECK_EQ(string_in(report, "missing_tab"sv), "No tab with id: 99999."sv);
    CHECK(string_in(report, "missing_frame"sv).contains("12345"sv));
    CHECK_EQ(string_in(report, "missing_file"sv), "Could not load file: 'nope.js'."sv);
    CHECK(string_in(report, "bad_world"sv) != "resolved"sv);
    CHECK(string_in(report, "unserializable_args"sv) != "resolved"sv);
    CHECK(string_in(report, "no_css"sv) != "resolved"sv);
}

void navigations_are_reported_by_frame(WebView::HeadlessWebView& view, Pages const& pages)
{
    auto report = run_case(view, tabs_extension_id, MUST(String::formatted("case=navigation&dir={}", pages.directory_url)));
    CHECK_EQ(json_of(report, "top_sequence"sv), R"(["onBeforeNavigate","onCommitted","onDOMContentLoaded","onCompleted"])"sv);
    CHECK_EQ(json_of(report, "child_sequence"sv), R"(["onBeforeNavigate","onCommitted","onDOMContentLoaded","onCompleted"])"sv);
    CHECK_EQ(report.get_integer<i64>("child_parent"sv).value_or(-5), 0);
    CHECK_EQ(report.get_integer<i64>("top_parent"sv).value_or(-5), -1);
    CHECK_EQ(json_of(report, "urls"sv), R"(["top:nav.html","child:frame.html"])"sv);
    CHECK_EQ(json_of(report, "committed"sv), R"({"transition":"string","time":"number","document":"string","frame_type":"outermost_frame"})"sv);
    CHECK_EQ(json_of(report, "frames"sv), R"(["top:-1:nav.html:false","child:0:frame.html:false"])"sv);
    CHECK_EQ(json_of(report, "top"sv), R"({"url":"nav.html","parent":-1,"type":"outermost_frame","lifecycle":"active"})"sv);
    CHECK_EQ(string_in(report, "by_document"sv), "frame.html"sv);
    CHECK(report.get("missing_frame"sv).has_value() && report.get("missing_frame"sv)->is_null());
    CHECK(report.get("missing_tab"sv).has_value() && report.get("missing_tab"sv)->is_null());
    CHECK(string_in(report, "bad_details"sv) != "resolved"sv);
}

void what_an_extension_sees_depends_on_its_permissions(WebView::HeadlessWebView& view, i64 bee, i64 sea, i64 self)
{
    auto report = run_case(view, limited_extension_id, MUST(String::formatted("case=limited&bee={}&sea={}&self={}", bee, sea, self)));
    CHECK_EQ(report.get_integer<i64>("count"sv).value_or(0), 3);
    CHECK_EQ(json_of(report, "urls"sv), R"(["bee.html",null,"sea.html"])"sv);
    CHECK_EQ(json_of(report, "titles"sv), R"(["Bee","Sea",null])"sv);
    CHECK(report.get("self_url"sv).has_value() && report.get("self_url"sv)->is_null());
    CHECK_EQ(json_of(report, "url_filter"sv), R"(["Bee","Sea"])"sv);
    CHECK_EQ(json_of(report, "title_filter"sv), R"(["Bee","Sea"])"sv);
    CHECK_EQ(string_in(report, "allowed"sv), "Bee"sv);
    CHECK(string_in(report, "own_page"sv).starts_with_bytes("Cannot access contents of the page"sv));
    CHECK_EQ(json_of(report, "child_frame"sv), R"(["Bee","Frame"])"sv);
    CHECK(string_in(report, "web_navigation"sv) != "resolved"sv);
    CHECK_EQ(string_in(report, "others_content_scripts"sv), "Could not establish connection. Receiving end does not exist."sv);
}

}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return ExtensionTests::run_extension_tests(arguments, [](ExtensionTests::Harness& harness) {
        using namespace ExtensionTests;
        MUST(harness.registry.load_unpacked(fixture("tabs"sv)));
        MUST(harness.registry.load_unpacked(fixture("tabs_limited"sv)));
        harness.registry.set_allows_file_access(tabs_extension_id, true);
        harness.registry.set_allows_file_access(limited_extension_id, true);
        auto pages = write_pages(harness.directory);

        auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
        FakeBrowser browser(MUST(Gfx::load_system_theme(theme_path.string())));
        auto first_window = browser.add_window();
        auto self = browser.add_tab(first_window, harness.view);
        auto bee = browser.add_loaded_tab(first_window, pages.bee);
        browser.set_pinned(bee, true);
        auto second_window = browser.add_window("popup"sv);
        auto sea = browser.add_loaded_tab(second_window, pages.sea);
        browser.focus(first_window);
        FakeBrowser::runtime().set_tabs_delegate(&browser);

        tabs_and_windows_are_described(harness.view);
        tabs_change_and_the_extension_hears_of_it(harness.view, pages);
        messages_reach_content_scripts_by_frame(harness.view);
        scripts_run_in_tabs(harness.view);
        what_an_extension_sees_depends_on_its_permissions(harness.view, bee, sea, self);
        navigations_are_reported_by_frame(harness.view, pages);
    });
}
