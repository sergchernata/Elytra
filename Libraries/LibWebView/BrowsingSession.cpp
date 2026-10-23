/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Directory.h>
#include <LibDatabase/Database.h>
#include <LibWebView/Application.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/SessionStore.h>
#include <LibWebView/StorageJar.h>

namespace WebView {

BrowsingSession::BrowsingSession(IsPrivate is_private)
    : m_is_private(is_private)
{
}

BrowsingSession::~BrowsingSession() = default;

NonnullRefPtr<BrowsingSession> BrowsingSession::create(IsPrivate is_private)
{
    auto session = adopt_ref(*new BrowsingSession(is_private));

    if (is_private == IsPrivate::Yes) {
        session->cookie_jar = CookieJar::create(IsPrivate::Yes);
        session->storage_jar = StorageJar::create();
        session->blob_url_store = make<BlobURLStore>();
        session->hsts_store = HSTSStore::create();
        session->favicon_store = FaviconStore::create();
        session->history_store = HistoryStore::create_disabled();
        session->session_store = SessionStore::create();
    }

    return session;
}

// Fork hook (Arc profiles)
ErrorOr<NonnullRefPtr<BrowsingSession>> BrowsingSession::create_persistent(ByteString const& directory, BrowsingSession& records)
{
    auto session = adopt_ref(*new BrowsingSession(IsPrivate::No));
    session->m_records = records;

    TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    auto database = TRY(Database::Database::create(directory, "Ladybird"sv));

    // As for the default session: migrate every store first, and persist none of them if the file is too new.
    auto persist = TRY(CookieJar::migrate_schema(database)) == Database::MigrationOutcome::Success
        && TRY(HSTSStore::migrate_schema(database)) == Database::MigrationOutcome::Success
        && TRY(StorageJar::migrate_schema(database)) == Database::MigrationOutcome::Success;
    if (persist) {
        session->cookie_jar = TRY(CookieJar::create(database));
        session->hsts_store = TRY(HSTSStore::create(database));
        session->storage_jar = TRY(StorageJar::create(database));
    } else {
        warnln("Profile database in {} was created by a newer Ladybird version; its site data will not be persisted", directory);
        session->cookie_jar = CookieJar::create();
        session->hsts_store = HSTSStore::create();
        session->storage_jar = StorageJar::create();
    }
    session->blob_url_store = make<BlobURLStore>();

    // Never used: views of this session keep their records in `records` (see records()). Present so that no store is
    // ever null.
    session->favicon_store = FaviconStore::create();
    session->history_store = HistoryStore::create_disabled();
    session->session_store = SessionStore::create();

    Application::did_create_profile_session(*session);
    return session;
}

}
