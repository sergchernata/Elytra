/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/JsonParser.h>
#include <AK/Time.h>
#include <LibURL/Parser.h>
#include <LibWebExtensions/MatchPattern.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionAPI/Management.h>
#include <LibWebView/ExtensionAPI/TabsDelegate.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

using Context = ExtensionRuntime::Context;
using Reply = ExtensionRuntime::Reply;
using Tab = ExtensionTabsDelegate::Tab;
using Window = ExtensionTabsDelegate::Window;

static constexpr i64 window_id_none = -1;
static constexpr i64 window_id_current = -2;
static constexpr auto no_receiver_error = "Could not establish connection. Receiving end does not exist."sv;
static constexpr auto no_access_error = "Cannot access contents of the page. Extension manifest must request permission to access the respective host."sv;

// chrome.tabs, chrome.windows, chrome.scripting and chrome.webNavigation, over the UI's ExtensionTabsDelegate.
class TabsAPI final : public ExtensionAPI {
public:
    explicit TabsAPI(ExtensionRuntime&);

    virtual void shut_down() override;

private:
    using Handler = Function<void(Context const&, Extension const&, JsonArray const&, Reply)>;
    // A function of extension pages and backgrounds (never content scripts), with the permission it needs, if any.
    void define(StringView name, Optional<StringView> permission, Handler);

    ExtensionTabsDelegate* delegate() const { return runtime().tabs_delegate(); }
    bool has_permission(Extension const& extension, StringView permission) const { return runtime().management().has_permission(extension.id(), permission); }
    // Whether the extension may see a tab's URL, title and icon: with "tabs", or host access to its URL.
    bool may_see(Extension const&, URL::URL const&) const;
    // Whether the extension may run scripts in a page of the URL.
    bool may_script(Extension const&, URL::URL const&) const;

    // Tabs and windows.
    Optional<Tab> find_tab(i64 id) const;
    Optional<Tab> tab_of(ViewImplementation const&) const;
    Optional<Tab> tab_of(Context const&) const;
    Optional<Window> find_window(i64 id) const;
    Optional<i64> current_window_id(Context const&) const;
    ErrorOr<Tab, String> tab_argument(Context const&, JsonValue const&, bool current_if_missing = false) const;
    ErrorOr<Window, String> window_argument(Context const&, JsonValue const&) const;
    URL::URL url_of(Tab const&) const;
    String title_of(Tab const&) const;
    StringView status_of(Tab const&) const;
    JsonObject tab_json(Tab const&, Extension const&) const;
    JsonObject window_json(Window const&, Extension const&, bool populate) const;
    ErrorOr<URL::URL, String> url_to_open(Extension const&, JsonValue const&) const;
    void open_url(i64 tab_id, URL::URL const&);

    void register_tabs_functions();
    void register_windows_functions();
    void register_scripting_functions();
    void register_web_navigation_functions();
    void register_messaging_functions();

    // Events, worked out from what the UI shows now and showed before.
    void tabs_changed();
    void watch_view(ViewImplementation&);
    // Sends an event to the extension pages and backgrounds listening, with the arguments for each extension (none for
    // an extension that mustn't hear of it).
    void dispatch(StringView event, Function<Optional<JsonArray>(Extension const&)> const& arguments);

    struct TabState {
        i64 window_id { 0 };
        i64 index { 0 };
        bool active { false };
        bool pinned { false };
        bool audible { false };
        bool muted { false };
        bool discarded { false };
        StringView status;
        URL::URL url;
        String title;
        Optional<String> fav_icon_url;
        ViewImplementation* view { nullptr };
    };
    HashMap<i64, TabState> m_tab_states;
    HashTable<i64> m_window_states;
    i64 m_focused_window { window_id_none };
    bool m_have_states { false };
    HashMap<ViewImplementation*, u64> m_watched_views;
    // URLs being loaded in tabs, until their load starts.
    HashMap<i64, URL::URL> m_pending_urls;

    // Frames: Chrome's frame IDs are 0 for a tab's top frame, and a number of the browser's for each of the others.
    i64 frame_id_of(CanonicalNavigable const&);
    Vector<CanonicalNavigable*> frames_of(ViewImplementation&) const;
    CanonicalNavigable* find_frame(ViewImplementation&, i64 frame_id);
    static String document_id_of(CanonicalNavigable const&);
    JsonObject frame_json(CanonicalNavigable&);
    URL::URL url_for_access(CanonicalNavigable const&) const;
    HashMap<Web::HTML::CrossProcessId, i64> m_frame_ids;
    i64 m_next_frame_id { 1 };

    // From the page processes.
    void page_message(WebContentPage&, JsonObject const&);
    void navigation_reported(WebContentPage&, JsonObject const&);
    void script_result(WebContentPage&, JsonObject const&);
    void page_closed(WebContentPage&);

    // Scripts run in frames: each frame's scripts in a run, whose results come back together.
    struct Injection {
        Vector<CanonicalNavigable*> frames;
        Vector<String> sources;
        Vector<String> filenames;
        bool main_world { false };
    };
    void inject(Extension const&, Injection, Function<void(Vector<JsonObject>)> on_complete);
    ErrorOr<Injection, String> injection_for(Context const&, Extension const&, JsonObject const& injection);
    struct ScriptRun {
        Function<void(Vector<JsonObject>)> on_complete;
        Vector<JsonObject> results; // One per frame: frameId and documentId, and result once known.
        size_t remaining { 0 };
    };
    struct ScriptRequest {
        RefPtr<WebContentPage> page;
        Web::HTML::CrossProcessId navigable_id;
        // The document it was sent to: only a context in that one counts as the script's, and only for a while.
        Web::HTML::CrossProcessId document_id;
        MonotonicTime sent_at;
        String extension_id;
        bool main_world { false };
        u64 run { 0 };
        size_t frame { 0 };
        bool is_last { false };
    };
    void finish_request(u64 request_id, Optional<JsonValue> result);
    HashMap<u64, ScriptRun> m_script_runs;
    HashMap<u64, ScriptRequest> m_script_requests;
    u64 m_next_run_id { 1 };
    u64 m_next_request_id { 1 };
};

NonnullOwnPtr<ExtensionAPI> create_tabs_api(ExtensionRuntime& runtime)
{
    return make<TabsAPI>(runtime);
}

static String id_string(Web::HTML::CrossProcessId id)
{
    return MUST(String::formatted("{:016X}{:016X}", id.namespace_id, id.local_id));
}

static double now_in_milliseconds()
{
    return static_cast<double>(UnixDateTime::now().nanoseconds_since_epoch()) / 1'000'000.0;
}

static JsonArray arguments_of(std::initializer_list<JsonValue> values)
{
    JsonArray array;
    for (auto const& value : values)
        array.must_append(value);
    return array;
}

static Optional<JsonValue const&> argument(JsonArray const& arguments, size_t index)
{
    if (index >= arguments.size() || arguments.at(index).is_null())
        return {};
    return arguments.at(index);
}

static ErrorOr<JsonObject, String> object_argument(JsonArray const& arguments, size_t index, StringView what, bool required = false)
{
    auto value = argument(arguments, index);
    if (!value.has_value()) {
        if (required)
            return MUST(String::formatted("Error in invocation: {} is required.", what));
        return JsonObject {};
    }
    if (!value->is_object())
        return MUST(String::formatted("Error in invocation: {} must be an object.", what));
    return value->as_object();
}

// An optional property of a type, or an error naming it.
static ErrorOr<Optional<bool>, String> optional_bool(JsonObject const& object, StringView key)
{
    auto value = object.get(key);
    if (!value.has_value() || value->is_null())
        return Optional<bool> {};
    if (!value->is_bool())
        return MUST(String::formatted("Invalid value for '{}': expected a boolean.", key));
    return Optional<bool> { value->as_bool() };
}

static ErrorOr<Optional<i64>, String> optional_integer(JsonObject const& object, StringView key)
{
    auto value = object.get(key);
    if (!value.has_value() || value->is_null())
        return Optional<i64> {};
    auto integer = value->get_integer<i64>();
    if (!integer.has_value())
        return MUST(String::formatted("Invalid value for '{}': expected an integer.", key));
    return integer;
}

static ErrorOr<Optional<String>, String> optional_string(JsonObject const& object, StringView key)
{
    auto value = object.get(key);
    if (!value.has_value() || value->is_null())
        return Optional<String> {};
    if (!value->is_string())
        return MUST(String::formatted("Invalid value for '{}': expected a string.", key));
    return Optional<String> { value->as_string() };
}

// A string or an array of strings.
static ErrorOr<Vector<String>, String> strings(JsonObject const& object, StringView key)
{
    Vector<String> result;
    auto value = object.get(key);
    if (!value.has_value() || value->is_null())
        return result;
    if (value->is_string()) {
        result.append(value->as_string());
        return result;
    }
    if (!value->is_array())
        return MUST(String::formatted("Invalid value for '{}': expected a string or an array of strings.", key));
    for (auto const& item : value->as_array().values()) {
        if (!item.is_string())
            return MUST(String::formatted("Invalid value for '{}': expected a string or an array of strings.", key));
        result.append(item.as_string());
    }
    return result;
}

