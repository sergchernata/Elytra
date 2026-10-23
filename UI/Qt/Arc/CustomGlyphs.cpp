/*
 * Monochrome glyphs for Arc space icons that SF Symbols doesn't have, drawn with QPainter so they tint and
 * size like the symbols around them.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/SymbolIcon.h>

#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace Ladybird::Arc {

// A filled skull in a 100x100 box: rounded cranium, jaw, eye sockets and nose cut out, teeth gaps.
static QPainterPath skull_path()
{
    QPainterPath head;
    head.addEllipse(QPointF(50, 42), 40, 37);
    QPainterPath jaw;
    jaw.addRoundedRect(QRectF(27, 56, 46, 38), 9, 9);
    auto shape = head.united(jaw);

    QPainterPath holes;
    holes.addEllipse(QPointF(34, 46), 11, 12);
    holes.addEllipse(QPointF(66, 46), 11, 12);
    QPainterPath nose;
    nose.moveTo(50, 59);
    nose.lineTo(44, 70);
    nose.lineTo(56, 70);
    nose.closeSubpath();
    holes.addPath(nose);
    holes.addRect(QRectF(40.5, 79, 4, 15));
    holes.addRect(QRectF(55.5, 79, 4, 15));
    return shape.subtracted(holes);
}

QPixmap custom_glyph_pixmap(QString const& name, int box, QColor const& color, qreal device_pixel_ratio)
{
    QPainterPath path;
    if (name == "arc.skull")
        path = skull_path();
    else
        return {};

    auto size = static_cast<int>(std::round(box * device_pixel_ratio));
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    auto bounds = path.boundingRect();
    // A solid shape filling its whole box looks heavier than SF Symbols, whose outlines and openings leave air:
    // draw it a little smaller so it reads at the same size as the symbols beside it.
    static constexpr double OPTICAL_SCALE = 0.8;
    auto scale = size * OPTICAL_SCALE / qMax(bounds.width(), bounds.height());
    painter.translate((size - bounds.width() * scale) / 2.0, (size - bounds.height() * scale) / 2.0);
    painter.scale(scale, scale);
    painter.translate(-bounds.topLeft());
    painter.fillPath(path, color);
    painter.end();
    pixmap.setDevicePixelRatio(device_pixel_ratio);
    return pixmap;
}

}
