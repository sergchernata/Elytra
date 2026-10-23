/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Types.h>
#include <AK/Weakable.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

enum class IsPrivate : u8 {
    No,
    Yes,
};

// The ordinary session lives as long as the application. A private one is owned by its views and content processes,
// and everything else holds it weakly, so nothing outliving them can bring one back.
class WEBVIEW_API BrowsingSession
    : public RefCounted<BrowsingSession>
    , public Weakable<BrowsingSession> {
public:
    static NonnullRefPtr<BrowsingSession> create(IsPrivate);
    ~BrowsingSession();

    // Fork hook (Arc profiles): a persistent session of its own, with cookies, site storage and HSTS policies in
    // `directory`. Its history, favicons and tab restore are `records`'s (see records()).
    static ErrorOr<NonnullRefPtr<BrowsingSession>> create_persistent(ByteString const& directory, BrowsingSession& records);

    // Fork hook (Arc profiles): the session that keeps the browser's own records (history, favicons, tab restore) for
    // views of this one. Itself, except for a profile's session.
    BrowsingSession& records() { return m_records ? *m_records : *this; }

    IsPrivate is_private() const { return m_is_private; }

    // NB: Null for the ordinary session until Application fills these in during startup.
    OwnPtr<CookieJar> cookie_jar;
    OwnPtr<StorageJar> storage_jar;
    OwnPtr<BlobURLStore> blob_url_store;
    OwnPtr<HSTSStore> hsts_store;
    OwnPtr<FaviconStore> favicon_store;
    OwnPtr<HistoryStore> history_store;
    OwnPtr<SessionStore> session_store;

private:
    explicit BrowsingSession(IsPrivate);

    IsPrivate m_is_private { IsPrivate::No };
    RefPtr<BrowsingSession> m_records; // Fork hook (Arc profiles)
};

}