static ErrorOr<i64, String> integer_argument(JsonArray const& arguments, size_t index, StringView what)
{
    auto value = argument(arguments, index);
    if (!value.has_value())
        return MUST(String::formatted("Error in invocation: {} is required.", what));
    auto integer = value->get_integer<i64>();
    if (!integer.has_value())
        return MUST(String::formatted("Error in invocation: {} must be an integer.", what));
    return *integer;
}

static String no_tab_error(i64 id)
{
    return MUST(String::formatted("No tab with id: {}.", id));
}

static String no_window_error(i64 id)
{
    return MUST(String::formatted("No window with id: {}.", id));
}

#define TRY_OR_REPLY(expression)                      \
    ({                                                \
        auto&& _result = (expression);                \
        if (_result.is_error())                       \
            return reply(_result.release_error());    \
        _result.release_value();                      \
    })

TabsAPI::TabsAPI(ExtensionRuntime& extension_runtime)
    : ExtensionAPI(extension_runtime)
{
    runtime().tabs_hooks.tabs_changed = [this] { tabs_changed(); };
    runtime().tabs_hooks.page_message = [this](WebContentPage& page, JsonObject const& message) { page_message(page, message); };
    runtime().tabs_hooks.page_closed = [this](WebContentPage& page) { page_closed(page); };
    // chrome.webRequest's frames (ExtensionAPI/WebRequest.cpp): the same tab and frame IDs as here.
    runtime().tabs_hooks.describe_frame = [this](WebContentPage& page, Web::HTML::CrossProcessId navigable_id) -> Optional<ExtensionRuntime::TabsHooks::Frame> {
        if (!page.is_open())
            return {};
        auto navigable = page.hosted_navigable(navigable_id);
        if (!navigable.has_value())
            return {};
        ExtensionRuntime::TabsHooks::Frame frame;
        frame.is_top_level = !navigable->parent();
        frame.frame_id = frame_id_of(*navigable);
        frame.parent_frame_id = navigable->parent() ? frame_id_of(*navigable->parent()) : -1;
        frame.document_id = document_id_of(*navigable);
        if (navigable->parent())
            frame.parent_document_id = document_id_of(*navigable->parent());
        if (!runtime().is_background_page(page)) {
            if (auto tab = tab_of(page.view()); tab.has_value()) {
                frame.tab_id = tab->id;
                frame.window_id = tab->window_id;
            }
        }
        return frame;
    };
    runtime().tabs_hooks.is_injecting = [this](WebContentPage const& page, Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId document_id, StringView extension_id) {
        static constexpr auto injection_window = AK::Duration::from_seconds(10);
        auto now = MonotonicTime::now();
        for (auto const& [id, request] : m_script_requests) {
            if (request.page.ptr() == &page && request.navigable_id == navigable_id && request.document_id == document_id
                && request.extension_id == extension_id && !request.main_world && now - request.sent_at < injection_window)
                return true;
        }
        return false;
    };
    runtime().tabs_hooks.describe_sender = [this](Context const& context, JsonObject& sender) {
        auto const* extension = runtime().registry().find(context.extension_id);
        auto tab = tab_of(context);
        if (!extension || !tab.has_value())
            return false;
        sender.set("tab"sv, tab_json(*tab, *extension));
        CanonicalNavigable* frame = nullptr;
        if (context.navigable_id.has_value() && tab->view->has_traversable())
            frame = tab->view->traversable().find(*context.navigable_id).ptr();
        if (frame) {
            sender.set("frameId"sv, frame_id_of(*frame));
            sender.set("documentId"sv, document_id_of(*frame));
        } else {
            sender.set("frameId"sv, context.is_top_level ? 0 : -1);
        }
        sender.set("documentLifecycle"sv, "active"sv);
        return true;
    };

    register_tabs_functions();
    register_windows_functions();
    register_scripting_functions();
    register_web_navigation_functions();
    register_messaging_functions();
}

void TabsAPI::shut_down()
{
    if (auto* tabs = delegate()) {
        for (auto const& tab : tabs->tabs()) {
            if (auto listener = m_watched_views.get(tab.view); listener.has_value())
                tab.view->remove_navigation_listener(*listener);
        }
    }
    m_watched_views.clear();
    runtime().tabs_hooks = {};
}

void TabsAPI::define(StringView name, Optional<StringView> permission, Handler handler)
{
    auto api_name = MUST(String::from_utf8(name));
    auto permission_name = permission.has_value() ? Optional<String> { MUST(String::from_utf8(*permission)) } : Optional<String> {};
    runtime().register_function(name, [this, api_name, permission_name, handler = move(handler)](Context const& context, JsonArray const& arguments, Reply reply) {
        if (context.type == "content_script"sv)
            return reply(MUST(String::formatted("{} isn't available in content scripts", api_name)));
        auto const* extension = runtime().registry().find(context.extension_id);
        if (!extension)
            return reply("The extension isn't installed"_string);
        if (permission_name.has_value() && !has_permission(*extension, *permission_name))
            return reply(MUST(String::formatted("{} needs the \"{}\" permission", api_name, *permission_name)));
        handler(context, *extension, arguments, move(reply));
    });
}

bool TabsAPI::may_see(Extension const& extension, URL::URL const& url) const
{
    return has_permission(extension, "tabs"sv) || runtime().management().has_host_permission(extension.id(), url);
}

bool TabsAPI::may_script(Extension const& extension, URL::URL const& url) const
{
    if (WebExtensions::is_restricted_url(url, runtime().registry().allows_file_access(extension.id()), extension.id().bytes_as_string_view()))
        return false;
    return runtime().management().has_host_permission(extension.id(), url);
}

Optional<Tab> TabsAPI::find_tab(i64 id) const
{
    if (!delegate())
        return {};
    for (auto const& tab : delegate()->tabs()) {
        if (tab.id == id && tab.view)
            return tab;
    }
    return {};
}

Optional<Tab> TabsAPI::tab_of(ViewImplementation const& view) const
{
    if (!delegate())
        return {};
    for (auto const& tab : delegate()->tabs()) {
        if (tab.view == &view)
            return tab;
    }
    return {};
}

Optional<Tab> TabsAPI::tab_of(Context const& context) const
{
    if (!context.page || !context.page->is_open() || runtime().is_background_page(*context.page))
        return {};
    return tab_of(context.page->view());
}

Optional<Window> TabsAPI::find_window(i64 id) const
{
    if (!delegate())
        return {};
    for (auto const& window : delegate()->windows()) {
        if (window.id == id)
            return window;
    }
    return {};
}

// The window of the caller's tab, or else the one last focused.
Optional<i64> TabsAPI::current_window_id(Context const& context) const
{
    if (!delegate())
        return {};
    if (auto tab = tab_of(context); tab.has_value())
        return tab->window_id;
    if (auto id = delegate()->last_focused_window_id(); id.has_value() && find_window(*id).has_value())
        return id;
    auto windows = delegate()->windows();
    if (windows.is_empty())
        return {};
    return windows.first().id;
}

ErrorOr<Tab, String> TabsAPI::tab_argument(Context const& context, JsonValue const& value, bool current_if_missing) const
{
    if (value.is_null() && current_if_missing) {
        // The active tab of the current window.
        auto window_id = current_window_id(context);
        if (window_id.has_value() && delegate()) {
            for (auto const& tab : delegate()->tabs()) {
                if (tab.window_id == *window_id && tab.active && tab.view)
                    return tab;
            }
        }
        return "No current tab"_string;
    }
    auto id = value.get_integer<i64>();
    if (!id.has_value())
        return "Error in invocation: tabId must be an integer."_string;
    auto tab = find_tab(*id);
    if (!tab.has_value())
        return no_tab_error(*id);
    return tab.release_value();
}

ErrorOr<Window, String> TabsAPI::window_argument(Context const& context, JsonValue const& value) const
{
    auto id = value.get_integer<i64>();
    if (!id.has_value())
        return "Error in invocation: windowId must be an integer."_string;
    if (*id == window_id_current) {
        auto current = current_window_id(context);
        if (!current.has_value())
            return "No current window"_string;
        id = current;
    }
    auto window = find_window(*id);
    if (!window.has_value())
        return no_window_error(*id);
    return window.release_value();
}

URL::URL TabsAPI::url_of(Tab const& tab) const
{
    if (!tab.view)
        return URL::about_blank();
    return tab.view->url();
}

String TabsAPI::title_of(Tab const& tab) const
{
    if (tab.title.has_value())
        return *tab.title;
    if (!tab.view)
        return {};
    return tab.view->title().to_utf8();
}

StringView TabsAPI::status_of(Tab const& tab) const
{
    if (m_pending_urls.contains(tab.id) || (tab.view && tab.view->is_loading()))
        return "loading"sv;
    return "complete"sv;
}

