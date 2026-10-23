/*
 * Non-macOS stand-in for SymbolIcon.mm: no symbol set, so callers fall back to letters.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/SymbolIcon.h>

namespace Ladybird::Arc {

QPixmap symbol_pixmap(QString const&, int, QColor const&, qreal)
{
    return {};
}

QPixmap fitted_symbol_pixmap(QString const& name, int box, QColor const& color, qreal device_pixel_ratio)
{
    // No SF Symbols here; the "arc." glyphs are drawn in Qt and work anywhere.
    return custom_glyph_pixmap(name, box, color, device_pixel_ratio);
}

QStringList const& space_symbol_names()
{
    static QStringList const names { "arc.skull" };
    return names;
}

}
