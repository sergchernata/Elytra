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

QStringList const& space_symbol_names()
{
    static QStringList const names;
    return names;
}

}
