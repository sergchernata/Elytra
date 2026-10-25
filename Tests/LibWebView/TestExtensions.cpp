/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Runs the extensions in Tests/LibWebExtensions/Fixtures/extensions in a real WebContent process. Each page reports
// what it saw as JSON in its title.

#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/Random.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

int s_failures = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            warnln("\033[31;1mFAIL\033[0m: {}:{}: CHECK({})", __FILE__, __LINE__, #condition); \
            ++s_failures;                                                                   \
        }                                                                                   \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                                         \
    do {                                                                                                                   \
        auto const& actual_value = (actual);                                                                               \
        auto const& expected_value = (expected);                                                                           \
        if (actual_value != expected_value) {                                                                              \
            warnln("\033[31;1mFAIL\033[0m: {}:{}: {} is \"{}\", expected \"{}\"", __FILE__, __LINE__, #actual, actual_value, expected_value); \
            ++s_failures;                                                                                                  \
        }                                                                                                                  \
    } while (false)

ByteString fixture(StringView name)
{
    auto source_directory = Core::Environment::get("LADYBIRD_SOURCE_DIR"sv);
    VERIFY(source_directory.has_value());
    return ByteString::formatted("{}/Tests/LibWebExtensions/Fixtures/extensions/{}", *source_directory, name);
}

// Loads the URL and waits for the page to put its JSON report in the title.
JsonObject report_of(WebView::HeadlessWebView& view, URL::URL const& url)
{
    Optional<String> report;
    view.on_title_change = [&](Utf16String const& title) {
        auto text = title.to_utf8();
        if (text.starts_with('{'))
            report = text;
    };
    view.load(url);

    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(15);
    Core::EventLoop::current().spin_until([&] { return report.has_value() || MonotonicTime::now() > deadline; });
    view.on_title_change = nullptr;

    if (!report.has_value()) {
        warnln("\033[31;1mFAIL\033[0m: no report from {}", url);
        ++s_failures;
        return {};
    }
    auto json = JsonParser::parse(*report);
    VERIFY(!json.is_error() && json.value().is_object());
    return json.value().as_object();
}

String string_in(JsonObject const& report, StringView key)
{
    return report.get_string(key).value_or("(missing)"_string);
}

URL::URL url(StringView string)
{
    return URL::Parser::basic_parse(string).release_value();
}

ByteString write_temporary_file(ByteString const& directory, StringView name, StringView contents)
{
    auto path = ByteString::formatted("{}/{}", directory, name);
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(contents.bytes()));
    return path;
}

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

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto directory = ByteString::formatted("{}/Ladybird-TestExtensions-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup = ScopeGuard([&] {
        MUST(FileSystem::remove(directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, directory, Core::Environment::Overwrite::Yes));

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    auto& registry = app->extension_registry();
    extension_pages_have_the_extension_as_their_origin(*view, registry);
    web_pages_reach_only_web_accessible_resources(*view, directory);
    links_out_of_the_extension_folder_are_not_followed(*view, registry, directory);
    extension_pages_get_the_chrome_api(*view, registry);
    extension_contexts_message_each_other(*view, registry);
    content_scripts_run_in_their_own_world(*view, registry, directory);
    content_scripts_stay_out_of_local_files_unless_allowed(*view, registry, directory);

    if (s_failures != 0) {
        warnln("{} checks failed", s_failures);
        return 1;
    }
    outln("All extension checks passed.");
    return 0;
}
