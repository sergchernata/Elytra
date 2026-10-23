/*
 * Imports Arc's spaces, folders, pinned tabs, favorites and bookmarks.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <QString>
#include <QStringList>

class QWidget;

namespace Ladybird::Arc {

struct ImportSummary {
    int spaces_added { 0 };
    int spaces_updated { 0 };
    int folders { 0 };
    int tabs { 0 };
    int favorites { 0 };
    int icons { 0 };
    int skipped_items { 0 };
    size_t bookmarks { 0 };
    QStringList notes;
};

// Default location of Arc's data on macOS.
QString default_arc_data_directory();

// Imports Arc's spaces (with their pinned folders and tabs) and favorites into the Arc sidebar, and Arc's
// ordinary bookmarks into Ladybird's bookmarks under an "Arc" folder. Re-importing updates the same spaces
// and replaces that bookmarks folder.
ErrorOr<ImportSummary> import_from_arc(QString const& arc_data_directory);
QString describe(ImportSummary const&);

// Menu entry point: confirms, imports and reports the result.
void run_import_dialog(QWidget* parent);

}
