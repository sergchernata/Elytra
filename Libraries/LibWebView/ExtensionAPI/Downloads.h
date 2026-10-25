/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/FileDownloader.h>

namespace WebView {

class Extension;

// chrome.downloads, on the browser's own downloads (FileDownloader): what extensions download appears in the downloads
// UI, and extensions hear of the user's (non-private) downloads too, as in Chrome.
class WEBVIEW_API DownloadsAPI final : public ExtensionAPI {
public:
    explicit DownloadsAPI(ExtensionRuntime&);
    virtual ~DownloadsAPI() override;

    virtual void extensions_changed() override;
    virtual void shut_down() override;

private:
    class Observer;
    friend class Observer;

    // The application's downloads exist only once it's whole: they're watched from the first use.
    void observe();

    ErrorOr<u64, String> start_download(ExtensionRuntime::Context const&, Extension const&, JsonObject const& options);
    ErrorOr<JsonArray, String> search(Extension const&, JsonObject const& query) const;
    JsonObject item_json(FileDownloader::Download const&) const;
    void dispatch(StringView event, JsonValue const&);

    void download_added(FileDownloader::Download const&);
    void download_updated(FileDownloader::Download const&);
    void download_removed(u64 id, IsPrivate);

    OwnPtr<Observer> m_observer;
    // While an extension starts a download: who, and what it is.
    Optional<String> m_starting_for;
    Optional<String> m_starting_mime_type;
    // By FileDownloader's ID: the extension that started the download, its type when known, when it ended, and what
    // extensions last heard of it (for onChanged's deltas).
    HashMap<u64, String> m_started_by;
    HashMap<u64, String> m_mime_types;
    HashMap<u64, UnixDateTime> m_end_times;
    HashMap<u64, JsonObject> m_last_seen;
};

}