JsonObject TabsAPI::tab_json(Tab const& tab, Extension const& extension) const
{
    JsonObject json;
    json.set("id"sv, tab.id);
    json.set("index"sv, tab.index);
    json.set("windowId"sv, tab.window_id);
    json.set("active"sv, tab.active);
    json.set("highlighted"sv, tab.highlighted || tab.active);
    json.set("selected"sv, tab.highlighted || tab.active);
    json.set("pinned"sv, tab.pinned);
    json.set("audible"sv, tab.audible);
    json.set("discarded"sv, tab.discarded);
    json.set("autoDiscardable"sv, true);
    json.set("frozen"sv, false);
    json.set("incognito"sv, tab.incognito);
    json.set("groupId"sv, -1);
    json.set("status"sv, status_of(tab));
    JsonObject muted_info;
    muted_info.set("muted"sv, tab.muted);
    json.set("mutedInfo"sv, move(muted_info));
    if (tab.opener_tab_id.has_value())
        json.set("openerTabId"sv, *tab.opener_tab_id);
    if (auto window = find_window(tab.window_id); window.has_value() && window->bounds.has_value()) {
        json.set("width"sv, window->bounds->width());
        json.set("height"sv, window->bounds->height());
    }

    auto url = url_of(tab);
    if (may_see(extension, url)) {
        json.set("url"sv, url.serialize());
        json.set("title"sv, title_of(tab));
        if (tab.fav_icon_url.has_value())
            json.set("favIconUrl"sv, *tab.fav_icon_url);
    }
    if (auto pending = m_pending_urls.get(tab.id); pending.has_value() && may_see(extension, *pending))
        json.set("pendingUrl"sv, pending->serialize());
    return json;
}

JsonObject TabsAPI::window_json(Window const& window, Extension const& extension, bool populate) const
{
    JsonObject json;
    json.set("id"sv, window.id);
    json.set("focused"sv, window.focused);
    json.set("incognito"sv, window.incognito);
    json.set("alwaysOnTop"sv, window.always_on_top);
    json.set("type"sv, window.type);
    json.set("state"sv, window.state);
    if (window.bounds.has_value()) {
        json.set("left"sv, window.bounds->x());
        json.set("top"sv, window.bounds->y());
        json.set("width"sv, window.bounds->width());
        json.set("height"sv, window.bounds->height());
    }
    if (populate && delegate()) {
        JsonArray tabs;
        for (auto const& tab : delegate()->tabs()) {
            if (tab.window_id == window.id && tab.view)
                tabs.must_append(tab_json(tab, extension));
        }
        json.set("tabs"sv, move(tabs));
    }
    return json;
}

// A URL an extension asks a tab to load: relative to the extension's root, and never one that runs script.
ErrorOr<URL::URL, String> TabsAPI::url_to_open(Extension const& extension, JsonValue const& value) const
{
    if (!value.is_string())
        return "Invalid value for 'url': expected a string."_string;
    auto base = URL::Parser::basic_parse(MUST(String::formatted("chrome-extension://{}/", extension.id())));
    auto url = URL::Parser::basic_parse(value.as_string(), base);
    if (!url.has_value())
        return MUST(String::formatted("Invalid url: \"{}\".", value.as_string()));
    auto const& scheme = url->scheme();
    bool allowed = scheme.is_one_of("http"sv, "https"sv, "chrome-extension"sv, "data"sv)
        || (scheme == "file"sv && runtime().registry().allows_file_access(extension.id()))
        || url->serialize() == "about:blank"sv;
    if (!allowed)
        return MUST(String::formatted("Cannot navigate to {}.", url->serialize()));
    return url.release_value();
}

void TabsAPI::open_url(i64 tab_id, URL::URL const& url)
{
    auto tab = find_tab(tab_id);
    if (!tab.has_value())
        return;
    m_pending_urls.set(tab_id, url);
    tabs_changed();
    tab->view->load(url);
}

