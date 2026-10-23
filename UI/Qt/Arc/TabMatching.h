/*
 * Which open tabs the command bar offers for what's typed. Header-only, so it can be tested on its own.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QString>
#include <QStringList>

namespace Ladybird::Arc {

// Whether a tab's title or URL contains every word of the query (case-insensitively). An empty query matches all.
inline bool tab_matches_query(QString const& query, QString const& title, QString const& url)
{
    for (auto const& word : query.split(' ', Qt::SkipEmptyParts)) {
        if (!title.contains(word, Qt::CaseInsensitive) && !url.contains(word, Qt::CaseInsensitive))
            return false;
    }
    return true;
}

}
