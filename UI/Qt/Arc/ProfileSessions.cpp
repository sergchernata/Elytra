/*
 * Arc profiles: each profile's browsing session.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Profile.h>
#include <UI/Qt/Arc/ProfileSessions.h>
#include <UI/Qt/Arc/SpaceStore.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QHash>

namespace Ladybird::Arc {

// What new views use outside a scope; ScopedProfileForNewViews restores it.
static RefPtr<WebView::BrowsingSession> s_session_for_new_views;

// The name as it was before folder names got a hash (it could collide).
static QString legacy_profile_directory_name(QString const& profile)
{
    QString name;
    for (auto character : profile) {
        if (character.isLetterOrNumber() || character == ' ' || character == '-' || character == '_')
            name.append(character);
        else
            name.append('_');
    }
    if (name.trimmed().isEmpty())
        name = "_";
    return name;
}

QString profile_directory_name(QString const& profile)
{
    // Readable, plus a hash of the exact name: profiles differing only in case, or in characters replaced here,
    // must not share a folder (and with it a cookie database).
    auto hash = QCryptographicHash::hash(profile.toUtf8(), QCryptographicHash::Sha256).toHex().left(10);
    return legacy_profile_directory_name(profile) + "-" + QString::fromLatin1(hash);
}

// Created on first use. Released when the app quits, so each session's last cookies and site data are written out
// as the views using it go away (stores save periodically, and when destroyed).
static QHash<QString, RefPtr<WebView::BrowsingSession>>& sessions_by_profile()
{
    static auto* sessions = [] {
        auto* sessions = new QHash<QString, RefPtr<WebView::BrowsingSession>>;
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, [sessions] {
            WebView::Application::set_session_for_new_views({});
            s_session_for_new_views = nullptr;
            sessions->clear();
        });
        return sessions;
    }();
    return *sessions;
}

RefPtr<WebView::BrowsingSession> session_for_profile(QString const& profile)
{
    if (profile.isEmpty())
        return {};

    auto& sessions = sessions_by_profile();
    if (auto it = sessions.find(profile); it != sessions.end())
        return it.value();

    auto const& data_directory = WebView::Application::profile().paths().data;
    auto profiles = QString::fromUtf8(data_directory.characters(), static_cast<qsizetype>(data_directory.length())) + "/ArcProfiles/";
    // Folders made before names got a hash move to their new name.
    if (auto legacy = profiles + legacy_profile_directory_name(profile), current = profiles + profile_directory_name(profile); QDir(legacy).exists() && !QDir(current).exists())
        QDir().rename(legacy, current);
    auto directory = ByteString::formatted("{}/ArcProfiles/{}", data_directory, profile_directory_name(profile).toUtf8().constData());
    auto persist = WebView::Application::browser_options().disable_sql_database == WebView::DisableSQLDatabase::Yes
        ? WebView::BrowsingSession::Persist::No
        : WebView::BrowsingSession::Persist::Yes;
    auto session = WebView::BrowsingSession::create_persistent(directory, WebView::Application::default_session(), persist);
    if (session.is_error()) {
        // Never fall back to the default profile's cookies: keep this profile's data apart, in memory, for this run.
        warnln("Unable to open the data of Arc profile \"{}\" ({}); keeping it in memory this time", profile.toUtf8().constData(), session.error());
        session = WebView::BrowsingSession::create_persistent(directory, WebView::Application::default_session(), WebView::BrowsingSession::Persist::No);
    }
    sessions.insert(profile, session.value());
    return session.release_value();
}

void open_all_profile_sessions()
{
    for (auto const& space : SpaceStore::the().spaces())
        (void)session_for_profile(space.profile);
}

void set_profile_for_new_views(QString const& profile)
{
    s_session_for_new_views = session_for_profile(profile);
    WebView::Application::set_session_for_new_views(s_session_for_new_views);
}

RefPtr<WebView::BrowsingSession> session_for_new_views()
{
    return s_session_for_new_views;
}

ScopedProfileForNewViews::ScopedProfileForNewViews(QString const& profile)
    : m_previous(s_session_for_new_views)
{
    WebView::Application::set_session_for_new_views(session_for_profile(profile));
}

ScopedProfileForNewViews::~ScopedProfileForNewViews()
{
    WebView::Application::set_session_for_new_views(m_previous);
}

}
