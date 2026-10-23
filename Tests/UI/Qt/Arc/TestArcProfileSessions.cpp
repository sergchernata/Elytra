/*
 * Arc fork: profiles. Each Arc profile has its own persistent browsing session, so its cookies and site data are
 * kept apart from the other profiles' while all of them share one window. These tests drive real WebContent and
 * RequestServer processes headlessly, like upstream's TestRequestServerClientSessions.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Time.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <UI/Qt/Arc/ProfileSessions.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        browser_options.allow_popups = WebView::AllowPopups::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

int s_failures = 0;

void expect(bool condition, StringView what)
{
    if (condition) {
        outln("PASS: {}", what);
        return;
    }
    warnln("FAIL: {}", what);
    ++s_failures;
}

URL::URL const& example_url()
{
    static auto url = URL::Parser::basic_parse("https://example.com/"sv).release_value();
    return url;
}

Optional<String> cookie_value(WebView::BrowsingSession& session, StringView name)
{
    for (auto const& cookie : session.cookie_jar->get_all_cookies_webdriver(example_url())) {
        if (cookie.name == name)
            return cookie.value;
    }
    return {};
}

void set_cookie(WebView::BrowsingSession& session, StringView name, StringView value)
{
    session.cookie_jar->set_cookie(example_url(), HTTP::Cookie::ParsedCookie { .name = MUST(String::from_utf8(name)), .value = MUST(String::from_utf8(value)) }, HTTP::Cookie::Source::Http, {});
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_directory = ByteString::formatted("{}/Ladybird-TestArcProfileSessions-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup = ScopeGuard([&] {
        MUST(FileSystem::remove(test_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_directory, Core::Environment::Overwrite::Yes));

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto& default_session = WebView::Application::default_session();

    // Profile folder names: never outside ArcProfiles/, and never shared by two profiles (macOS folders ignore case).
    {
        using Ladybird::Arc::profile_directory_name;
        expect(profile_directory_name("Work") != profile_directory_name("work"), "profiles differing only in case get different folders"sv);
        expect(profile_directory_name("a/b") != profile_directory_name("a_b"), "names that sanitize alike get different folders"sv);
        expect(profile_directory_name("Work").toLower() != profile_directory_name("work").toLower(), "even on a case-insensitive disk"sv);
        bool contained = true;
        for (auto const* name : { "../../evil", "a/b", "..", ".", "", " ", "Profile 1", "x\\y", "\xF0\x9F\x9A\x80" }) {
            auto folder = profile_directory_name(QString::fromUtf8(name));
            contained &= !folder.isEmpty() && !folder.contains('/') && !folder.contains('\\') && folder != "." && folder != ".." && !folder.startsWith('.');
        }
        expect(contained, "and folder names stay inside ArcProfiles/"sv);
        expect(profile_directory_name("Profile 1") == profile_directory_name("Profile 1"), "the same profile always gets the same folder"sv);
    }
    auto work_directory = LexicalPath::join(test_directory, "work"sv).string();
    auto side_directory = LexicalPath::join(test_directory, "side"sv).string();

    // A profile's session keeps site data apart and persistent; the browser's own records stay shared.
    {
        auto work = TRY(WebView::BrowsingSession::create_persistent(work_directory, default_session, WebView::BrowsingSession::Persist::Yes));
        auto side = TRY(WebView::BrowsingSession::create_persistent(side_directory, default_session, WebView::BrowsingSession::Persist::Yes));
        expect(work->is_private() == WebView::IsPrivate::No, "a profile session is not private"sv);
        expect(work.ptr() != &default_session && work.ptr() != side.ptr(), "each profile has its own session"sv);
        expect(work->cookie_jar.ptr() != default_session.cookie_jar.ptr(), "a profile has its own cookie jar"sv);
        expect(work->storage_jar.ptr() != default_session.storage_jar.ptr(), "a profile has its own site storage"sv);
        expect(work->hsts_store.ptr() != default_session.hsts_store.ptr(), "a profile has its own HSTS policies"sv);
        expect(work->blob_url_store.ptr() != default_session.blob_url_store.ptr(), "a profile has its own blob URLs"sv);
        expect(&work->records() == &default_session && &default_session.records() == &default_session, "history and tab restore are shared with the default session"sv);

        set_cookie(*work, "who"sv, "work"sv);
        set_cookie(default_session, "who"sv, "default"sv);
        expect(cookie_value(*work, "who"sv) == "work"sv, "a profile reads its own cookie"sv);
        expect(cookie_value(default_session, "who"sv) == "default"sv, "the default session keeps its own cookie"sv);
        expect(!cookie_value(*side, "who"sv).has_value(), "another profile sees neither"sv);
    }
    {
        auto work = TRY(WebView::BrowsingSession::create_persistent(work_directory, default_session, WebView::BrowsingSession::Persist::Yes));
        expect(cookie_value(*work, "who"sv) == "work"sv, "a profile's cookies survive a restart"sv);
    }

    // A profile that mustn't persist (SQL disabled, or its database couldn't be opened) still has its own jar, in
    // memory, and writes nothing.
    {
        auto memory_directory = LexicalPath::join(test_directory, "memory"sv).string();
        auto memory = TRY(WebView::BrowsingSession::create_persistent(memory_directory, default_session, WebView::BrowsingSession::Persist::No));
        set_cookie(*memory, "who"sv, "memory"sv);
        expect(cookie_value(*memory, "who"sv) == "memory"sv && cookie_value(default_session, "who"sv) == "default"sv, "a profile kept in memory has its own cookies"sv);
        expect(!FileSystem::exists(memory_directory), "and writes nothing to disk"sv);
    }

    auto work = TRY(WebView::BrowsingSession::create_persistent(work_directory, default_session, WebView::BrowsingSession::Persist::Yes));
    auto theme = TRY(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));

    // A view created while a profile is chosen for new views belongs to it, down to its processes.
    auto default_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    WebView::Application::set_session_for_new_views(work);
    auto work_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    WebView::Application::set_session_for_new_views({});
    auto later_view = WebView::HeadlessWebView::create(theme, { 800, 600 });

    expect(&work_view->session() == work.ptr(), "a profile's view uses the profile's session"sv);
    expect(&work_view->page().client().session() == work.ptr(), "its WebContent process belongs to the profile"sv);
    expect(&work_view->page().client() != &default_view->page().client(), "it never shares a process with the default session"sv);
    expect(&default_view->session() == &default_session && &default_view->page().client().session() == &default_session, "views outside a profile use the default session"sv);
    expect(&later_view->session() == &default_session, "choosing no profile goes back to the default session"sv);

    auto work_client_id = work_view->page().client().request_server_site_bindings().client_id();
    expect(work_client_id.has_value() && app->session_for_request_server_client(*work_client_id) == work, "its HTTP requests use the profile's cookies"sv);

    // Pages a profile's view opens stay in the profile, whether they share its process or get their own.
    Vector<WebView::BrowsingSession const*> popup_sessions;
    auto open_popup = move(work_view->on_new_web_view);
    work_view->on_new_web_view = [&](auto activate_tab, auto hints, WebView::CanonicalTraversable& traversable) {
        auto handle = open_popup(activate_tab, hints, traversable);
        auto popup = WebView::ViewImplementation::find_view_by_handle(handle);
        popup_sessions.append(popup.has_value() ? &popup->page().client().session() : nullptr);
        if (popup.has_value() && &popup->session() != &popup->page().client().session())
            popup_sessions.append(nullptr);
        return handle;
    };
    work_view->run_javascript("window.open('about:blank'); window.open('about:blank', '_blank', 'noopener');"_string);
    Core::EventLoop::current().spin_until([&] { return popup_sessions.size() >= 2; });
    expect(popup_sessions.size() == 2 && popup_sessions[0] == work.ptr() && popup_sessions[1] == work.ptr(), "popups of a profile's page stay in the profile"sv);

    // A worker started by a profile's page makes its requests with the profile's cookies: its RequestServer client
    // is the profile's, not the default session's.
    WebView::Application::set_session_for_new_views(work);
    auto worker_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    WebView::Application::set_session_for_new_views({});
    size_t worker_view_loads = 0;
    worker_view->on_load_finish = [&](auto const&) { ++worker_view_loads; };
    worker_view->load(URL::Parser::basic_parse("data:text/html,<title>worker owner</title>"sv).release_value());
    Core::EventLoop::current().spin_until([&] { return worker_view_loads > 0 && worker_view->url().scheme() == "data"sv; });

    auto work_clients = app->request_server_client_ids_for_testing(*work).size();
    auto default_clients = app->request_server_client_ids_for_testing(default_session).size();
    worker_view->run_javascript("window.w = new Worker('data:text/javascript,setInterval(() => {}, 1000)');"_string);
    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(20);
    Core::EventLoop::current().spin_until([&] {
        return MonotonicTime::now() > deadline
            || app->request_server_client_ids_for_testing(*work).size() > work_clients
            || app->request_server_client_ids_for_testing(default_session).size() > default_clients;
    });
    expect(app->request_server_client_ids_for_testing(*work).size() > work_clients
            && app->request_server_client_ids_for_testing(default_session).size() == default_clients,
        "a profile page's workers use the profile's cookies"sv);

    // Clearing site data clears every profile's, not just the default session's.
    set_cookie(*work, "who"sv, "work"sv);
    set_cookie(default_session, "who"sv, "default"sv);
    WebView::Application::ClearBrowsingDataOptions clear_options;
    clear_options.delete_site_data = WebView::Application::ClearBrowsingDataOptions::Delete::Yes;
    (void)app->clear_browsing_data(clear_options);
    expect(!cookie_value(default_session, "who"sv).has_value() && !cookie_value(*work, "who"sv).has_value(), "clearing site data clears every profile's"sv);

    if (s_failures > 0) {
        warnln("{} profile session check(s) failed", s_failures);
        return 1;
    }
    outln("PASS: profiles keep their cookies and site data apart");
    return 0;
}
