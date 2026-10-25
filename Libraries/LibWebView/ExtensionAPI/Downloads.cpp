/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Loader/DownloadFilename.h>
#include <LibWebView/Application.h>
#include <LibWebView/ExtensionAPI/Downloads.h>
#include <LibWebView/ExtensionAPI/Notifications.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/FileDownloader.h>
#include <time.h>

namespace WebView {

NonnullOwnPtr<ExtensionAPI> create_downloads_api(ExtensionRuntime& runtime)
{
    return make<DownloadsAPI>(runtime);
}

using Context = ExtensionRuntime::Context;
using Reply = ExtensionRuntime::Reply;
using Download = FileDownloader::Download;
using DownloadStatus = FileDownloader::DownloadStatus;

// What an extension downloads from its own files, data: or blob: URLs is read whole first.
static constexpr size_t maximum_in_memory_download_size = 256 * MiB;

// The browser's downloads, as extensions see them.
class DownloadsAPI::Observer final : public FileDownloaderObserver {
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit Observer(DownloadsAPI& api)
        : m_api(api)
    {
    }

    virtual void download_added(Download const& download) override { m_api.download_added(download); }
    virtual void download_updated(Download const& download) override { m_api.download_updated(download); }
    virtual void download_removed(u64 id, IsPrivate is_private) override { m_api.download_removed(id, is_private); }

private:
    DownloadsAPI& m_api;
};

// Chrome's IDs start at 1; FileDownloader's at 0.
static i64 chrome_id_of(u64 id) { return static_cast<i64>(id) + 1; }

static Optional<u64> download_id_in(JsonValue const& value)
{
    auto id = value.get_integer<i64>();
    if (!id.has_value() || *id < 1)
        return {};
    return static_cast<u64>(*id - 1);
}

static String iso_time(UnixDateTime time)
{
    auto seconds = static_cast<time_t>(time.seconds_since_epoch());
    struct tm parts {};
    gmtime_r(&seconds, &parts);
    return MUST(String::formatted("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec, time.milliseconds_since_epoch() % 1000));
}

DownloadsAPI::DownloadsAPI(ExtensionRuntime& extension_runtime)
    : ExtensionAPI(extension_runtime)
{
    // For the extension's pages and background, with the "downloads" permission.
    auto add = [this](StringView name, Function<void(Context const&, Extension const&, JsonArray const&, Reply)> handler) {
        runtime().register_function(name, [this, handler = move(handler)](Context const& context, JsonArray const& arguments, Reply reply) {
            if (context.type == "content_script"sv)
                return reply("Content scripts can't use chrome.downloads"_string);
            auto const* extension = runtime().registry().find(context.extension_id);
            if (!extension || !extension->manifest().has_permission("downloads"sv))
                return reply("chrome.downloads needs the \"downloads\" permission"_string);
            observe();
            handler(context, *extension, arguments, move(reply));
        });
    };

    // A download of the browser's, for functions given its ID.
    auto find_download = [](JsonArray const& arguments) -> ErrorOr<Download, String> {
        if (arguments.is_empty())
            return "Invalid downloadId."_string;
        auto id = download_id_in(arguments.at(0));
        if (!id.has_value())
            return "Invalid downloadId."_string;
        auto download = Application::the().file_downloader().download(*id);
        if (!download.has_value() || download->is_private == IsPrivate::Yes)
            return "Invalid downloadId."_string;
        return *download;
    };

    add("downloads.download"sv, [this](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
        if (arguments.is_empty() || !arguments.at(0).is_object())
            return reply("Expected download options"_string);
        auto result = start_download(context, extension, arguments.at(0).as_object());
        if (result.is_error())
            return reply(result.release_error());
        reply(JsonValue { chrome_id_of(result.value()) });
    });

    add("downloads.search"sv, [this](Context const&, Extension const& extension, JsonArray const& arguments, Reply reply) {
        if (arguments.size() != 1 || !arguments.at(0).is_object())
            return reply("Expected a query"_string);
        auto results = search(extension, arguments.at(0).as_object());
        if (results.is_error())
            return reply(results.release_error());
        reply(JsonValue { results.release_value() });
    });

    add("downloads.pause"sv, [find_download](Context const&, Extension const&, JsonArray const& arguments, Reply reply) {
        auto download = find_download(arguments);
        if (download.is_error())
            return reply(download.release_error());
        if (download.value().status != DownloadStatus::InProgress)
            return reply("Download must be in progress"_string);
        Application::the().file_downloader().pause_download(download.value().id);
        reply(JsonValue {});
    });

    add("downloads.resume"sv, [find_download](Context const&, Extension const&, JsonArray const& arguments, Reply reply) {
        auto download = find_download(arguments);
        if (download.is_error())
            return reply(download.release_error());
        if (download.value().status != DownloadStatus::Paused || !download.value().can_resume)
            return reply("Download must be resumable"_string);
        Application::the().file_downloader().resume_download(download.value().id);
        reply(JsonValue {});
    });

    add("downloads.cancel"sv, [find_download](Context const&, Extension const&, JsonArray const& arguments, Reply reply) {
        auto download = find_download(arguments);
        if (download.is_error())
            return reply(download.release_error());
        // Cancelling a finished download does nothing, as in Chrome.
        if (FileDownloader::status_is_active(download.value().status))
            Application::the().file_downloader().cancel_download(download.value().id);
        reply(JsonValue {});
    });

    // Opening a file runs it with the app it's for: only when the user just did something in the extension's page, as
    // in Chrome.
    add("downloads.open"sv, [this, find_download](Context const& context, Extension const& extension, JsonArray const& arguments, Reply reply) {
        if (!extension.manifest().has_permission("downloads.open"sv))
            return reply("The \"downloads.open\" permission is required"_string);
        auto download = find_download(arguments);
        if (download.is_error())
            return reply(download.release_error());
        if (download.value().status != DownloadStatus::Completed)
            return reply("Download must be complete"_string);
        if (!runtime().consume_user_activation(context))
            return reply("User gesture required"_string);
        Application::the().open_download(download.value());
        reply(JsonValue {});
    });

    add("downloads.show"sv, [find_download](Context const&, Extension const&, JsonArray const& arguments, Reply reply) {
        auto download = find_download(arguments);
        if (download.is_error())
            return reply(download.release_error());
        Application::the().show_download_in_folder(download.value());
        reply(JsonValue { true });
    });

    add("downloads.removeFile"sv, [this, find_download](Context const&, Extension const&, JsonArray const& arguments, Reply reply) {
        auto download = find_download(arguments);
        if (download.is_error())
            return reply(download.release_error());
        if (download.value().status != DownloadStatus::Completed)
            return reply("Download must be complete"_string);
        auto const& path = download.value().destination.string();
        if (!FileSystem::exists(path))
            return reply("Download file already deleted"_string);
        if (auto result = FileSystem::remove(path, FileSystem::RecursionMode::Disallowed); result.is_error())
            return reply(MUST(String::formatted("Couldn't remove the file: {}", result.error())));
        reply(JsonValue {});
        download_updated(download.value());
    });

    // What the browser's downloads can't do.
    for (auto name : { "downloads.erase"sv, "downloads.showDefaultFolder"sv, "downloads.acceptDanger"sv, "downloads.setUiOptions"sv, "downloads.getFileIcon"sv }) {
        add(name, [name](Context const&, Extension const&, JsonArray const&, Reply reply) {
            reply(MUST(String::formatted("{} isn't supported by this browser", name)));
        });
    }
}

DownloadsAPI::~DownloadsAPI() = default;

void DownloadsAPI::observe()
{
    if (!m_observer)
        m_observer = make<Observer>(*this);
}

void DownloadsAPI::extensions_changed()
{
    observe();
}

void DownloadsAPI::shut_down()
{
    m_observer = nullptr;
}

// A path below the downloads directory, as an extension names it: relative, without "." or "..".
static ErrorOr<Vector<ByteString>, String> filename_components(StringView filename)
{
    if (filename.is_empty() || filename.starts_with('/') || filename.contains('\\') || filename.contains(':'))
        return "Invalid filename"_string;
    Vector<ByteString> components;
    for (auto component : filename.split_view('/', SplitBehavior::KeepEmpty)) {
        if (component.is_empty() || component == "."sv || component == ".."sv || component.length() > 255)
            return "Invalid filename"_string;
        if (any_of(component.bytes(), [](u8 byte) { return byte < 0x20 || byte == 0x7f; }))
            return "Invalid filename"_string;
        if (component.ends_with('.') || component.ends_with(' ') || component.starts_with(' '))
            return "Invalid filename"_string;
        components.append(component);
    }
    if (components.size() > 32)
        return "Invalid filename"_string;
    return components;
}

ErrorOr<u64, String> DownloadsAPI::start_download(Context const& context, Extension const& extension, JsonObject const& options)
{
    auto url_value = options.get("url"sv);
    if (!url_value.has_value() || !url_value->is_string())
        return "The url is required"_string;
    auto url = URL::Parser::basic_parse(url_value->as_string(), context.url);
    if (!url.has_value())
        return "Invalid URL"_string;

    auto scheme = url->scheme();
    bool is_web = scheme == "http"sv || scheme == "https"sv;
    bool is_own = scheme == "data"sv || scheme == "blob"sv || scheme == "chrome-extension"sv;
    if (!is_web && !is_own)
        return "Invalid URL"_string;

    // Requests other than a plain GET aren't something the browser's downloads can make.
    if (auto method = options.get("method"sv); method.has_value() && !method->is_null()) {
        if (!method->is_string() || !first_is_one_of(method->as_string(), "GET"sv, "POST"sv))
            return "Invalid method"_string;
        if (method->as_string() != "GET"sv)
            return "Only GET downloads are supported by this browser"_string;
    }
    if (auto headers = options.get("headers"sv); headers.has_value() && !headers->is_null()) {
        if (!headers->is_array())
            return "Invalid headers"_string;
        if (!headers->as_array().is_empty())
            return "Download request headers aren't supported by this browser"_string;
    }
    if (auto body = options.get("body"sv); body.has_value() && !body->is_null())
        return "Download request bodies aren't supported by this browser"_string;

    auto conflict_action = "uniquify"_string;
    if (auto action = options.get("conflictAction"sv); action.has_value() && !action->is_null()) {
        if (!action->is_string() || !first_is_one_of(action->as_string(), "uniquify"sv, "overwrite"sv, "prompt"sv))
            return "Invalid conflictAction"_string;
        conflict_action = action->as_string();
    }
    bool save_as = false;
    if (auto value = options.get("saveAs"sv); value.has_value() && !value->is_null()) {
        if (!value->is_bool())
            return "saveAs must be a boolean"_string;
        save_as = value->as_bool();
    }

    // Where it goes: the extension's relative path in the downloads directory, or the URL's name.
    Vector<ByteString> components;
    if (auto filename = options.get("filename"sv); filename.has_value() && !filename->is_null()) {
        if (!filename->is_string())
            return "Invalid filename"_string;
        components = TRY(filename_components(filename->as_string()));
    } else {
        ByteString name;
        if (is_web || scheme == "chrome-extension"sv) {
            auto segments = url->path_segments();
            if (!segments.is_empty())
                name = URL::percent_decode(segments.last());
        }
        components.append(Web::sanitize_suggested_download_filename(name));
    }

    // What the extension's own URLs hold, read now.
    Optional<ExtensionResource> resource;
    if (is_own) {
        auto read = read_extension_resource(runtime(), context, *url, maximum_in_memory_download_size);
        if (read.is_error())
            return read.release_error();
        resource = read.release_value();
    }

    auto downloads_directory = Core::StandardPaths::downloads_directory();
    auto real_downloads_directory = FileSystem::real_path(downloads_directory);
    if (real_downloads_directory.is_error())
        return "There's no downloads directory"_string;

    auto file = Web::sanitize_suggested_download_filename(components.take_last());
    auto may_replace = FileDownloader::MayReplaceDestination::No;
    LexicalPath destination { "/"sv };
    if (save_as || conflict_action == "prompt"sv) {
        auto chosen = Application::the().path_for_downloaded_file(file);
        if (chosen.is_error())
            return "Download canceled"_string;
        destination = chosen.release_value();
        if (FileSystem::exists(destination.string()))
            may_replace = FileDownloader::MayReplaceDestination::Yes;
    } else {
        // The folders, one by one, never out of the downloads directory (through a link).
        auto is_inside = [&](ByteString const& path) {
            return path == real_downloads_directory.value() || path.starts_with(ByteString::formatted("{}/", real_downloads_directory.value()));
        };
        auto real_directory = real_downloads_directory.value();
        for (auto const& component : components) {
            auto directory = LexicalPath::join(real_directory, component).string();
            if (!FileSystem::exists(directory)) {
                if (auto created = Core::System::mkdir(directory, 0755); created.is_error())
                    return "Couldn't make the download's folder"_string;
            }
            auto real_path = FileSystem::real_path(directory);
            if (real_path.is_error() || !is_inside(real_path.value()) || !FileSystem::is_directory(real_path.value()))
                return "Invalid filename"_string;
            real_directory = real_path.release_value();
        }

        destination = LexicalPath::join(real_directory, file);
        if (conflict_action == "overwrite"sv) {
            if (FileSystem::is_directory(destination.string()))
                return "Invalid filename"_string;
            may_replace = FileDownloader::MayReplaceDestination::Yes;
        } else {
            auto is_available = [](LexicalPath const& path) {
                if (FileSystem::exists(path.string()))
                    return false;
                for (auto const& download : Application::the().file_downloader().downloads()) {
                    if (FileDownloader::status_is_active(download.status) && download.destination.string() == path.string())
                        return false;
                }
                return true;
            };
            for (u64 index = 1; !is_available(destination); ++index)
                destination = LexicalPath::join(real_directory, Web::numbered_download_filename(file, index));
        }
    }

    auto& file_downloader = Application::the().file_downloader();
    m_starting_for = extension.id();
    u64 id = 0;
    if (resource.has_value()) {
        m_starting_mime_type = resource->mime_type;
        id = file_downloader.start_download(IsPrivate::No, *url, destination, resource->bytes.size(), may_replace);
        m_starting_for.clear();
        m_starting_mime_type.clear();
        file_downloader.append_download_data(id, resource->bytes);
        file_downloader.finish_download(id);
    } else {
        id = file_downloader.download_file(IsPrivate::No, *url, destination, may_replace);
        m_starting_for.clear();
    }

    auto download = file_downloader.download(id);
    if (!download.has_value())
        return "The download couldn't start"_string;
    if (download->status == DownloadStatus::Failed)
        return download->error.value_or("The download couldn't start"_string);
    return id;
}

JsonObject DownloadsAPI::item_json(Download const& download) const
{
    JsonObject item;
    item.set("id"sv, chrome_id_of(download.id));
    item.set("url"sv, download.url.serialize());
    item.set("finalUrl"sv, download.url.serialize());
    item.set("referrer"sv, ""sv);
    item.set("filename"sv, MUST(String::from_byte_string(download.destination.string())));
    item.set("incognito"sv, false);
    item.set("danger"sv, "safe"sv);
    item.set("mime"sv, m_mime_types.get(download.id).value_or({}));
    item.set("startTime"sv, iso_time(download.created_time));
    if (auto end = m_end_times.get(download.id); end.has_value())
        item.set("endTime"sv, iso_time(*end));

    switch (download.status) {
    case DownloadStatus::InProgress:
    case DownloadStatus::Paused:
        item.set("state"sv, "in_progress"sv);
        break;
    case DownloadStatus::Completed:
        item.set("state"sv, "complete"sv);
        break;
    case DownloadStatus::Canceled:
        item.set("state"sv, "interrupted"sv);
        item.set("error"sv, "USER_CANCELED"sv);
        break;
    case DownloadStatus::Failed:
        item.set("state"sv, "interrupted"sv);
        item.set("error"sv, "NETWORK_FAILED"sv);
        break;
    }
    item.set("paused"sv, download.status == DownloadStatus::Paused);
    item.set("canResume"sv, download.status == DownloadStatus::Paused && download.can_resume);
    item.set("bytesReceived"sv, download.downloaded_size);
    item.set("totalBytes"sv, download.total_size.has_value() ? static_cast<i64>(*download.total_size) : -1);
    item.set("fileSize"sv, download.status == DownloadStatus::Completed ? static_cast<i64>(download.downloaded_size) : (download.total_size.has_value() ? static_cast<i64>(*download.total_size) : -1));
    bool exists = download.status == DownloadStatus::Completed ? FileSystem::exists(download.destination.string()) : FileDownloader::status_is_active(download.status);
    item.set("exists"sv, exists);
    if (auto by = m_started_by.get(download.id); by.has_value()) {
        item.set("byExtensionId"sv, *by);
        if (auto const* extension = runtime().registry().find(*by))
            item.set("byExtensionName"sv, extension->messages().substitute_manifest_messages(extension->manifest().name));
    }
    if (auto estimate = download.estimated_time_remaining(); estimate.has_value() && download.status == DownloadStatus::InProgress)
        item.set("estimatedEndTime"sv, iso_time(UnixDateTime::now() + *estimate));
    return item;
}

ErrorOr<JsonArray, String> DownloadsAPI::search(Extension const&, JsonObject const& query) const
{
    Optional<size_t> limit;
    if (auto value = query.get("limit"sv); value.has_value() && !value->is_null()) {
        auto number = value->get_integer<i64>();
        if (!number.has_value() || *number < 0)
            return "Invalid limit"_string;
        if (*number > 0)
            limit = static_cast<size_t>(*number);
    }
    Vector<String> terms;
    if (auto value = query.get("query"sv); value.has_value() && !value->is_null()) {
        if (!value->is_array())
            return "query must be a list of strings"_string;
        for (auto const& term : value->as_array().values()) {
            if (!term.is_string())
                return "query must be a list of strings"_string;
            terms.append(term.as_string());
        }
    }
    bool newest_first = false;
    if (auto value = query.get("orderBy"sv); value.has_value() && !value->is_null()) {
        if (!value->is_array())
            return "orderBy must be a list of strings"_string;
        for (auto const& key : value->as_array().values()) {
            if (!key.is_string())
                return "orderBy must be a list of strings"_string;
        }
        if (!value->as_array().is_empty())
            newest_first = value->as_array().at(0).as_string() == "-startTime"sv;
    }

    // The properties a query can name, which must be the same.
    static constexpr Array exact_keys { "id"sv, "url"sv, "finalUrl"sv, "filename"sv, "mime"sv, "state"sv, "paused"sv, "danger"sv, "exists"sv, "error"sv, "canResume"sv, "incognito"sv, "totalBytes"sv, "fileSize"sv, "bytesReceived"sv };

    JsonArray results;
    auto downloads = Application::the().file_downloader().downloads_of_session(IsPrivate::No);
    if (newest_first)
        downloads.reverse();
    for (auto const& download : downloads) {
        auto item = item_json(download);
        bool matches = true;
        for (auto key : exact_keys) {
            auto wanted = query.get(key);
            if (!wanted.has_value() || wanted->is_null())
                continue;
            auto actual = item.get(key);
            if (!actual.has_value() || actual->serialized() != wanted->serialized())
                matches = false;
        }
        auto haystack = MUST(String::formatted("{} {}", item.get_string("url"sv).value_or({}), item.get_string("filename"sv).value_or({}))).to_ascii_lowercase();
        for (auto const& term : terms) {
            auto view = term.bytes_as_string_view();
            bool excluded = view.starts_with('-');
            if (excluded)
                view = view.substring_view(1);
            auto lowercase = ByteString(view).to_lowercase();
            if (haystack.bytes_as_string_view().contains(lowercase.view()) == excluded)
                matches = false;
        }
        if (!matches)
            continue;
        results.must_append(move(item));
        if (limit.has_value() && results.size() >= *limit)
            break;
    }
    return results;
}

void DownloadsAPI::dispatch(StringView event, JsonValue const& argument)
{
    runtime().registry().for_each_extension([&](Extension const& extension) {
        if (!extension.manifest().has_permission("downloads"sv))
            return;
        // A background still starting hears of it once it can.
        if (runtime().background_is_starting(extension.id())) {
            runtime().wait_for_background(extension.id(), [this, extension_id = extension.id(), event = MUST(String::from_utf8(event)), argument] {
                runtime().dispatch_event(extension_id, event, JsonArray { Vector<JsonValue> { argument } });
            });
            return;
        }
        runtime().dispatch_event(extension.id(), event, JsonArray { Vector<JsonValue> { argument } });
    });
}

void DownloadsAPI::download_added(Download const& download)
{
    if (download.is_private == IsPrivate::Yes)
        return;
    if (m_starting_for.has_value())
        m_started_by.set(download.id, *m_starting_for);
    if (m_starting_mime_type.has_value() && !m_starting_mime_type->is_empty())
        m_mime_types.set(download.id, *m_starting_mime_type);
    auto item = item_json(download);
    m_last_seen.set(download.id, item);
    dispatch("downloads.onCreated"sv, item);
}

void DownloadsAPI::download_updated(Download const& download)
{
    if (download.is_private == IsPrivate::Yes)
        return;
    if (!FileDownloader::status_is_active(download.status) && !m_end_times.contains(download.id))
        m_end_times.set(download.id, UnixDateTime::now());

    // onChanged tells only of what changed.
    static constexpr Array delta_keys { "url"sv, "finalUrl"sv, "filename"sv, "danger"sv, "mime"sv, "startTime"sv, "endTime"sv, "state"sv, "canResume"sv, "paused"sv, "error"sv, "totalBytes"sv, "fileSize"sv, "exists"sv };
    auto item = item_json(download);
    auto previous = m_last_seen.get(download.id).value_or({});
    JsonObject delta;
    delta.set("id"sv, chrome_id_of(download.id));
    bool changed = false;
    for (auto key : delta_keys) {
        auto before = previous.get(key);
        auto after = item.get(key);
        if (before.has_value() == after.has_value() && (!before.has_value() || before->serialized() == after->serialized()))
            continue;
        JsonObject change;
        if (before.has_value())
            change.set("previous"sv, *before);
        if (after.has_value())
            change.set("current"sv, *after);
        delta.set(key, move(change));
        changed = true;
    }
    m_last_seen.set(download.id, item);
    if (changed)
        dispatch("downloads.onChanged"sv, delta);
}

void DownloadsAPI::download_removed(u64 id, IsPrivate is_private)
{
    if (is_private == IsPrivate::Yes)
        return;
    m_last_seen.remove(id);
    m_started_by.remove(id);
    m_mime_types.remove(id);
    m_end_times.remove(id);
    dispatch("downloads.onErased"sv, JsonValue { chrome_id_of(id) });
}

}
