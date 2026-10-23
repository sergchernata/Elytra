/*
 * The fork's releases on GitHub: reading the latest one and telling whether it's newer than this build.
 * Qt only, so it can be tested on its own.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <QByteArray>
#include <QString>

namespace Ladybird::Arc {

struct ReleaseInfo {
    QString tag;      // e.g. "arc-v0.2.0"
    QString page_url; // The release's page on GitHub.

    // The Apple Silicon Mac download and its published SHA-256, when the release has both (from GitHub).
    QString archive_name;
    QString archive_url;
    QString checksum_url;

    bool can_install() const { return !archive_url.isEmpty() && !checksum_url.isEmpty(); }
};

// Reads GitHub's "latest release" response. Nothing for drafts, prereleases or anything unreadable.
Optional<ReleaseInfo> parse_latest_release(QByteArray const& json);

// Whether `candidate` is a later release than `current`. Tags are arc-v<major>.<minor>.<patch> (a missing part is
// 0), or arc-<year>.<month>.<day>-<commit> for releases made by hand. Anything else is never newer.
bool is_newer_release(QString const& current, QString const& candidate);

// The hex SHA-256 a checksum file (as written by `shasum -a 256`) gives for `file_name`; empty if it gives none.
QString checksum_for(QByteArray const& checksum_file, QString const& file_name);

// A shell script that waits for process `browser_pid` to exit, puts the app at `staged_app` in place of the one at
// `installed_app` (restoring the old one if that fails), then runs `relaunch_command`.
QString install_script(QString const& installed_app, QString const& staged_app, qint64 browser_pid, QString const& relaunch_command);

}