void TabsAPI::register_tabs_functions()
{
    define("tabs.get"sv, {}, [this](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
        if (!argument(arguments, 0).has_value())
            return reply("Error in invocation: tabId is required."_string);
        auto tab = TRY_OR_REPLY(tab_argument(context, arguments.at(0)));
        reply(JsonValue { tab_json(tab, extension) });
    });

    define("tabs.getCurrent"sv, {}, [this](Context const& context, Extension const& extension, JsonArray const&, Reply reply) {
        auto tab = tab_of(context);
        if (!tab.has_value())
            return reply(JsonValue {});
        reply(JsonValue { tab_json(*tab, extension) });
    });

    define("tabs.query"sv, {}, [this](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
        auto query = TRY_OR_REPLY(object_argument(arguments, 0, "queryInfo"sv, true));

        HashMap<StringView, Optional<bool>> flags;
        for (auto key : { "active"sv, "pinned"sv, "audible"sv, "muted"sv, "highlighted"sv, "discarded"sv, "currentWindow"sv, "lastFocusedWindow"sv, "autoDiscardable"sv, "frozen"sv })
            flags.set(key, TRY_OR_REPLY(optional_bool(query, key)));
        auto window_id = TRY_OR_REPLY(optional_integer(query, "windowId"sv));
        auto index = TRY_OR_REPLY(optional_integer(query, "index"sv));
        auto opener_tab_id = TRY_OR_REPLY(optional_integer(query, "openerTabId"sv));
        auto group_id = TRY_OR_REPLY(optional_integer(query, "groupId"sv));
        auto status = TRY_OR_REPLY(optional_string(query, "status"sv));
        auto title = TRY_OR_REPLY(optional_string(query, "title"sv));
        auto window_type = TRY_OR_REPLY(optional_string(query, "windowType"sv));
        Vector<WebExtensions::MatchPattern> patterns;
        auto urls = TRY_OR_REPLY(strings(query, "url"sv));
        for (auto const& string : urls) {
            auto pattern = WebExtensions::MatchPattern::parse(string, WebExtensions::MatchPattern::Schemes::IncludingExtensions);
            if (pattern.is_error())
                return reply(MUST(String::formatted("Invalid url pattern '{}'", string)));
            patterns.append(pattern.release_value());
        }

        JsonArray result;
        if (!delegate())
            return reply(JsonValue { move(result) });
        auto current = current_window_id(context);
        auto last_focused = delegate()->last_focused_window_id();
        if (window_id == window_id_current)
            window_id = current;
        auto windows = delegate()->windows();

        auto matches = [&](Tab const& tab) {
            auto flag = [&](StringView key, bool value) {
                auto wanted = flags.get(key).value();
                return !wanted.has_value() || *wanted == value;
            };
            if (!flag("active"sv, tab.active) || !flag("pinned"sv, tab.pinned) || !flag("audible"sv, tab.audible) || !flag("muted"sv, tab.muted)
                || !flag("highlighted"sv, tab.highlighted || tab.active) || !flag("discarded"sv, tab.discarded) || !flag("autoDiscardable"sv, true) || !flag("frozen"sv, false))
                return false;
            if (!flag("currentWindow"sv, current == tab.window_id) || !flag("lastFocusedWindow"sv, last_focused == tab.window_id))
                return false;
            if (window_id.has_value() && *window_id != tab.window_id)
                return false;
            if (index.has_value() && *index != tab.index)
                return false;
            if (opener_tab_id.has_value() && tab.opener_tab_id != opener_tab_id)
                return false;
            if (group_id.has_value() && *group_id != -1)
                return false;
            if (status.has_value() && *status != status_of(tab))
                return false;
            if (window_type.has_value()) {
                auto window = windows.first_matching([&](auto const& window) { return window.id == tab.window_id; });
                if (!window.has_value() || window->type != *window_type)
                    return false;
            }
            if (title.has_value() || !patterns.is_empty()) {
                // Tabs whose URL the extension mayn't see don't match by URL or title.
                auto url = url_of(tab);
                if (!may_see(extension, url))
                    return false;
                if (title.has_value() && !WebExtensions::glob_matches(*title, title_of(tab)))
                    return false;
                if (!patterns.is_empty() && !any_of(patterns, [&](auto const& pattern) { return pattern.matches(url); }))
                    return false;
            }
            return true;
        };
        for (auto const& tab : delegate()->tabs()) {
            if (tab.view && matches(tab))
                result.must_append(tab_json(tab, extension));
        }
        reply(JsonValue { move(result) });
    });

    define("tabs.create"sv, {}, [this](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
        auto properties = TRY_OR_REPLY(object_argument(arguments, 0, "createProperties"sv, true));
        if (!delegate())
            return reply("No window"_string);
        ExtensionTabsDelegate::CreateTab create;
        create.window_id = TRY_OR_REPLY(optional_integer(properties, "windowId"sv));
        if (create.window_id == window_id_current)
            create.window_id = current_window_id(context);
        else if (create.window_id.has_value() && !find_window(*create.window_id).has_value())
            return reply(no_window_error(*create.window_id));
        if (!create.window_id.has_value())
            create.window_id = current_window_id(context);
        create.index = TRY_OR_REPLY(optional_integer(properties, "index"sv));
        create.active = TRY_OR_REPLY(optional_bool(properties, "active"sv)).value_or(TRY_OR_REPLY(optional_bool(properties, "selected"sv)).value_or(true));
        create.pinned = TRY_OR_REPLY(optional_bool(properties, "pinned"sv)).value_or(false);
        create.opener_tab_id = TRY_OR_REPLY(optional_integer(properties, "openerTabId"sv));
        if (create.opener_tab_id.has_value() && !find_tab(*create.opener_tab_id).has_value())
            return reply(no_tab_error(*create.opener_tab_id));
        if (auto url = properties.get("url"sv); url.has_value() && !url->is_null())
            create.url = TRY_OR_REPLY(url_to_open(extension, *url));

        auto url = create.url;
        auto id = TRY_OR_REPLY(delegate()->create_tab(move(create)));
        if (url.has_value())
            open_url(id, *url);
        else
            tabs_changed();
        auto tab = find_tab(id);
        if (!tab.has_value())
            return reply(no_tab_error(id));
        reply(JsonValue { tab_json(*tab, extension) });
    });

    define("tabs.duplicate"sv, {}, [this](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
        if (!argument(arguments, 0).has_value())
            return reply("Error in invocation: tabId is required."_string);
        auto tab = TRY_OR_REPLY(tab_argument(context, arguments.at(0)));
        auto url = url_of(tab);
        auto id = TRY_OR_REPLY(delegate()->create_tab({ .window_id = tab.window_id, .index = tab.index + 1, .url = url, .active = true, .pinned = tab.pinned, .opener_tab_id = tab.id }));
        open_url(id, url);
        auto duplicate = find_tab(id);
        if (!duplicate.has_value())
            return reply(JsonValue {});
        reply(JsonValue { tab_json(*duplicate, extension) });
    });

    define("tabs.update"sv, {}, [this](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
        // update([tabId], updateProperties)
        bool has_tab_id = arguments.size() >= 2 || (arguments.size() == 1 && !arguments.at(0).is_object());
        auto tab = TRY_OR_REPLY(tab_argument(context, has_tab_id ? arguments.at(0) : JsonValue {}, true));
        auto properties = TRY_OR_REPLY(object_argument(arguments, has_tab_id ? 1 : 0, "updateProperties"sv, true));

        ExtensionTabsDelegate::UpdateTab update;
        update.active = TRY_OR_REPLY(optional_bool(properties, "active"sv));
        if (!update.active.has_value())
            update.active = TRY_OR_REPLY(optional_bool(properties, "selected"sv));
        update.highlighted = TRY_OR_REPLY(optional_bool(properties, "highlighted"sv));
        update.pinned = TRY_OR_REPLY(optional_bool(properties, "pinned"sv));
        update.muted = TRY_OR_REPLY(optional_bool(properties, "muted"sv));
        update.opener_tab_id = TRY_OR_REPLY(optional_integer(properties, "openerTabId"sv));
        if (update.opener_tab_id.has_value() && !find_tab(*update.opener_tab_id).has_value())
            return reply(no_tab_error(*update.opener_tab_id));
        Optional<URL::URL> url;
        if (auto value = properties.get("url"sv); value.has_value() && !value->is_null())
            url = TRY_OR_REPLY(url_to_open(extension, *value));

        TRY_OR_REPLY(delegate()->update_tab(tab.id, move(update)));
        if (url.has_value())
            open_url(tab.id, *url);
        else
            tabs_changed();
        auto updated = find_tab(tab.id);
        if (!updated.has_value())
            return reply(JsonValue {});
        reply(JsonValue { tab_json(*updated, extension) });
    });

    define("tabs.move"sv, {}, [this](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
        auto ids_value = argument(arguments, 0);
        auto properties = TRY_OR_REPLY(object_argument(arguments, 1, "moveProperties"sv, true));
        auto index = TRY_OR_REPLY(optional_integer(properties, "index"sv));
        if (!index.has_value())
            return reply("Error in invocation: moveProperties.index is required."_string);
        auto window_id = TRY_OR_REPLY(optional_integer(properties, "windowId"sv));
        if (window_id == window_id_current)
            window_id = current_window_id(context);

        Vector<Tab> tabs;
        bool single = ids_value.has_value() && !ids_value->is_array();
        if (!ids_value.has_value())
            return reply("Error in invocation: tabIds is required."_string);
        for (auto const& value : single ? Vector<JsonValue> { *ids_value } : ids_value->as_array().values())
            tabs.append(TRY_OR_REPLY(tab_argument(context, value)));

        auto position = *index;
        for (auto const& tab : tabs) {
            auto target = window_id.value_or(tab.window_id);
            if (!find_window(target).has_value())
                return reply(no_window_error(target));
            TRY_OR_REPLY(delegate()->move_tab(tab.id, target, position));
            if (position >= 0)
                ++position;
        }
        tabs_changed();

        JsonArray moved;
        for (auto const& tab : tabs) {
            if (auto now = find_tab(tab.id); now.has_value())
                moved.must_append(tab_json(*now, extension));
        }
        if (single)
            return reply(moved.is_empty() ? JsonValue {} : moved.at(0));
        reply(JsonValue { move(moved) });
    });

    define("tabs.reload"sv, {}, [this](Context const& context, Extension const&, JsonArray const& arguments, Reply reply) {
        bool has_tab_id = argument(arguments, 0).has_value() && !arguments.at(0).is_object();
        auto tab = TRY_OR_REPLY(tab_argument(context, has_tab_id ? arguments.at(0) : JsonValue {}, true));
        tab.view->reload();
        reply(JsonValue {});
    });

    for (auto name : { "tabs.goBack"sv, "tabs.goForward"sv }) {
        int delta = name == "tabs.goBack"sv ? -1 : 1;
        define(name, {}, [this, delta](Context const& context, Extension const&, JsonArray const& arguments, Reply reply) {
            auto tab = TRY_OR_REPLY(tab_argument(context, argument(arguments, 0).value_or(JsonValue {}), true));
            tab.view->traverse_the_history_by_delta(delta);
            reply(JsonValue {});
        });
    }

    define("tabs.remove"sv, {}, [this](Context const& context, Extension const&, JsonArray const& arguments, Reply reply) {
        auto ids_value = argument(arguments, 0);
        if (!ids_value.has_value())
            return reply("Error in invocation: tabIds is required."_string);
        Vector<Tab> tabs;
        for (auto const& value : ids_value->is_array() ? ids_value->as_array().values() : Vector<JsonValue> { *ids_value })
            tabs.append(TRY_OR_REPLY(tab_argument(context, value)));
        // The caller may be one of them: it hears back before its tab goes.
        reply(JsonValue {});
        for (auto const& tab : tabs)
            delegate()->close_tab(tab.id);
        tabs_changed();
    });

    define("tabs.captureVisibleTab"sv, {}, [this](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
        // captureVisibleTab([windowId], [options])
        size_t options_index = argument(arguments, 0).has_value() && arguments.at(0).is_object() ? 0 : 1;
        auto window = TRY_OR_REPLY(window_argument(context, options_index == 1 && argument(arguments, 0).has_value() ? arguments.at(0) : JsonValue { window_id_current }));
        auto options = TRY_OR_REPLY(object_argument(arguments, options_index, "options"sv));
        auto format = TRY_OR_REPLY(optional_string(options, "format"sv)).value_or("jpeg"_string);
        if (!format.is_one_of("jpeg"sv, "png"sv))
            return reply("Invalid value for 'format'."_string);
        auto quality = clamp(TRY_OR_REPLY(optional_integer(options, "quality"sv)).value_or(92), 0, 100);

        Optional<Tab> active;
        for (auto const& tab : delegate()->tabs()) {
            if (tab.window_id == window.id && tab.active)
                active = tab;
        }
        if (!active.has_value() || !active->view)
            return reply("No active tab"_string);
        if (!may_script(extension, url_of(*active)))
            return reply("Either the '<all_urls>' or 'activeTab' permission is required."_string);
        delegate()->capture_visible_tab(window.id, format, static_cast<u8>(quality), [reply = move(reply)](ErrorOr<String, String> result) {
            if (result.is_error())
                return reply(result.release_error());
            reply(JsonValue { result.release_value() });
        });
    });

    // MV2's tabs.executeScript, insertCSS and removeCSS: (tabId?, details { code | file, allFrames, frameId }).
    for (auto name : { "tabs.executeScript"sv, "tabs.insertCSS"sv, "tabs.removeCSS"sv }) {
        bool is_css = name != "tabs.executeScript"sv;
        bool remove = name == "tabs.removeCSS"sv;
        define(name, {}, [this, is_css, remove](Context const& context, Extension const&, JsonArray const& arguments, Reply reply) {
            bool has_tab_id = argument(arguments, 0).has_value() && !arguments.at(0).is_object();
            auto tab = TRY_OR_REPLY(tab_argument(context, has_tab_id ? arguments.at(0) : JsonValue {}, true));
            auto details = TRY_OR_REPLY(object_argument(arguments, has_tab_id ? 1 : 0, "details"sv, true));
            JsonObject target;
            target.set("tabId"sv, tab.id);
            if (auto all = TRY_OR_REPLY(optional_bool(details, "allFrames"sv)); all.has_value())
                target.set("allFrames"sv, *all);
            if (auto frame = TRY_OR_REPLY(optional_integer(details, "frameId"sv)); frame.has_value())
                target.set("frameIds"sv, arguments_of({ *frame }));
            JsonObject injection;
            injection.set("target"sv, move(target));
            auto code = TRY_OR_REPLY(optional_string(details, "code"sv));
            auto file = TRY_OR_REPLY(optional_string(details, "file"sv));
            if (code.has_value() == file.has_value())
                return reply("Exactly one of 'code' or 'file' must be specified."_string);
            if (code.has_value())
                injection.set(is_css ? "css"sv : "js"sv, *code);
            else
                injection.set("files"sv, arguments_of({ *file }));
            auto api = is_css ? (remove ? "scripting.removeCSS"sv : "scripting.insertCSS"sv) : "scripting.executeScript"sv;
            runtime().call_function(api, context, arguments_of({ move(injection) }), [reply = move(reply), is_css](ErrorOr<JsonValue, String> result) {
                if (result.is_error() || is_css)
                    return reply(move(result));
                // Only the results, as MV2 had them.
                JsonArray values;
                if (result.value().is_array()) {
                    for (auto const& entry : result.value().as_array().values())
                        values.must_append(entry.is_object() ? entry.as_object().get("result"sv).value_or(JsonValue {}) : JsonValue {});
                }
                reply(JsonValue { move(values) });
            });
        });
    }
}

