/*
 * Text and filtering for the downloads shelf. No dependencies beyond Qt, so it can be tested on its own.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/SidebarWidgets.h>

#include <QStringList>

namespace Ladybird::Arc {

QString downloads_relative_time(QDateTime const& time, QDateTime const& now)
{
    auto seconds = time.secsTo(now);
    if (seconds < 60)
        return "Just now";
    if (seconds < 3600)
        return seconds < 120 ? "1 minute ago" : QString("%1 minutes ago").arg(seconds / 60);
    if (seconds < 86400)
        return seconds < 7200 ? "1 hour ago" : QString("%1 hours ago").arg(seconds / 3600);
    auto days = seconds / 86400;
    if (days < 30)
        return days == 1 ? "1 day ago" : QString("%1 days ago").arg(days);
    return time.date().toString("MMM d");
}

bool is_partial_download_name(QString const& name)
{
    static QStringList const suffixes { ".download", ".part", ".crdownload", ".partial", ".tmp" };
    for (auto const& suffix : suffixes) {
        if (name.endsWith(suffix, Qt::CaseInsensitive))
            return true;
    }
    return name.startsWith('.');
}

}
