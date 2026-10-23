/*
 * Reads Arc's sidebar data (StorableSidebar.json) into Arc-fork spaces: each Arc space with its pinned tree
 * of folders and tabs, plus the favorites. Qt only, so it can be tested with fixture data.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <UI/Qt/Arc/SpaceStore.h>

namespace Ladybird::Arc {

// A tab from a space's Today list (Arc's unpinned tabs). Opened as a regular tab, not pinned.
struct ArcTodayTab {
    QString space_source_id;
    QString url;
    QString title;
};

struct ArcSidebarData {
    QList<Space> spaces; // Ids are left empty; source_id is the Arc space's id.
    QList<Favorite> favorites;
    QList<ArcTodayTab> today_tabs;
    int folders { 0 };
    int tabs { 0 };
    int skipped_items { 0 }; // Arc-only item kinds (notes, easels) with no Ladybird equivalent.
    QStringList notes;       // Things worth telling the user (e.g. an icon that had to fall back).
};

// Parses StorableSidebar.json's contents. Returns nullopt-like empty data with a note on malformed input.
ArcSidebarData parse_arc_sidebar(QByteArray const& json);

// Maps an Arc space icon name to an SF Symbol name, or returns an empty string when there is none.
QString sf_symbol_for_arc_icon(QString const& arc_icon);

}
