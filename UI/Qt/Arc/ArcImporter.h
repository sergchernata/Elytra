/*
 * Imports Arc's spaces, folders, pinned tabs, favorites and bookmarks into Ladybird bookmarks.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <QString>

class QWidget;

namespace Ladybird::Arc {

struct ImportSummary {
    size_t spaces { 0 };
    size_t folders { 0 };
    size_t tabs { 0 };
    size_t bookmarks { 0 };
};

// Default location of Arc's data on macOS.
QString default_arc_data_directory();

// Reads Arc's data and replaces any earlier Arc import under one top-level "Arc" bookmark folder:
//   Arc/<Space>/<Arc folders and pinned tabs>, Arc/<Space>/Today (unpinned tabs),
//   Arc/Favorites (<profile>), Arc/Bookmarks (<profile>)
// Space colors and profiles are saved for the Spaces sidebar.
ErrorOr<ImportSummary> import_from_arc(QString const& arc_data_directory);

// Menu entry point: confirms, imports and reports the result.
void run_import_dialog(QWidget* parent);

}
