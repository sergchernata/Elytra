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

#include <QCoreApplication>
#include <QHash>

namespace Ladybird::Arc {

// What new views use outside a scope; ScopedProfileForNewViews restores it.
static RefPtr<WebView::BrowsingSession> s_session_for_new_views;

QString profile_directory_name(QString const& profile)
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
    auto directory = ByteString::formatted("{}/ArcProfiles/{}", data_directory, profile_directory_name(profile).toUtf8().constData());
    auto session = WebView::BrowsingSession::create_persistent(directory, WebView::Application::default_session());
    if (session.is_error()) {
        warnln("Unable to open the data of Arc profile \"{}\": {}", profile.toUtf8().constData(), session.error());
        sessions.insert(profile, nullptr);
        return {};
    }
    sessions.insert(profile, session.value());
    return session.release_value();
}

void set_profile_for_new_views(QString const& profile)
{
    s_session_for_new_views = session_for_profile(profile);
    WebView::Application::set_session_for_new_views(s_session_for_new_views);
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
