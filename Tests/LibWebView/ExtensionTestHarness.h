/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// What the extension tests share: they run the extensions in Tests/LibWebExtensions/Fixtures/extensions in real
// WebContent processes, and each page reports what it saw as JSON in its title.

#pragma once

#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/Random.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

namespace ExtensionTests {

class TestApplication final : public WebView::Application {
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

inline int s_failures = 0;

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

inline ByteString fixture(StringView name)
{
    auto source_directory = Core::Environment::get("LADYBIRD_SOURCE_DIR"sv);
    VERIFY(source_directory.has_value());
    return ByteString::formatted("{}/Tests/LibWebExtensions/Fixtures/extensions/{}", *source_directory, name);
}

// Loads the URL and waits for the page to put its JSON report in the title.
inline JsonObject report_of(WebView::HeadlessWebView& view, URL::URL const& url)
{
    Optional<String> report;
    view.on_title_change = [&](Utf16String const& title) {
        auto text = title.to_utf8();
        if (text.starts_with('{'))
            report = text;
    };
    view.load(url);

    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(15);
    // Wakes the loop, so the deadline passes even when nothing else happens.
    auto wake = Core::Timer::create_repeating(AK::Duration::from_milliseconds(100), [] { });
    wake->start();
    Core::EventLoop::current().spin_until([&] { return report.has_value() || MonotonicTime::now() > deadline; });
    wake->stop();
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

inline String string_in(JsonObject const& report, StringView key)
{
    return report.get_string(key).value_or("(missing)"_string);
}

inline URL::URL url(StringView string)
{
    return URL::Parser::basic_parse(string).release_value();
}

// A copy of a folder, and everything in it, (or of a file) at a path where nothing is yet.
inline ErrorOr<void> copy_recursively(ByteString const& destination, ByteString const& source)
{
    if (!FileSystem::is_directory(source)) {
        auto file = TRY(Core::File::open(source, Core::File::OpenMode::Read));
        auto source_stat = TRY(Core::System::fstat(file->fd()));
        return FileSystem::copy_file(destination, source, source_stat, *file);
    }
    TRY(Core::System::mkdir(destination, 0755));
    Core::DirIterator iterator(source, Core::DirIterator::SkipParentAndBaseDir);
    if (iterator.has_error())
        return iterator.error();
    while (iterator.has_next()) {
        auto name = iterator.next_path();
        TRY(copy_recursively(ByteString::formatted("{}/{}", destination, name), ByteString::formatted("{}/{}", source, name)));
    }
    return {};
}

inline ByteString write_temporary_file(ByteString const& directory, StringView name, StringView contents)
{
    auto path = ByteString::formatted("{}/{}", directory, name);
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(contents.bytes()));
    return path;
}

struct Harness {
    WebView::HeadlessWebView& view;
    WebView::ExtensionRegistry& registry;
    // A temporary directory, removed afterwards.
    ByteString directory;
};

// Starts the browser and a view, runs the tests, and reports.
inline ErrorOr<int> run_extension_tests(Main::Arguments arguments, Function<void(Harness&)> tests)
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

    Harness harness { *view, app->extension_registry(), directory };
    tests(harness);

    if (s_failures != 0) {
        warnln("{} checks failed", s_failures);
        return 1;
    }
    outln("All extension checks passed.");
    return 0;
}

}
