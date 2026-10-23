/*
 * Site icons for the Arc sidebar, kept per Ladybird profile (one PNG per site) so unloaded pinned tabs and
 * favorites show their icons. Filled from Arc's icon databases on import, and from pages as they load.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QIcon>
#include <QString>

namespace Ladybird::Arc::ArcIcons {

// Where the icons live; set once at startup (the profile's data directory).
void set_directory(QString const&);

// The cached icon for the site of this URL, or a null icon.
QIcon icon_for_url(QString const& url);

// Remembers a page's icon for its site (skipped if the site already has one).
void store(QString const& url, QIcon const&);

// Saves PNG data as the icon for the URL's site; returns false if the site already had one.
bool store_png(QString const& url, QByteArray const& png);

bool has_icon(QString const& url);

}