void TabsAPI::register_windows_functions()
{
    // getAll([queryOptions]) and get(windowId, [queryOptions]): { populate, windowTypes }.
    struct WindowQuery {
        bool populate { false };
        Vector<String> types;
    };
    auto populate_and_types = [](JsonObject const& options) -> ErrorOr<WindowQuery, String> {
        auto populate = TRY(optional_bool(options, "populate"sv)).value_or(false);
        auto types = TRY(strings(options, "windowTypes"sv));
        return WindowQuery { populate, move(types) };
    };

    define("windows.get"sv, {}, [this, populate_and_types](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
        if (!argument(arguments, 0).has_value())
            return reply("Error in invocation: windowId is required."_string);
        auto window = TRY_OR_REPLY(window_argument(context, arguments.at(0)));
        auto options = TRY_OR_REPLY(populate_and_types(TRY_OR_REPLY(object_argument(arguments, 1, "queryOptions"sv))));
        reply(JsonValue { window_json(window, extension, options.populate) });
    });

    for (auto name : { "windows.getCurrent"sv, "windows.getLastFocused"sv }) {
        bool last_focused = name == "windows.getLastFocused"sv;
        define(name, {}, [this, last_focused, populate_and_types](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
            auto options = TRY_OR_REPLY(populate_and_types(TRY_OR_REPLY(object_argument(arguments, 0, "queryOptions"sv))));
            Optional<i64> id = last_focused && delegate() ? delegate()->last_focused_window_id() : current_window_id(context);
            auto window = id.has_value() ? find_window(*id) : Optional<Window> {};
            if (!window.has_value())
                return reply("No current window"_string);
            reply(JsonValue { window_json(*window, extension, options.populate) });
        });
    }

    define("windows.getAll"sv, {}, [this, populate_and_types](Context const&, Extension const& extension, JsonArray const& arguments, Reply reply) {
        auto options = TRY_OR_REPLY(populate_and_types(TRY_OR_REPLY(object_argument(arguments, 0, "queryOptions"sv))));
        JsonArray windows;
        if (delegate()) {
            for (auto const& window : delegate()->windows()) {
                auto const& types = options.types;
                bool wanted = types.is_empty() ? window.type != "devtools"sv : types.contains_slow(window.type);
                if (wanted)
                    windows.must_append(window_json(window, extension, options.populate));
            }
        }
        reply(JsonValue { move(windows) });
    });

    define("windows.create"sv, {}, [this](Context const&, Extension const& extension, JsonArray const& arguments, Reply reply) {
        auto data = TRY_OR_REPLY(object_argument(arguments, 0, "createData"sv));
        if (!delegate())
            return reply("Windows can't be created"_string);
        ExtensionTabsDelegate::CreateWindow create;
        for (auto const& string : TRY_OR_REPLY(strings(data, "url"sv)))
            create.urls.append(TRY_OR_REPLY(url_to_open(extension, JsonValue { string })));
        create.tab_id = TRY_OR_REPLY(optional_integer(data, "tabId"sv));
        if (create.tab_id.has_value() && !find_tab(*create.tab_id).has_value())
            return reply(no_tab_error(*create.tab_id));
        auto type = TRY_OR_REPLY(optional_string(data, "type"sv)).value_or("normal"_string);
        if (!type.is_one_of("normal"sv, "popup"sv, "panel"sv))
            return reply("Invalid value for 'type'."_string);
        create.type = type == "popup"sv ? "popup"sv : type == "panel"sv ? "panel"sv : "normal"sv;
        if (auto state = TRY_OR_REPLY(optional_string(data, "state"sv)); state.has_value()) {
            if (!state->is_one_of("normal"sv, "minimized"sv, "maximized"sv, "fullscreen"sv))
                return reply("Invalid value for 'state'."_string);
            create.state = *state == "minimized"sv ? "minimized"sv : *state == "maximized"sv ? "maximized"sv : *state == "fullscreen"sv ? "fullscreen"sv : "normal"sv;
        }
        create.focused = TRY_OR_REPLY(optional_bool(data, "focused"sv)).value_or(true);
        create.incognito = TRY_OR_REPLY(optional_bool(data, "incognito"sv)).value_or(false);
        auto left = TRY_OR_REPLY(optional_integer(data, "left"sv));
        auto top = TRY_OR_REPLY(optional_integer(data, "top"sv));
        auto width = TRY_OR_REPLY(optional_integer(data, "width"sv));
        auto height = TRY_OR_REPLY(optional_integer(data, "height"sv));
        if (left.has_value() || top.has_value() || width.has_value() || height.has_value()) {
            auto in_range = [](Optional<i64> value, i64 fallback) { return static_cast<int>(clamp(value.value_or(fallback), -100000, 100000)); };
            create.bounds = Gfx::IntRect { in_range(left, 0), in_range(top, 0), max(1, in_range(width, 800)), max(1, in_range(height, 600)) };
        }

        auto urls = create.urls;
        auto id = TRY_OR_REPLY(delegate()->create_window(move(create)));
        tabs_changed();
        size_t loaded = 0;
        for (auto const& tab : delegate()->tabs()) {
            if (tab.window_id == id && tab.view && loaded < urls.size())
                open_url(tab.id, urls[loaded++]);
        }
        auto window = find_window(id);
        if (!window.has_value())
            return reply(JsonValue {});
        reply(JsonValue { window_json(*window, extension, true) });
    });

    define("windows.update"sv, {}, [this](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
        if (!argument(arguments, 0).has_value())
            return reply("Error in invocation: windowId is required."_string);
        auto window = TRY_OR_REPLY(window_argument(context, arguments.at(0)));
        auto info = TRY_OR_REPLY(object_argument(arguments, 1, "updateInfo"sv, true));
        ExtensionTabsDelegate::UpdateWindow update;
        update.focused = TRY_OR_REPLY(optional_bool(info, "focused"sv));
        update.draw_attention = TRY_OR_REPLY(optional_bool(info, "drawAttention"sv));
        if (auto state = TRY_OR_REPLY(optional_string(info, "state"sv)); state.has_value()) {
            if (!state->is_one_of("normal"sv, "minimized"sv, "maximized"sv, "fullscreen"sv))
                return reply("Invalid value for 'state'."_string);
            update.state = *state == "minimized"sv ? "minimized"sv : *state == "maximized"sv ? "maximized"sv : *state == "fullscreen"sv ? "fullscreen"sv : "normal"sv;
        }
        auto coordinate = [&](StringView key) -> ErrorOr<Optional<i32>, String> {
            auto value = TRY(optional_integer(info, key));
            if (!value.has_value())
                return Optional<i32> {};
            return Optional<i32> { static_cast<i32>(clamp(*value, -100000, 100000)) };
        };
        update.left = TRY_OR_REPLY(coordinate("left"sv));
        update.top = TRY_OR_REPLY(coordinate("top"sv));
        update.width = TRY_OR_REPLY(coordinate("width"sv));
        update.height = TRY_OR_REPLY(coordinate("height"sv));
        TRY_OR_REPLY(delegate()->update_window(window.id, move(update)));
        tabs_changed();
        auto updated = find_window(window.id);
        if (!updated.has_value())
            return reply(JsonValue {});
        reply(JsonValue { window_json(*updated, extension, false) });
    });

    define("windows.remove"sv, {}, [this](Context const& context, Extension const&, JsonArray const& arguments, Reply reply) {
        if (!argument(arguments, 0).has_value())
            return reply("Error in invocation: windowId is required."_string);
        auto window = TRY_OR_REPLY(window_argument(context, arguments.at(0)));
        reply(JsonValue {});
        delegate()->close_window(window.id);
        tabs_changed();
    });
}

// Frames.
i64 TabsAPI::frame_id_of(CanonicalNavigable const& navigable)
{
    if (navigable.is_top_level_traversable())
        return 0;
    return m_frame_ids.ensure(navigable.id(), [&] { return m_next_frame_id++; });
}

Vector<CanonicalNavigable*> TabsAPI::frames_of(ViewImplementation& view) const
{
    Vector<CanonicalNavigable*> frames;
    if (!view.has_traversable())
        return frames;
    view.traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable& navigable) {
        frames.append(&navigable);
        return IterationDecision::Continue;
    });
    return frames;
}

CanonicalNavigable* TabsAPI::find_frame(ViewImplementation& view, i64 frame_id)
{
    for (auto* frame : frames_of(view)) {
        if (frame_id_of(*frame) == frame_id)
            return frame;
    }
    return nullptr;
}

String TabsAPI::document_id_of(CanonicalNavigable const& navigable)
{
    return id_string(navigable.active_document().id());
}

// The URL whose access decides whether a frame may be scripted: an about:blank or srcdoc frame's creator's, when it has
// its creator's origin, or else its origin's.
URL::URL TabsAPI::url_for_access(CanonicalNavigable const& navigable) const
{
    auto const& url = navigable.active_document_url();
    if (url.scheme() != "about"sv)
        return url;
    return ExtensionRuntime::creator_url_for_matching(navigable).value_or(url);
}

