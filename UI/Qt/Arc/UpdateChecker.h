/*
 * Arc-style updates: checks the fork's GitHub releases for one newer than this build, downloads and verifies it in
 * the background, and on request restarts into it.
 *
 * Only release builds check: the release workflow builds them with ARC_RELEASE_TAG and ARC_RELEASE_REPOSITORY
 * (see Arc.cmake). ARC_DEV_RELEASE_TAG and ARC_DEV_RELEASE_REPOSITORY set them for trying it out locally.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibRequests/Forward.h>
#include <QObject>
#include <UI/Qt/Arc/ReleaseInfo.h>

class QTimer;

namespace Ladybird::Arc {

class UpdateChecker final : public QObject {
    Q_OBJECT

public:
    enum class State {
        UpToDate,
        Preparing, // Downloading and verifying a newer release.
        Ready,     // Staged: restarting installs it.
        PageOnly,  // A newer release this build can't install itself: offer its page.
    };

    static UpdateChecker& the();

    // Checks shortly after launch, then every few hours. Does nothing in builds that aren't releases.
    void start();

    State state() const { return m_state; }
    Optional<ReleaseInfo> const& release() const { return m_release; }

    // Quits (as the Quit command does), then puts the staged release in place and relaunches it.
    void restart_into_update();

signals:
    void state_changed();

private:
    UpdateChecker() = default;
    void check();
    void found(ReleaseInfo);
    void prepare();
    void fail_preparing(QString const& why);
    void set_state(State);
    void fetch(QString const& url, Function<void(Optional<QByteArray>)> done);

    QString installed_app_path() const;
    QString staging_directory() const;

    QString m_current_tag;
    QString m_repository;
    QTimer* m_timer { nullptr };
    RefPtr<Requests::Request> m_request;
    Optional<ReleaseInfo> m_release;
    State m_state { State::UpToDate };
    QString m_staged_app;
};

}
