/*
 * Arc profiles: each profile's browsing session (its own cookies, site storage and HSTS policies), kept in the
 * Ladybird profile's data directory under ArcProfiles/. The default profile (an empty name) is Ladybird's default
 * session.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibWebView/Forward.h>
#include <QString>

namespace Ladybird::Arc {

// The session for a profile, created on first use; null for the default profile (or if it can't be opened, in
// which case the profile's tabs fall back to the default session and a warning is logged).
RefPtr<WebView::BrowsingSession> session_for_profile(QString const& profile);

// The directory name a profile's data lives in: its name with anything but letters, digits, spaces, dashes and
// underscores replaced, so a name can never leave ArcProfiles/.
QString profile_directory_name(QString const& profile);

// While alive, new web views open in the given profile; afterwards, in whatever was chosen before.
class ScopedProfileForNewViews {
public:
    explicit ScopedProfileForNewViews(QString const& profile);
    ~ScopedProfileForNewViews();

private:
    RefPtr<WebView::BrowsingSession> m_previous;
};

// Opens the session of every profile a space uses, so browser-wide actions (clearing site data) reach all of them,
// not only those visited this run.
void open_all_profile_sessions();

// The profile new views use outside a ScopedProfileForNewViews: the current space's, in the active window.
void set_profile_for_new_views(QString const& profile);
RefPtr<WebView::BrowsingSession> session_for_new_views();

}
