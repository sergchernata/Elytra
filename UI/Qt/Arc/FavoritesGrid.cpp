/*
 * Arc's favorites grid.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/FavoritesGrid.h>
#include <UI/Qt/ChromeStyle.h>

#include <QApplication>
#include <QEasingCurve>
#include <QGraphicsDropShadowEffect>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <QtMath>

namespace Ladybird::Arc {

static constexpr qreal TILE_RADIUS = 12;
static constexpr int ICON_SIZE = 16;
static constexpr qreal INNER_INSET = 2;
static constexpr int EMPHASIS_MS = 140;
static constexpr int LIFT_MS = 160;

// --- Tile -----------------------------------------------------------------------------------------

FavoriteTile::FavoriteTile(QWidget* parent)
    : QToolButton(parent)
{
    setIconSize(QSize(ICON_SIZE, ICON_SIZE));
    setCheckable(true);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);

    m_emphasis_animation = new QVariantAnimation(this);
    m_emphasis_animation->setDuration(EMPHASIS_MS);
    m_emphasis_animation->setEasingCurve(QEasingCurve::OutQuad);
    connect(m_emphasis_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_emphasis = value.toReal();
        update();
    });

    m_lift_animation = new QVariantAnimation(this);
    m_lift_animation->setDuration(LIFT_MS);
    m_lift_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_lift_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_lift = value.toReal();
        if (auto* shadow = qobject_cast<QGraphicsDropShadowEffect*>(graphicsEffect()))
            shadow->setColor(QColor(0, 0, 0, qRound(70 * m_lift)));
        update();
    });
    connect(m_lift_animation, &QVariantAnimation::finished, this, [this] {
        if (m_lift <= 0)
            setGraphicsEffect(nullptr);
    });
}

void FavoriteTile::set_dark(bool dark)
{
    if (m_dark == dark)
        return;
    m_dark = dark;
    update();
}

void FavoriteTile::set_lifted(bool lifted)
{
    if (lifted && !graphicsEffect()) {
        // The shadow paints outside the tile, under its neighbors' gaps; only a lifted tile has one.
        auto* shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(16);
        shadow->setOffset(0, 3);
        shadow->setColor(QColor(0, 0, 0, 0));
        setGraphicsEffect(shadow);
    }
    m_lift_animation->stop();
    m_lift_animation->setStartValue(m_lift);
    m_lift_animation->setEndValue(lifted ? 1.0 : 0.0);
    if (!isVisible()) {
        m_lift = lifted ? 1 : 0;
        if (!lifted)
            setGraphicsEffect(nullptr);
        return;
    }
    m_lift_animation->start();
}

qreal FavoriteTile::target_emphasis() const
{
    if (isChecked())
        return 1;
    return m_hovered ? 0.5 : 0;
}

void FavoriteTile::animate_emphasis()
{
    auto target = target_emphasis();
    if (m_emphasis_animation->state() == QAbstractAnimation::Running && m_emphasis_animation->endValue().toReal() == target)
        return;
    m_emphasis_animation->stop();
    if (!isVisible()) {
        m_emphasis = target;
        update();
        return;
    }
    m_emphasis_animation->setStartValue(m_emphasis);
    m_emphasis_animation->setEndValue(target);
    m_emphasis_animation->start();
}

void FavoriteTile::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    animate_emphasis();
    QToolButton::enterEvent(event);
}

void FavoriteTile::leaveEvent(QEvent* event)
{
    m_hovered = false;
    animate_emphasis();
    QToolButton::leaveEvent(event);
}

void FavoriteTile::checkStateSet()
{
    QToolButton::checkStateSet();
    animate_emphasis();
}

void FavoriteTile::nextCheckState()
{
    QToolButton::nextCheckState();
    animate_emphasis();
}

// A box blur, a few times over (close to a gaussian), with the edges held: the rim keeps the colors at the icon's edges.
static void blur(QImage& image, int radius, int passes)
{
    auto width = image.width();
    auto height = image.height();
    QList<QRgb> line(qMax(width, height));
    auto blur_line = [&](auto get, auto set, int length) {
        for (int i = 0; i < length; ++i)
            line[i] = get(i);
        for (int i = 0; i < length; ++i) {
            int sums[4] {};
            for (int k = -radius; k <= radius; ++k) {
                auto pixel = line[qBound(0, i + k, length - 1)];
                sums[0] += qAlpha(pixel);
                sums[1] += qRed(pixel);
                sums[2] += qGreen(pixel);
                sums[3] += qBlue(pixel);
            }
            auto count = radius * 2 + 1;
            set(i, qRgba(sums[1] / count, sums[2] / count, sums[3] / count, sums[0] / count));
        }
    };
    for (int pass = 0; pass < passes; ++pass) {
        for (int y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            blur_line([&](int x) { return row[x]; }, [&](int x, QRgb value) { row[x] = value; }, width);
        }
        for (int x = 0; x < width; ++x) {
            blur_line([&](int y) { return reinterpret_cast<QRgb const*>(image.constScanLine(y))[x]; },
                [&](int y, QRgb value) { reinterpret_cast<QRgb*>(image.scanLine(y))[x] = value; }, height);
        }
    }
}

// How much a pixel's color counts: its black, white and gray (a dark square behind a logo, say) would only darken or
// wash out the tile.
static qreal color_weight(QColor const& color)
{
    return color.alphaF() * qBound<qreal>(0, (color.hsvSaturationF() * color.valueF() - 0.15) / 0.35, 1);
}

QImage FavoriteTile::backdrop_for(QIcon const& icon, QSize size)
{
    if (icon.isNull() || size.isEmpty())
        return {};
    auto source = icon.pixmap(QSize(32, 32), 1.0).toImage().convertToFormat(QImage::Format_ARGB32);
    if (source.isNull())
        return {};

    // Each pixel weighed by its color, and where the colored part of the icon is. An icon with no color at all (black
    // on white, say) counts as it is, and gives a gray.
    QImage weighted(source.size(), QImage::Format_ARGB32_Premultiplied);
    QRect colored;
    qreal sums[4] {};
    for (int y = 0; y < source.height(); ++y) {
        auto const* in = reinterpret_cast<QRgb const*>(source.constScanLine(y));
        auto* out = reinterpret_cast<QRgb*>(weighted.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            auto color = QColor::fromRgba(in[x]);
            auto weight = color_weight(color);
            if (weight > 0.25)
                colored |= QRect(x, y, 1, 1);
            sums[0] += weight;
            sums[1] += color.redF() * weight;
            sums[2] += color.greenF() * weight;
            sums[3] += color.blueF() * weight;
            color.setAlphaF(weight);
            out[x] = qPremultiply(color.rgba());
        }
    }
    if (sums[0] < 0.5) {
        weighted = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        colored = {};
        sums[0] = sums[1] = sums[2] = sums[3] = 0;
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                auto color = source.pixelColor(x, y);
                sums[0] += color.alphaF();
                sums[1] += color.redF() * color.alphaF();
                sums[2] += color.greenF() * color.alphaF();
                sums[3] += color.blueF() * color.alphaF();
            }
        }
    }
    if (colored.isEmpty())
        colored = weighted.rect();
    auto average = sums[0] > 0 ? QColor::fromRgbF(sums[1] / sums[0], sums[2] / sums[0], sums[3] / sums[0]) : QColor(Qt::gray);

    // The colored part, scaled to cover the tile, as Arc does: a logo's top color is the rim's top, and so on. Worked
    // on small, where blurring is cheap; scaling it up smooth softens it further.
    constexpr int small_width = 20;
    auto small_height = qMax(4, qRound(small_width * qreal(size.height()) / size.width()));
    QImage small(small_width, small_height, QImage::Format_ARGB32_Premultiplied);
    small.fill(Qt::transparent);
    {
        QPainter painter(&small);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        auto scale = qMax(small_width / qreal(colored.width()), small_height / qreal(colored.height()));
        QSizeF covered(colored.width() * scale, colored.height() * scale);
        painter.drawImage(QRectF(QPointF((small_width - covered.width()) / 2, (small_height - covered.height()) / 2), covered), weighted, colored);
    }
    blur(small, 2, 3);

    // Filling the whole tile (the average where the icon has no color nearby), and kept light: never darker than a
    // pastel.
    for (int y = 0; y < small.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(small.scanLine(y));
        for (int x = 0; x < small.width(); ++x) {
            auto here = QColor::fromRgba(qUnpremultiply(row[x]));
            auto mix = qMin<qreal>(1, here.alphaF() * 3);
            auto color = QColor::fromRgbF(average.redF() + (here.redF() - average.redF()) * mix,
                average.greenF() + (here.greenF() - average.greenF()) * mix,
                average.blueF() + (here.blueF() - average.blueF()) * mix)
                             .toHsl();
            color.setHslF(color.hslHueF(), color.hslSaturationF(), qMax<qreal>(color.lightnessF(), 0.6));
            row[x] = color.rgb();
        }
    }
    return small.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

void FavoriteTile::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    auto bounds = QRectF(rect());
    QPainterPath shape;
    shape.addRoundedRect(bounds, TILE_RADIUS, TILE_RADIUS);
    painter.setClipPath(shape);

    // Hovered is the first half of the emphasis, active (the tile of the tab shown) the second.
    auto hovered = qMin<qreal>(1, qMin<qreal>(m_emphasis, 0.5) * 2 + m_lift * 0.6);
    auto active = qBound<qreal>(0, (m_emphasis - 0.5) * 2, 1);

    // Every tile: a see-through white fill, a little lighter while hovered (or lifted).
    auto rest = m_dark ? 0.09 : 0.45;
    auto hover = m_dark ? 0.14 : 0.62;
    painter.fillRect(bounds, QColor::fromRgbF(1, 1, 1, rest + (hover - rest) * hovered));

    // Only the active tile: its favicon's wash of color, with a softer fill set in from its edge over it, so the
    // color shows as a rim.
    auto dpr = devicePixelRatioF();
    if (active > 0) {
        auto device_size = (bounds.size() * dpr).toSize();
        if (icon().cacheKey() != m_backdrop_key || device_size != m_backdrop_size) {
            m_backdrop_key = icon().cacheKey();
            m_backdrop_size = device_size;
            m_backdrop = backdrop_for(icon(), device_size);
        }
        painter.setOpacity(active);
        if (!m_backdrop.isNull()) {
            painter.drawImage(bounds, m_backdrop);
            // Pastel, as Arc's is.
            painter.fillRect(bounds, QColor::fromRgbF(1, 1, 1, m_dark ? 0.15 : 0.25));
        }

        auto inner = bounds.adjusted(INNER_INSET, INNER_INSET, -INNER_INSET, -INNER_INSET);
        QPainterPath inner_shape;
        inner_shape.addRoundedRect(inner, TILE_RADIUS - INNER_INSET, TILE_RADIUS - INNER_INSET);
        // Calmed toward the sidebar's own color (the space's), then lit.
        auto calm = ChromeStyle::chrome_background(palette());
        calm.setAlphaF(m_backdrop.isNull() ? 0 : 0.75);
        painter.fillPath(inner_shape, calm);
        painter.fillPath(inner_shape, QColor::fromRgbF(1, 1, 1, m_dark ? 0.44 : 0.6));
        painter.setOpacity(1);
    }

    // A faint rim of light.
    painter.setClipping(false);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor::fromRgbF(1, 1, 1, m_dark ? 0.06 * (1 - active) : 0.25 * (1 - active)), 1));
    painter.drawRoundedRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5), TILE_RADIUS - 0.5, TILE_RADIUS - 0.5);

    // The favicon itself, sunk a touch while pressed.
    auto pixmap = icon().pixmap(iconSize(), dpr);
    auto icon_rect = QRectF(QPointF(0, 0), QSizeF(iconSize()));
    icon_rect.moveCenter(bounds.center() + QPointF(0, isDown() ? 0.5 : 0));
    painter.drawPixmap(icon_rect, pixmap, pixmap.rect());
}

// --- Grid -----------------------------------------------------------------------------------------

FavoritesGrid::FavoritesGrid(int tile_height, QWidget* parent)
    : QWidget(parent)
    , m_tile_height(tile_height)
{
    auto policy = QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

QList<FavoriteTile*> FavoritesGrid::tiles() const
{
    QList<FavoriteTile*> tiles;
    for (auto const& tile : m_tiles) {
        if (tile)
            tiles.append(tile);
    }
    return tiles;
}

int FavoritesGrid::slot_count() const
{
    return tiles().size() + (m_gap >= 0 ? 1 : 0);
}

int FavoritesGrid::height_for(int, int slot_total) const
{
    if (slot_total <= 0)
        return 0;
    auto rows = (slot_total + COLUMNS - 1) / COLUMNS;
    return rows * m_tile_height + (rows - 1) * SPACING + BOTTOM_MARGIN;
}

int FavoritesGrid::heightForWidth(int width) const
{
    return height_for(width, slot_count());
}

QSize FavoritesGrid::sizeHint() const
{
    return { COLUMNS * 40, heightForWidth(width()) };
}

QSize FavoritesGrid::minimumSizeHint() const
{
    return { COLUMNS * 20, heightForWidth(width()) };
}

QRect FavoritesGrid::slot_rect(int slot) const
{
    // Four equal columns however many tiles there are, like Arc: one favorite is a tile, not a full-width bar.
    auto column = slot % COLUMNS;
    auto row = slot / COLUMNS;
    auto tile_width = (width() - SPACING * (COLUMNS - 1)) / qreal(COLUMNS);
    auto left = qRound(column * (tile_width + SPACING));
    auto right = qRound(column * (tile_width + SPACING) + tile_width);
    return { left, row * (m_tile_height + SPACING), right - left, m_tile_height };
}

int FavoritesGrid::slot_at(QPoint position, int slot_total) const
{
    if (slot_total <= 0)
        return 0;
    auto rows = (slot_total + COLUMNS - 1) / COLUMNS;
    auto column = qBound(0, qFloor(position.x() / ((width() + SPACING) / qreal(COLUMNS))), COLUMNS - 1);
    auto row = qBound(0, qFloor(position.y() / qreal(m_tile_height + SPACING)), rows - 1);
    return qMin(row * COLUMNS + column, slot_total - 1);
}

int FavoritesGrid::insertion_index_at(QPoint position) const
{
    // Counted with the gap's slot in, so the gap stays put under the pointer once it has opened.
    return slot_at(position, tiles().size() + 1);
}

void FavoritesGrid::show_insertion_gap(QPoint position)
{
    auto gap = insertion_index_at(position);
    if (gap == m_gap)
        return;
    auto rows_before = slot_count();
    m_gap = gap;
    if (slot_count() != rows_before)
        updateGeometry();
    place_tiles(Animate::Yes);
}

void FavoritesGrid::hide_insertion_gap(Animate animate)
{
    if (m_gap < 0)
        return;
    m_gap = -1;
    updateGeometry();
    // Not animated: the tiles stay where they are, for the one about to be dropped into the gap.
    if (animate == Animate::Yes)
        place_tiles(Animate::Yes);
}

void FavoritesGrid::set_tiles(QList<FavoriteTile*> const& new_tiles)
{
    auto old_tiles = tiles();
    auto added = false;
    for (auto* tile : new_tiles) {
        if (old_tiles.contains(tile))
            continue;
        added = true;
        if (tile->parentWidget() != this)
            tile->setParent(this);
        tile->installEventFilter(this);
        connect(tile, &QObject::destroyed, this, [this, tile] { m_animations.remove(tile); });
    }
    for (auto* tile : old_tiles) {
        if (new_tiles.contains(tile))
            continue;
        tile->removeEventFilter(this);
        if (auto* animation = m_animations.take(tile))
            animation->stop();
    }

    // Whatever was being dragged has been taken over by the change.
    if (m_dragging) {
        if (auto* dragged = m_tiles.value(m_drag_from).data()) {
            dragged->set_lifted(false);
            dragged->unsetCursor();
        }
        m_dragging = false;
        m_pressed = nullptr;
    }

    m_tiles.clear();
    for (auto* tile : new_tiles)
        m_tiles.append(tile);
    updateGeometry();

    // New tiles show in their places at once: the others gliding past them would cross over them.
    place_tiles(added ? Animate::No : Animate::Yes);
    if (isVisible()) {
        for (auto* tile : new_tiles)
            tile->show();
    }
}

void FavoritesGrid::move_tile(FavoriteTile& tile, QRect const& rect, Animate animate)
{
    auto* animation = m_animations.value(&tile);
    if (animate == Animate::No || !isVisible()) {
        if (animation)
            animation->stop();
        tile.setGeometry(rect);
        return;
    }
    tile.resize(rect.size());
    if (animation && animation->state() == QAbstractAnimation::Running && animation->endValue().toPoint() == rect.topLeft())
        return;
    if (tile.pos() == rect.topLeft()) {
        if (animation)
            animation->stop();
        return;
    }
    if (!animation) {
        animation = new QPropertyAnimation(&tile, "pos", &tile);
        animation->setDuration(MOVE_MS);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        m_animations.insert(&tile, animation);
    }
    animation->stop();
    animation->setStartValue(tile.pos());
    animation->setEndValue(rect.topLeft());
    animation->start();
}

void FavoritesGrid::place_tiles(Animate animate)
{
    auto order = tiles();
    FavoriteTile* dragged = nullptr;
    if (m_dragging && m_drag_from >= 0 && m_drag_from < order.size()) {
        dragged = order[m_drag_from];
        order.move(m_drag_from, qBound(0, m_drag_to, static_cast<int>(order.size()) - 1));
    }
    for (int i = 0; i < order.size(); ++i) {
        if (order[i] == dragged) {
            // It follows the pointer; only its size keeps up.
            dragged->resize(slot_rect(0).size());
            continue;
        }
        auto slot = m_gap >= 0 && i >= m_gap ? i + 1 : i;
        move_tile(*order[i], slot_rect(slot), animate);
    }
}

void FavoritesGrid::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    place_tiles(Animate::No);
}

void FavoritesGrid::begin_drag(FavoriteTile& tile)
{
    auto index = tiles().indexOf(&tile);
    if (index < 0)
        return;
    m_dragging = true;
    m_drag_from = index;
    m_drag_to = index;
    m_grab_offset = m_press_position - tile.pos();
    if (auto* animation = m_animations.value(&tile))
        animation->stop();
    // Let go, it isn't a click.
    tile.setDown(false);
    tile.raise();
    tile.set_lifted(true);
    tile.setCursor(Qt::ClosedHandCursor);
}

void FavoritesGrid::drag_to(QPoint position)
{
    auto order = tiles();
    if (m_drag_from < 0 || m_drag_from >= order.size())
        return;
    auto* tile = order[m_drag_from];
    auto top_left = position - m_grab_offset;
    top_left.setX(qBound(0, top_left.x(), qMax(0, width() - tile->width())));
    top_left.setY(qBound(0, top_left.y(), qMax(0, height() - BOTTOM_MARGIN - tile->height())));
    tile->move(top_left);

    auto center = top_left + QPoint(tile->width() / 2, tile->height() / 2);
    auto to = slot_at(center, order.size());
    if (to == m_drag_to)
        return;
    m_drag_to = to;
    place_tiles(Animate::Yes);
}

void FavoritesGrid::end_drag()
{
    auto order = tiles();
    m_dragging = false;
    if (m_drag_from < 0 || m_drag_from >= order.size())
        return;
    auto* tile = order[m_drag_from];
    auto from = m_drag_from;
    auto to = qBound(0, m_drag_to, static_cast<int>(order.size()) - 1);
    m_tiles.move(m_tiles.indexOf(tile), to);
    tile->set_lifted(false);
    tile->unsetCursor();
    place_tiles(Animate::Yes);
    if (from != to)
        emit tile_moved(from, to);
}

bool FavoritesGrid::eventFilter(QObject* watched, QEvent* event)
{
    auto* tile = qobject_cast<FavoriteTile*>(watched);
    if (!tile || !m_tiles.contains(tile))
        return QWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            m_pressed = tile;
            m_press_position = tile->mapTo(this, mouse->position().toPoint());
        }
        return false;
    }
    case QEvent::MouseMove: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (m_pressed != tile || !(mouse->buttons() & Qt::LeftButton))
            return false;
        auto position = tile->mapTo(this, mouse->position().toPoint());
        if (!m_dragging) {
            if ((position - m_press_position).manhattanLength() < QApplication::startDragDistance())
                return false;
            begin_drag(*tile);
        }
        drag_to(position);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton)
            return false;
        m_pressed = nullptr;
        // Passed on all the same: the tile isn't down any more, so it lets go without a click.
        if (m_dragging)
            end_drag();
        return false;
    }
    default:
        return false;
    }
}

}