JsonObject TabsAPI::frame_json(CanonicalNavigable& navigable)
{
    JsonObject json;
    json.set("frameId"sv, frame_id_of(navigable));
    json.set("parentFrameId"sv, navigable.parent() ? frame_id_of(*navigable.parent()) : -1);
    json.set("url"sv, navigable.active_document_url().serialize());
    json.set("errorOccurred"sv, false);
    json.set("documentId"sv, document_id_of(navigable));
    if (navigable.parent())
        json.set("parentDocumentId"sv, document_id_of(*navigable.parent()));
    json.set("frameType"sv, navigable.parent() ? "sub_frame"sv : "outermost_frame"sv);
    json.set("documentLifecycle"sv, "active"sv);
    json.set("processId"sv, -1);
    return json;
}

void TabsAPI::register_web_navigation_functions()
{
    define("webNavigation.getAllFrames"sv, "webNavigation"sv, [this](Context const&, Extension const&, JsonArray const& arguments, Reply reply) {
        auto details = TRY_OR_REPLY(object_argument(arguments, 0, "details"sv, true));
        auto tab_id = TRY_OR_REPLY(optional_integer(details, "tabId"sv));
        if (!tab_id.has_value())
            return reply("Error in invocation: details.tabId is required."_string);
        auto tab = find_tab(*tab_id);
        if (!tab.has_value())
            return reply(JsonValue {});
        JsonArray frames;
        for (auto* frame : frames_of(*tab->view))
            frames.must_append(frame_json(*frame));
        reply(JsonValue { move(frames) });
    });

    define("webNavigation.getFrame"sv, "webNavigation"sv, [this](Context const&, Extension const&, JsonArray const& arguments, Reply reply) {
        auto details = TRY_OR_REPLY(object_argument(arguments, 0, "details"sv, true));
        auto tab_id = TRY_OR_REPLY(optional_integer(details, "tabId"sv));
        auto frame_id = TRY_OR_REPLY(optional_integer(details, "frameId"sv));
        auto document_id = TRY_OR_REPLY(optional_string(details, "documentId"sv));
        if (!document_id.has_value() && (!tab_id.has_value() || !frame_id.has_value()))
            return reply("Error in invocation: details needs a documentId, or a tabId and frameId."_string);
        if (!delegate())
            return reply(JsonValue {});
        for (auto const& tab : delegate()->tabs()) {
            if (!tab.view || (tab_id.has_value() && tab.id != *tab_id))
                continue;
            for (auto* frame : frames_of(*tab.view)) {
                if (frame_id.has_value() && frame_id_of(*frame) != *frame_id)
                    continue;
                if (document_id.has_value() && document_id_of(*frame) != *document_id)
                    continue;
                auto json = frame_json(*frame);
                json.remove("frameId"sv);
                return reply(JsonValue { move(json) });
            }
        }
        reply(JsonValue {});
    });
}

// scripting.executeScript, insertCSS and removeCSS.
ErrorOr<TabsAPI::Injection, String> TabsAPI::injection_for(Context const& context, Extension const& extension, JsonObject const& injection)
{
    auto target_value = injection.get("target"sv);
    if (!target_value.has_value() || !target_value->is_object())
        return "Error in invocation: injection.target is required."_string;
    auto const& target = target_value->as_object();
    auto tab_id = TRY(optional_integer(target, "tabId"sv));
    if (!tab_id.has_value())
        return "Error in invocation: target.tabId is required."_string;
    auto tab = TRY(tab_argument(context, JsonValue { *tab_id }));
    auto all_frames = TRY(optional_bool(target, "allFrames"sv)).value_or(false);

    Injection result;
    Vector<CanonicalNavigable*> chosen;
    bool explicit_frames = false;
    if (auto frame_ids = target.get("frameIds"sv); frame_ids.has_value() && !frame_ids->is_null()) {
        if (!frame_ids->is_array())
            return "Invalid value for 'frameIds'."_string;
        explicit_frames = true;
        for (auto const& value : frame_ids->as_array().values()) {
            auto id = value.get_integer<i64>();
            if (!id.has_value())
                return "Invalid value for 'frameIds'."_string;
            auto* frame = find_frame(*tab.view, *id);
            if (!frame)
                return MUST(String::formatted("No frame with id {} in tab with id {}", *id, tab.id));
            if (!chosen.contains_slow(frame))
                chosen.append(frame);
        }
    }
    if (target.has("documentIds"sv)) {
        auto document_ids = TRY(strings(target, "documentIds"sv));
        explicit_frames = true;
        for (auto const& id : document_ids) {
            auto frames = frames_of(*tab.view);
            auto frame = frames.first_matching([&](auto* frame) { return document_id_of(*frame) == id; });
            if (!frame.has_value())
                return MUST(String::formatted("No document with id {} in tab with id {}", id, tab.id));
            if (!chosen.contains_slow(*frame))
                chosen.append(*frame);
        }
    }
    if (explicit_frames && all_frames)
        return "Cannot specify 'allFrames' if either 'frameIds' or 'documentIds' is specified."_string;

    if (explicit_frames) {
        for (auto* frame : chosen) {
            if (!may_script(extension, url_for_access(*frame)))
                return MUST(String::from_utf8(no_access_error));
        }
        result.frames = move(chosen);
    } else {
        auto frames = frames_of(*tab.view);
        if (frames.is_empty() || !may_script(extension, url_for_access(*frames.first())))
            return MUST(String::from_utf8(no_access_error));
        for (auto* frame : frames) {
            // With allFrames, those the extension mayn't script are left out.
            if (frame == frames.first() || (all_frames && may_script(extension, url_for_access(*frame))))
                result.frames.append(frame);
        }
    }

    auto world = TRY(optional_string(injection, "world"sv)).value_or("ISOLATED"_string);
    if (!world.is_one_of("ISOLATED"sv, "MAIN"sv))
        return "Invalid value for 'world'."_string;
    result.main_world = world == "MAIN"sv;
    return result;
}

static String css_script(StringView css, bool insert)
{
    // Kept in the extension's world, so that the page can't take the sheets away, and removeCSS finds them.
    auto text = JsonValue { MUST(String::from_utf8(css)) }.serialized();
    if (insert) {
        return MUST(String::formatted(R"~~~((() => {{
    const sheets = globalThis[Symbol.for("ladybird.extensionStyleSheets")] ??= new Map();
    const text = {};
    const sheet = new CSSStyleSheet();
    sheet.replaceSync(text);
    document.adoptedStyleSheets = [...document.adoptedStyleSheets, sheet];
    sheets.set(text, [...(sheets.get(text) ?? []), sheet]);
}})();)~~~",
            text));
    }
    return MUST(String::formatted(R"~~~((() => {{
    const sheets = globalThis[Symbol.for("ladybird.extensionStyleSheets")];
    const sheet = sheets?.get({})?.pop();
    if (sheet)
        document.adoptedStyleSheets = document.adoptedStyleSheets.filter(candidate => candidate !== sheet);
}})();)~~~",
        text));
}

void TabsAPI::register_scripting_functions()
{
    define("scripting.executeScript"sv, "scripting"sv, [this](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
        auto injection = TRY_OR_REPLY(object_argument(arguments, 0, "injection"sv, true));
        auto plan = TRY_OR_REPLY(injection_for(context, extension, injection));
        auto js = TRY_OR_REPLY(optional_string(injection, "js"sv));
        auto files = TRY_OR_REPLY(strings(injection, "files"sv));
        if (injection.has("func"sv))
            return reply("Invalid value for 'func': expected a function."_string);
        if (js.has_value() == !files.is_empty())
            return reply("Exactly one of 'func' and 'files' must be specified."_string);
        if (js.has_value()) {
            plan.sources.append(*js);
            plan.filenames.append(MUST(String::formatted("chrome-extension://{}/", extension.id())));
        }
        for (auto const& file : files) {
            auto source = extension.read_file(file);
            if (!source.has_value())
                return reply(MUST(String::formatted("Could not load file: '{}'.", file)));
            plan.sources.append(source.release_value());
            plan.filenames.append(MUST(String::formatted("chrome-extension://{}/{}", extension.id(), file.bytes_as_string_view().trim("/"sv, TrimMode::Left))));
        }
        inject(extension, move(plan), [reply = move(reply)](Vector<JsonObject> results) {
            JsonArray array;
            for (auto& result : results)
                array.must_append(move(result));
            reply(JsonValue { move(array) });
        });
    });

    for (auto name : { "scripting.insertCSS"sv, "scripting.removeCSS"sv }) {
        bool insert = name == "scripting.insertCSS"sv;
        define(name, "scripting"sv, [this, insert](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
            auto injection = TRY_OR_REPLY(object_argument(arguments, 0, "injection"sv, true));
            auto plan = TRY_OR_REPLY(injection_for(context, extension, injection));
            auto css = TRY_OR_REPLY(optional_string(injection, "css"sv));
            auto files = TRY_OR_REPLY(strings(injection, "files"sv));
            if (auto origin = TRY_OR_REPLY(optional_string(injection, "origin"sv)); origin.has_value() && !origin->is_one_of("AUTHOR"sv, "USER"sv))
                return reply("Invalid value for 'origin'."_string);
            if (css.has_value() == !files.is_empty())
                return reply("Exactly one of 'css' and 'files' must be specified."_string);
            Vector<String> sheets;
            if (css.has_value())
                sheets.append(*css);
            for (auto const& file : files) {
                auto source = extension.read_file(file);
                if (!source.has_value())
                    return reply(MUST(String::formatted("Could not load file: '{}'.", file)));
                sheets.append(source.release_value());
            }
            for (auto const& sheet : sheets) {
                plan.sources.append(css_script(sheet, insert));
                plan.filenames.append(MUST(String::formatted("chrome-extension://{}/", extension.id())));
            }
            plan.main_world = false;
            inject(extension, move(plan), [reply = move(reply)](Vector<JsonObject>) {
                reply(JsonValue {});
            });
        });
    }
}

