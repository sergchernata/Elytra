/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// chrome.downloads: what extensions download goes through the browser's downloads, and extensions hear of downloads.

#include "ExtensionTestHarness.h"

#include <AK/IPv4Address.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/FileDownloader.h>

namespace {

constexpr auto extension_id = "kdmnkplhnbneeedommahachoibbaajeh"sv;

bool is_error(String const& value)
{
    return value.bytes_as_string_view().starts_with("error"sv);
}

// Answers every request with the same small file.
class TestServer {
public:
    TestServer()
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto socket = m_server->accept();
            if (socket.is_error())
                return;
            auto* client = socket.value().ptr();
            m_clients.append(socket.release_value());
            client->on_ready_to_read = [client] {
                Array<u8, 4096> buffer;
                auto read = client->read_some(buffer);
                if (read.is_error() || read.value().is_empty() || !StringView { read.value() }.contains("\r\n\r\n"sv))
                    return;
                (void)client->write_until_depleted("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 12\r\nConnection: close\r\n\r\nFrom the web"sv.bytes());
                client->close();
            };
        };
    }

    u16 port() const { return m_server->local_port().value(); }

private:
    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Core::TCPSocket>> m_clients;
};

ByteString contents_of(ByteString const& path)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return "(no file)";
    auto contents = MUST(file.value()->read_until_eof());
    return ByteString { contents.bytes() };
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return ExtensionTests::run_extension_tests(arguments, [](ExtensionTests::Harness& harness) {
        using namespace ExtensionTests;
        auto downloads_directory = ByteString::formatted("{}/Downloads", harness.directory);
        MUST(Core::Directory::create(downloads_directory, Core::Directory::CreateDirectories::Yes));
        MUST(Core::Environment::set("XDG_DOWNLOAD_DIR"sv, downloads_directory, Core::Environment::Overwrite::Yes));
        downloads_directory = MUST(FileSystem::real_path(downloads_directory));
        // A file outside the downloads directory, which nothing may replace.
        write_temporary_file(harness.directory, "escape.txt"sv, "Untouched"sv);
        // And a link out of it.
        MUST(Core::System::symlink(harness.directory, ByteString::formatted("{}/link", downloads_directory)));

        TestServer server;
        auto server_url = MUST(String::formatted("http://127.0.0.1:{}/file.txt", server.port()));

        MUST(harness.registry.load_unpacked(fixture("downloads"sv)));

        auto report = report_of(harness.view, url(MUST(String::formatted("chrome-extension://{}/page.html?step=download&server={}", extension_id, server_url))));
        for (auto key : { "data"sv, "attachment"sv, "subdirectory"sv, "blob"sv, "own_file"sv, "uniquified"sv, "overwritten"sv, "web"sv, "callback"sv })
            CHECK(report.get_integer<i64>(key).has_value());
        for (auto key : { "escaping"sv, "escaping_inside"sv, "through_link"sv, "absolute"sv, "backslash"sv, "empty_filename"sv, "hidden_folder"sv, "bad_filename"sv, "no_url"sv, "bad_url"sv, "javascript_url"sv, "file_url"sv, "other_extension"sv, "missing_file"sv, "bad_conflict_action"sv, "post"sv, "headers"sv, "bad_options"sv, "search_bad"sv, "pause_complete"sv, "resume_complete"sv, "pause_missing"sv, "open_without_permission"sv, "erase"sv, "remove_file_again"sv, "file_icon"sv }) {
            if (!is_error(string_in(report, key)))
                warnln("{} is \"{}\"", key, string_in(report, key));
            CHECK(is_error(string_in(report, key)));
        }

        auto path = [&](StringView name) { return MUST(String::formatted("{}/{}", downloads_directory, name)); };
        CHECK_EQ(string_in(report, "data_state"sv), "complete"sv);
        CHECK_EQ(string_in(report, "data_filename"sv), path("hello.txt"sv));
        CHECK_EQ(string_in(report, "data_mime"sv), "text/plain"sv);
        CHECK_EQ(string_in(report, "data_by"sv), extension_id);
        CHECK(report.get_bool("data_exists"sv).value_or(false));
        CHECK_EQ(report.get_integer<i64>("data_size"sv).value_or(-1), 5);
        CHECK_EQ(string_in(report, "attachment_filename"sv), path("export.json"sv));
        CHECK_EQ(string_in(report, "subdirectory_filename"sv), path("sub/dir/note.txt"sv));
        CHECK_EQ(string_in(report, "blob_filename"sv), path("blob.txt"sv));
        CHECK_EQ(string_in(report, "own_file_filename"sv), path("file.txt"sv));
        CHECK_EQ(string_in(report, "uniquified_filename"sv), path("hello (1).txt"sv));
        CHECK_EQ(string_in(report, "overwritten_filename"sv), path("hello.txt"sv));
        CHECK_EQ(string_in(report, "web_filename"sv), path("web.txt"sv));
        CHECK_EQ(string_in(report, "web_state"sv), "complete"sv);

        CHECK_EQ(contents_of(ByteString::formatted("{}/hello.txt", downloads_directory)), "Bye"sv);
        CHECK_EQ(contents_of(ByteString::formatted("{}/hello (1).txt", downloads_directory)), "Again"sv);
        CHECK_EQ(contents_of(ByteString::formatted("{}/export.json", downloads_directory)), R"({"a":1})"sv);
        CHECK_EQ(contents_of(ByteString::formatted("{}/sub/dir/note.txt", downloads_directory)), "In a folder"sv);
        CHECK_EQ(contents_of(ByteString::formatted("{}/blob.txt", downloads_directory)), "From a blob"sv);
        CHECK_EQ(contents_of(ByteString::formatted("{}/file.txt", downloads_directory)), "A file of the extension.\n"sv);
        CHECK_EQ(contents_of(ByteString::formatted("{}/web.txt", downloads_directory)), "From the web"sv);
        CHECK_EQ(contents_of(ByteString::formatted("{}/escape.txt", harness.directory)), "Untouched"sv);
        CHECK(!FileSystem::exists(ByteString::formatted("{}/cb.txt", downloads_directory)));

        CHECK(report.get_integer<i64>("search_all"sv).value_or(0) >= 9);
        CHECK_EQ(MUST(String::formatted("{}", report.get("search_query"sv).value_or(JsonValue {}).serialized())), MUST(String::formatted("\"{}\"", report.get_integer<i64>("attachment"sv).value_or(-1))));
        CHECK_EQ(report.get_integer<i64>("search_state"sv).value_or(-1), 2);
        CHECK_EQ(report.get_integer<i64>("search_missing"sv).value_or(-1), 0);
        CHECK_EQ(string_in(report, "cancel_complete"sv), "canceled"sv);
        CHECK_EQ(string_in(report, "show"sv), "shown"sv);
        CHECK_EQ(string_in(report, "remove_file"sv), "removed"sv);
        CHECK(!report.get_bool("callback_exists"sv).value_or(true));

        // The downloads are the browser's, which its downloads UI lists.
        auto& file_downloader = WebView::Application::the().file_downloader();
        CHECK(file_downloader.downloads_of_session(WebView::IsPrivate::No).size() >= 9);

        // A download the user started, the extension hears of too.
        auto user_download = file_downloader.download_file(WebView::IsPrivate::No, url(server_url), LexicalPath { ByteString::formatted("{}/by-the-user.txt", downloads_directory) });
        // And a private one, not.
        (void)file_downloader.download_file(WebView::IsPrivate::Yes, url(server_url), LexicalPath { ByteString::formatted("{}/private.txt", downloads_directory) });

        auto wait_for = [&](Function<bool()> condition) {
            auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(10);
            auto wake = Core::Timer::create_repeating(AK::Duration::from_milliseconds(50), [] { });
            wake->start();
            Core::EventLoop::current().spin_until([&] { return condition() || MonotonicTime::now() > deadline; });
            wake->stop();
        };
        wait_for([&] {
            auto download = file_downloader.download(user_download);
            return download.has_value() && download->status == WebView::FileDownloader::DownloadStatus::Completed;
        });

        report = report_of(harness.view, url(MUST(String::formatted("chrome-extension://{}/page.html?step=events&events=21", extension_id))));
        auto events = JsonParser::parse(string_in(report, "events"sv));
        CHECK(!events.is_error() && events.value().is_array());
        if (!events.is_error() && events.value().is_array()) {
            size_t created = 0;
            size_t completed = 0;
            bool heard_of_user_download = false;
            bool heard_of_private_download = false;
            bool heard_of_removed_file = false;
            for (auto const& event : events.value().as_array().values()) {
                if (!event.is_array() || event.as_array().size() != 2 || !event.as_array().at(1).is_object())
                    continue;
                auto const& name = event.as_array().at(0);
                auto const& details = event.as_array().at(1).as_object();
                auto filename = details.get_string("filename"sv).value_or({});
                if (name.is_string() && name.as_string() == "created"sv) {
                    ++created;
                    if (filename.ends_with_bytes("by-the-user.txt"sv)) {
                        heard_of_user_download = true;
                        CHECK(!details.has("byExtensionId"sv));
                    }
                    if (filename.ends_with_bytes("private.txt"sv))
                        heard_of_private_download = true;
                    CHECK_EQ(details.get_string("state"sv).value_or({}), "in_progress"sv);
                } else if (name.is_string() && name.as_string() == "changed"sv) {
                    auto state = details.get_object("state"sv);
                    if (state.has_value() && state->get_string("current"sv) == "complete"sv) {
                        ++completed;
                        CHECK_EQ(state->get_string("previous"sv).value_or({}), "in_progress"sv);
                    }
                    auto exists = details.get_object("exists"sv);
                    if (exists.has_value() && exists->get_bool("current"sv) == false)
                        heard_of_removed_file = true;
                }
            }
            if (created != 10u)
                warnln("Events: {}", string_in(report, "events"sv));
            CHECK_EQ(created, 10u);
            CHECK_EQ(completed, 10u);
            CHECK(heard_of_user_download);
            CHECK(!heard_of_private_download);
            CHECK(heard_of_removed_file);
        }

        // downloads.open needs its permission, and the user's gesture (as Chrome): an extension can't open files
        // whenever it likes.
        {
            auto directory = ByteString::formatted("{}/downloads-open", harness.directory);
            MUST(copy_recursively(directory, fixture("downloads"sv)));
            auto manifest_path = ByteString::formatted("{}/manifest.json", directory);
            auto manifest = MUST(MUST(Core::File::open(manifest_path, Core::File::OpenMode::Read))->read_until_eof());
            auto with_open = StringView { manifest }.replace("\"permissions\": [\"downloads\""sv, "\"permissions\": [\"downloads.open\", \"downloads\""sv, ReplaceMode::FirstOnly);
            CHECK(with_open != StringView { manifest });
            MUST(FileSystem::remove(manifest_path, FileSystem::RecursionMode::Disallowed));
            write_temporary_file(directory, "manifest.json"sv, with_open);
            MUST(harness.registry.load_unpacked(directory));
            auto open_url = url(MUST(String::formatted("chrome-extension://{}/page.html?step=open", extension_id)));

            size_t activations_used = 0;
            WebView::set_user_activation_for_testing([&] { ++activations_used; return false; });
            report = report_of(harness.view, open_url);
            CHECK_EQ(string_in(report, "open"sv), "error: User gesture required"sv);
            CHECK(activations_used >= 1);

            WebView::set_user_activation_for_testing([] { return true; });
            report = report_of(harness.view, open_url);
            CHECK_EQ(string_in(report, "open"sv), "opened"sv);
            CHECK(is_error(string_in(report, "open_missing"sv)));
            CHECK(is_error(string_in(report, "open_bad"sv)));
            WebView::set_user_activation_for_testing({});
        }

        MUST(Core::System::unlink(ByteString::formatted("{}/link", downloads_directory)));
    });
}
