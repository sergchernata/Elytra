/*
 * Monochrome symbol icons for the Arc chrome. On macOS these are Apple's SF Symbols (as in Arc);
 * elsewhere symbol_pixmap() returns a null pixmap and callers fall back to a letter.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QColor>
#include <QPixmap>
#include <QString>
#include <QStringList>

namespace Ladybird::Arc {

// Renders an SF Symbol tinted with one color. point_size is in logical pixels. Names starting with "arc." are
// glyphs drawn here for Arc icons that SF Symbols lacks (e.g. "arc.skull"); they work on every platform.
QPixmap symbol_pixmap(QString const& name, int point_size, QColor const& color, qreal device_pixel_ratio);

// Like symbol_pixmap(), but prefers the filled variant ("<name>.fill") as Arc does, and scales the
// symbol so its larger side is exactly box logical pixels. Use this wherever icons sit side by side.
QPixmap fitted_symbol_pixmap(QString const& name, int box, QColor const& color, qreal device_pixel_ratio);

// Symbols offered for spaces, in picker order. Ones the system lacks are skipped at runtime.
QStringList const& space_symbol_names();

// Draws one of the "arc." glyphs, or returns a null pixmap for an unknown name.
QPixmap custom_glyph_pixmap(QString const& name, int box, QColor const& color, qreal device_pixel_ratio);

// Space icons are stored as "sf:<symbol name>"; anything else is literal text (older emoji icons).
inline bool is_symbol_icon(QString const& icon) { return icon.startsWith("sf:"); }
inline QString symbol_name(QString const& icon) { return icon.mid(3); }

}
