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

// Renders an SF Symbol tinted with one color. point_size is in logical pixels.
QPixmap symbol_pixmap(QString const& name, int point_size, QColor const& color, qreal device_pixel_ratio);

// Symbols offered for spaces, in picker order. Ones the system lacks are skipped at runtime.
QStringList const& space_symbol_names();

// Space icons are stored as "sf:<symbol name>"; anything else is literal text (older emoji icons).
inline bool is_symbol_icon(QString const& icon) { return icon.startsWith("sf:"); }
inline QString symbol_name(QString const& icon) { return icon.mid(3); }

}