void TabsAPI::inject(Extension const& extension, Injection injection, Function<void(Vector<JsonObject>)> on_complete)
{
    auto run_id = m_next_run_id++;
    ScriptRun run { .on_complete = move(on_complete), .results = {}, .remaining = 0 };
    struct Request {
        u64 id { 0 };
        RefPtr<WebContentPage> page;
        Web::HTML::CrossProcessId navigable_id;
        size_t source { 0 };
    };
    Vector<Request> to_send;
    for (size_t index = 0; index < injection.frames.size(); ++index) {
        auto* frame = injection.frames[index];
        JsonObject result;
        result.set("frameId"sv, frame_id_of(*frame));
        result.set("documentId"sv, document_id_of(*frame));
        run.results.append(move(result));
        auto host = frame->active_document().host();
        if (!host || !host->is_open())
            continue;
        for (size_t source = 0; source < injection.sources.size(); ++source) {
            auto request_id = m_next_request_id++;
            m_script_requests.set(request_id, { .page = host, .navigable_id = frame->id(), .document_id = frame->active_document().id(), .sent_at = MonotonicTime::now(), .extension_id = extension.id(), .main_world = injection.main_world, .run = run_id, .frame = index, .is_last = source + 1 == injection.sources.size() });
            to_send.append({ request_id, host, frame->id(), source });
            ++run.remaining;
        }
    }
    if (run.remaining == 0) {
        run.on_complete(move(run.results));
        return;
    }
    m_script_runs.set(run_id, move(run));
    for (auto const& request : to_send)
        request.page->async_run_extension_script(request.id, request.navigable_id, extension.id(), injection.main_world, injection.sources[request.source], injection.filenames[request.source]);
}

void TabsAPI::finish_request(u64 request_id, Optional<JsonValue> result)
{
    auto request = m_script_requests.take(request_id);
    if (!request.has_value())
        return;
    auto run = m_script_runs.find(request->run);
    if (run == m_script_runs.end())
        return;
    if (request->is_last && result.has_value())
        run->value.results[request->frame].set("result"sv, result.release_value());
    if (--run->value.remaining > 0)
        return;
    auto finished = m_script_runs.take(request->run).release_value();
    finished.on_complete(move(finished.results));
}

void TabsAPI::script_result(WebContentPage& page, JsonObject const& message)
{
    auto request_id = message.get_u64("request"sv);
    if (!request_id.has_value())
        return;
    auto request = m_script_requests.get(*request_id);
    // Only the page the script was sent to may answer for it.
    if (!request.has_value() || request->page.ptr() != &page)
        return;
    Optional<JsonValue> result;
    if (auto text = message.get_string("result"sv); text.has_value()) {
        if (auto parsed = JsonParser::parse(*text); !parsed.is_error())
            result = parsed.release_value();
    } else if (!message.has("error"sv)) {
        result = JsonValue {};
    }
    finish_request(*request_id, move(result));
}

void TabsAPI::page_closed(WebContentPage& page)
{
    Vector<u64> unanswered;
    for (auto const& [id, request] : m_script_requests) {
        if (request.page.ptr() == &page)
            unanswered.append(id);
    }
    for (auto id : unanswered)
        finish_request(id, {});
}

void TabsAPI::page_message(WebContentPage& page, JsonObject const& message)
{
    auto type = message.get_string("type"sv);
    if (type == "script_result"sv)
        script_result(page, message);
    else if (type == "navigation"sv)
        navigation_reported(page, message);
}

// A frame's document committed, finished parsing or loaded, as its page reports it.
void TabsAPI::navigation_reported(WebContentPage& page, JsonObject const& message)
{
    auto event = message.get_string("event"sv);
    auto navigable_object = message.get_object("navigable"sv);
    auto url_string = message.get_string("url"sv);
    if (!event.has_value() || !navigable_object.has_value() || !url_string.has_value() || !page.is_open())
        return;
    auto namespace_id = navigable_object->get_u64("ns"sv);
    auto local_id = navigable_object->get_u64("id"sv);
    if (!namespace_id.has_value() || !local_id.has_value())
        return;
    Web::HTML::CrossProcessId navigable_id { .namespace_id = *namespace_id, .local_id = *local_id };
    auto navigable = page.hosted_navigable(navigable_id);
    if (!navigable.has_value())
        return;
    auto tab = tab_of(page.view());
    if (!tab.has_value())
        return;
    // The page's word is taken only for the document the browser knows the frame shows.
    auto url = URL::Parser::basic_parse(*url_string);
    if (!url.has_value() || url->serialize() != navigable->active_document_url().serialize())
        return;

    JsonObject details;
    details.set("tabId"sv, tab->id);
    details.set("frameId"sv, frame_id_of(*navigable));
    details.set("parentFrameId"sv, navigable->parent() ? frame_id_of(*navigable->parent()) : -1);
    details.set("url"sv, url->serialize());
    details.set("processId"sv, -1);
    details.set("frameType"sv, navigable->parent() ? "sub_frame"sv : "outermost_frame"sv);
    if (navigable->parent())
        details.set("parentDocumentId"sv, document_id_of(*navigable->parent()));

    auto send = [&](StringView name, JsonObject event_details) {
        event_details.set("timeStamp"sv, now_in_milliseconds());
        dispatch(name, [&](Extension const& extension) -> Optional<JsonArray> {
            if (!has_permission(extension, "webNavigation"sv))
                return {};
            if (WebExtensions::is_restricted_url(*url, runtime().registry().allows_file_access(extension.id()), extension.id().bytes_as_string_view()))
                return {};
            return arguments_of({ event_details });
        });
    };

    if (*event == "committed"sv) {
        // FIXME: Report navigations as they start, not as they commit.
        send("webNavigation.onBeforeNavigate"sv, details);
        auto committed = details;
        committed.set("documentId"sv, document_id_of(*navigable));
        committed.set("documentLifecycle"sv, "active"sv);
        committed.set("transitionType"sv, "link"sv);
        committed.set("transitionQualifiers"sv, JsonArray {});
        send("webNavigation.onCommitted"sv, move(committed));
        if (!navigable->parent())
            tabs_changed();
        return;
    }
    details.set("documentId"sv, document_id_of(*navigable));
    details.set("documentLifecycle"sv, "active"sv);
    if (*event == "dom_content_loaded"sv)
        send("webNavigation.onDOMContentLoaded"sv, move(details));
    else if (*event == "completed"sv)
        send("webNavigation.onCompleted"sv, move(details));
}

// tabs.sendMessage and tabs.connect: to the extension's content scripts in a tab, or one of its frames.
void TabsAPI::register_messaging_functions()
{
    auto receivers_in = [this](Context const& caller, Tab const& tab, JsonObject const& options, StringView event) {
        auto frame_id = options.get_integer<i64>("frameId"sv);
        auto document_id = options.get_string("documentId"sv);
        Vector<Context const*> receivers;
        runtime().for_each_context([&](Context const& context) {
            if (context.extension_id != caller.extension_id || context.type != "content_script"sv || context.id == caller.id || !context.has_listener(event))
                return;
            if (!context.page || !context.page->is_open() || &context.page->view() != tab.view)
                return;
            if (frame_id.has_value() || document_id.has_value()) {
                CanonicalNavigable* frame = nullptr;
                if (context.navigable_id.has_value() && tab.view->has_traversable())
                    frame = tab.view->traversable().find(*context.navigable_id).ptr();
                if (!frame || (frame_id.has_value() && frame_id_of(*frame) != *frame_id) || (document_id.has_value() && document_id_of(*frame) != *document_id))
                    return;
            }
            receivers.append(&context);
        });
        return receivers;
    };

    define("tabs.sendMessage"sv, {}, [this, receivers_in](Context const& context, Extension const&, JsonArray const& arguments, Reply reply) {
        auto tab_id = TRY_OR_REPLY(integer_argument(arguments, 0, "tabId"sv));
        auto tab = find_tab(tab_id);
        if (!tab.has_value())
            return reply(no_tab_error(tab_id));
        auto options = TRY_OR_REPLY(object_argument(arguments, 2, "options"sv));
        auto receivers = receivers_in(context, *tab, options, "runtime.onMessage"sv);
        if (receivers.is_empty())
            return reply(MUST(String::from_utf8(no_receiver_error)));
        runtime().deliver_message(context, move(receivers), argument(arguments, 1).value_or(JsonValue {}), move(reply));
    });

    define("tabs.connect"sv, {}, [this, receivers_in](Context const& context, Extension const&, JsonArray const& arguments, Reply reply) {
        reply(JsonValue {});
        if (arguments.size() < 3 || !arguments.at(2).is_string())
            return;
        auto info = arguments.at(1).is_object() ? arguments.at(1).as_object() : JsonObject {};
        auto name = info.get_string("name"sv).value_or({});
        Vector<Context const*> receivers;
        if (auto tab_id = arguments.at(0).get_integer<i64>(); tab_id.has_value()) {
            if (auto tab = find_tab(*tab_id); tab.has_value())
                receivers = receivers_in(context, *tab, info, "runtime.onConnect"sv);
        }
        runtime().open_channel(context, arguments.at(2).as_string(), move(receivers), name);
    });
}

// Events.
void TabsAPI::dispatch(StringView event, Function<Optional<JsonArray>(Extension const&)> const& arguments)
{
    Vector<Context const*> receivers;
    runtime().for_each_context([&](Context const& context) {
        if (context.type != "content_script"sv && context.has_listener(event))
            receivers.append(&context);
    });
    HashMap<String, Optional<JsonArray>> arguments_by_extension;
    for (auto const* receiver : receivers) {
        auto& extension_arguments = arguments_by_extension.ensure(receiver->extension_id, [&]() -> Optional<JsonArray> {
            auto const* extension = runtime().registry().find(receiver->extension_id);
            if (!extension)
                return {};
            return arguments(*extension);
        });
        if (!extension_arguments.has_value())
            continue;
        JsonObject message;
        message.set("type"sv, "event"sv);
        message.set("event"sv, event);
        message.set("args"sv, *extension_arguments);
        runtime().send(*receiver, message);
    }
}

void TabsAPI::watch_view(ViewImplementation& view)
{
    if (m_watched_views.contains(&view))
        return;
    ViewImplementation::NavigationListener listener;
    auto view_pointer = &view;
    listener.on_load_start = [this, view_pointer](URL::URL const&) {
        if (auto tab = tab_of(*view_pointer); tab.has_value())
            m_pending_urls.remove(tab->id);
        tabs_changed();
    };
    listener.on_load_finish = [this, view_pointer](URL::URL const&) {
        if (auto tab = tab_of(*view_pointer); tab.has_value())
            m_pending_urls.remove(tab->id);
        tabs_changed();
    };
    m_watched_views.set(&view, view.add_navigation_listener(move(listener)));
}

void TabsAPI::tabs_changed()
{
    if (!delegate()) {
        m_tab_states.clear();
        m_window_states.clear();
        m_focused_window = window_id_none;
        m_have_states = false;
        return;
    }

    auto windows = delegate()->windows();
    auto tabs = delegate()->tabs();
    HashMap<i64, TabState> tab_states;
    for (auto const& tab : tabs) {
        if (!tab.view)
            continue;
        watch_view(*tab.view);
        tab_states.set(tab.id, {
                                   .window_id = tab.window_id,
                                   .index = tab.index,
                                   .active = tab.active,
                                   .pinned = tab.pinned,
                                   .audible = tab.audible,
                                   .muted = tab.muted,
                                   .discarded = tab.discarded,
                                   .status = status_of(tab),
                                   .url = url_of(tab),
                                   .title = title_of(tab),
                                   .fav_icon_url = tab.fav_icon_url,
                                   .view = tab.view,
                               });
    }
    HashTable<i64> window_states;
    i64 focused_window = window_id_none;
    for (auto const& window : windows) {
        window_states.set(window.id);
        if (window.focused)
            focused_window = window.id;
    }

    auto old_tabs = move(m_tab_states);
    auto old_windows = move(m_window_states);
    auto old_focused = m_focused_window;
    bool had_states = m_have_states;
    m_tab_states = move(tab_states);
    m_window_states = move(window_states);
    m_focused_window = focused_window;
    m_have_states = true;
    // Views of tabs that are gone may be gone too.
    for (auto const& [id, state] : old_tabs) {
        if (!m_tab_states.contains(id) && !any_of(m_tab_states, [&](auto const& entry) { return entry.value.view == state.view; })) {
            m_watched_views.remove(state.view);
            m_pending_urls.remove(id);
        }
    }
    if (!had_states)
        return;

    for (auto const& window : windows) {
        if (!old_windows.contains(window.id))
            dispatch("windows.onCreated"sv, [&](Extension const& extension) { return Optional<JsonArray> { arguments_of({ window_json(window, extension, false) }) }; });
    }

    for (auto const& tab : tabs) {
        if (!tab.view)
            continue;
        auto const& state = m_tab_states.get(tab.id).value();
        auto old = old_tabs.get(tab.id);
        if (!old.has_value()) {
            dispatch("tabs.onCreated"sv, [&](Extension const& extension) { return Optional<JsonArray> { arguments_of({ tab_json(tab, extension) }) }; });
            if (tab.opener_tab_id.has_value()) {
                if (auto opener = old_tabs.get(*tab.opener_tab_id); opener.has_value()) {
                    JsonObject details;
                    details.set("sourceTabId"sv, *tab.opener_tab_id);
                    details.set("sourceProcessId"sv, -1);
                    details.set("sourceFrameId"sv, 0);
                    details.set("tabId"sv, tab.id);
                    details.set("url"sv, m_pending_urls.get(tab.id).value_or(state.url).serialize());
                    details.set("timeStamp"sv, now_in_milliseconds());
                    dispatch("webNavigation.onCreatedNavigationTarget"sv, [&](Extension const& extension) -> Optional<JsonArray> {
                        if (!has_permission(extension, "webNavigation"sv))
                            return {};
                        return arguments_of({ details });
                    });
                }
            }
            continue;
        }

        if (old->window_id != state.window_id) {
            JsonObject detach_info;
            detach_info.set("oldWindowId"sv, old->window_id);
            detach_info.set("oldPosition"sv, old->index);
            dispatch("tabs.onDetached"sv, [&](Extension const&) { return Optional<JsonArray> { arguments_of({ tab.id, detach_info }) }; });
            JsonObject attach_info;
            attach_info.set("newWindowId"sv, state.window_id);
            attach_info.set("newPosition"sv, state.index);
            dispatch("tabs.onAttached"sv, [&](Extension const&) { return Optional<JsonArray> { arguments_of({ tab.id, attach_info }) }; });
        }

        dispatch("tabs.onUpdated"sv, [&](Extension const& extension) -> Optional<JsonArray> {
            JsonObject change;
            if (old->status != state.status)
                change.set("status"sv, state.status);
            if (may_see(extension, state.url)) {
                if (old->url != state.url)
                    change.set("url"sv, state.url.serialize());
                if (old->title != state.title)
                    change.set("title"sv, state.title);
                if (old->fav_icon_url != state.fav_icon_url && state.fav_icon_url.has_value())
                    change.set("favIconUrl"sv, *state.fav_icon_url);
            }
            if (old->pinned != state.pinned)
                change.set("pinned"sv, state.pinned);
            if (old->audible != state.audible)
                change.set("audible"sv, state.audible);
            if (old->discarded != state.discarded)
                change.set("discarded"sv, state.discarded);
            if (old->muted != state.muted) {
                JsonObject muted_info;
                muted_info.set("muted"sv, state.muted);
                change.set("mutedInfo"sv, move(muted_info));
            }
            if (change.is_empty())
                return {};
            return arguments_of({ tab.id, move(change), tab_json(tab, extension) });
        });

        if (state.active && (!old->active || old->window_id != state.window_id)) {
            JsonObject info;
            info.set("tabId"sv, tab.id);
            info.set("windowId"sv, state.window_id);
            dispatch("tabs.onActivated"sv, [&](Extension const&) { return Optional<JsonArray> { arguments_of({ info }) }; });
            JsonObject highlight;
            highlight.set("windowId"sv, state.window_id);
            highlight.set("tabIds"sv, arguments_of({ tab.id }));
            dispatch("tabs.onHighlighted"sv, [&](Extension const&) { return Optional<JsonArray> { arguments_of({ highlight }) }; });
        }
    }
    // A new tab that's active is activated too.
    for (auto const& tab : tabs) {
        if (tab.view && tab.active && !old_tabs.contains(tab.id)) {
            JsonObject info;
            info.set("tabId"sv, tab.id);
            info.set("windowId"sv, tab.window_id);
            dispatch("tabs.onActivated"sv, [&](Extension const&) { return Optional<JsonArray> { arguments_of({ info }) }; });
        }
    }

    for (auto const& [id, old] : old_tabs) {
        if (m_tab_states.contains(id))
            continue;
        JsonObject info;
        info.set("windowId"sv, old.window_id);
        info.set("isWindowClosing"sv, !m_window_states.contains(old.window_id));
        dispatch("tabs.onRemoved"sv, [&](Extension const&) { return Optional<JsonArray> { arguments_of({ id, info }) }; });
    }

    for (auto id : old_windows) {
        if (!m_window_states.contains(id))
            dispatch("windows.onRemoved"sv, [&](Extension const&) { return Optional<JsonArray> { arguments_of({ id }) }; });
    }

    if (old_focused != focused_window)
        dispatch("windows.onFocusChanged"sv, [&](Extension const&) { return Optional<JsonArray> { arguments_of({ focused_window }) }; });
}

}
