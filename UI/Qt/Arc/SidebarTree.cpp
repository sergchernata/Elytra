/*
 * The list in the Arc sidebar: pinned folders and tabs, the "New Tab" row, then Today's tabs.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/SidebarTree.h>
#include <UI/Qt/Arc/SymbolIcon.h>
#include <UI/Qt/ChromeStyle.h>

#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace Ladybird::Arc {

static constexpr int ROW_HEIGHT = 36;
static constexpr int NEW_TAB_ROW_HEIGHT = 48;
static constexpr int ROW_RADIUS = 9;
static constexpr int FOLDER_ICON_SIZE = 20;
static constexpr int FONT_PIXEL_SIZE = 14;
static constexpr int ICON_SIZE = 16;
static constexpr int COMPACT_WIDTH = 90;
static constexpr char const* INTERNAL_MIME = "application/x-qstandarditemmodeldatalist";

RowKind row_kind(QModelIndex const& index)
{
    return static_cast<RowKind>(index.data(KindRole).toInt());
}

// --- Model ------------------------------------------------------------------------------------

QStringList SidebarModel::mimeTypes() const
{
    return { INTERNAL_MIME, "text/uri-list" };
}

bool SidebarModel::canDropMimeData(QMimeData const* data, Qt::DropAction, int, int, QModelIndex const&) const
{
    return data->hasFormat(INTERNAL_MIME) || data->hasUrls();
}

// --- Delegate ---------------------------------------------------------------------------------

QRect SidebarDelegate::close_button_rect(QRect const& row)
{
    return QRect(row.right() - 26, row.center().y() - 9, 18, 18);
}

QSize SidebarDelegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    auto height = row_kind(index) == RowKind::NewTab ? NEW_TAB_ROW_HEIGHT : ROW_HEIGHT;
    return QSize(option.rect.width(), height);
}

static void draw_chevron(QPainter& painter, QRectF rect, bool expanded, QColor color)
{
    painter.save();
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    auto center = rect.center();
    QPainterPath path;
    if (expanded) {
        path.moveTo(center + QPointF(-4, -2));
        path.lineTo(center + QPointF(0, 2));
        path.lineTo(center + QPointF(4, -2));
    } else {
        path.moveTo(center + QPointF(-2, -4));
        path.lineTo(center + QPointF(2, 0));
        path.lineTo(center + QPointF(-2, 4));
    }
    painter.drawPath(path);
    painter.restore();
}

static void draw_folder_glyph(QPainter& painter, QRectF rect, QColor color, bool open)
{
    painter.save();
    painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    auto r = rect.adjusted(1, 3, -1, -3);
    auto tab_right = r.left() + r.width() * 0.4;
    QPainterPath back;
    back.moveTo(r.left(), r.bottom());
    back.lineTo(r.left(), r.top() + 1.5);
    back.quadTo(r.left(), r.top(), r.left() + 1.5, r.top());
    back.lineTo(tab_right - 1, r.top());
    back.lineTo(tab_right + 1.5, r.top() + 2.5);
    back.lineTo(r.right() - 1.5, r.top() + 2.5);
    back.quadTo(r.right(), r.top() + 2.5, r.right(), r.top() + 4);
    if (!open) {
        back.lineTo(r.right(), r.bottom() - 1.5);
        back.quadTo(r.right(), r.bottom(), r.right() - 1.5, r.bottom());
        back.closeSubpath();
        painter.drawPath(back);
    } else {
        // Open folder: the front flap tilts forward.
        back.lineTo(r.right(), r.top() + r.height() * 0.42);
        painter.drawPath(back);
        QPainterPath front;
        front.moveTo(r.left(), r.bottom());
        front.lineTo(r.left() + r.width() * 0.18, r.top() + r.height() * 0.42);
        front.lineTo(r.right() + 2, r.top() + r.height() * 0.42);
        front.lineTo(r.right() - r.width() * 0.16, r.bottom());
        front.closeSubpath();
        painter.drawPath(front);
    }
    painter.restore();
}

static void draw_close_glyph(QPainter& painter, QRect rect, QColor color, bool hovered)
{
    painter.save();
    if (hovered) {
        painter.setPen(Qt::NoPen);
        auto background = color;
        background.setAlpha(40);
        painter.setBrush(background);
        painter.drawRoundedRect(rect, 5, 5);
    }
    painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap));
    auto c = QRectF(rect).center();
    painter.drawLine(c + QPointF(-3.5, -3.5), c + QPointF(3.5, 3.5));
    painter.drawLine(c + QPointF(3.5, -3.5), c + QPointF(-3.5, 3.5));
    painter.restore();
}

void SidebarDelegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    auto const& palette = option.palette;
    auto kind = row_kind(index);
    auto hovered = option.state & QStyle::State_MouseOver;
    auto is_current = index.data(IsCurrentRole).toBool();
    auto compact = option.rect.width() < COMPACT_WIDTH;
    auto text_color = ChromeStyle::chrome_text(palette);
    auto muted = ChromeStyle::chrome_muted_text(palette);

    auto row = QRectF(option.rect).adjusted(0, 1, -2, -1);

    if (kind == RowKind::NewTab) {
        // Divider line, then a "+ New Tab" button-row.
        auto divider_y = row.top() + 6;
        painter->setPen(QPen(ChromeStyle::chrome_separator(palette), 1));
        painter->drawLine(QPointF(row.left() + 6, divider_y), QPointF(row.right() - 6, divider_y));

        auto button = row.adjusted(0, 12, 0, 0);
        if (hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(ChromeStyle::chrome_surface_hover(palette));
            painter->drawRoundedRect(button, ROW_RADIUS, ROW_RADIUS);
        }
        painter->setPen(QPen(muted, 1.6, Qt::SolidLine, Qt::RoundCap));
        auto plus_center = QPointF(button.left() + 18, button.center().y());
        painter->drawLine(plus_center + QPointF(-5, 0), plus_center + QPointF(5, 0));
        painter->drawLine(plus_center + QPointF(0, -5), plus_center + QPointF(0, 5));
        if (!compact) {
            auto font = option.font;
            font.setPixelSize(FONT_PIXEL_SIZE);
            font.setWeight(QFont::Medium);
            painter->setFont(font);
            painter->setPen(muted);
            painter->drawText(button.adjusted(36, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, "New Tab");
        }
        painter->restore();
        return;
    }

    // Row background: the current tab gets a raised "card", hover a soft fill.
    if (is_current) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(ChromeStyle::is_dark(palette) ? QColor(255, 255, 255, 38) : QColor(255, 255, 255, 230));
        painter->drawRoundedRect(row, ROW_RADIUS, ROW_RADIUS);
    } else if (hovered) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(ChromeStyle::is_dark(palette) ? QColor(255, 255, 255, 16) : QColor(255, 255, 255, 120));
        painter->drawRoundedRect(row, ROW_RADIUS, ROW_RADIUS);
    }

    auto x = row.left() + 10;
    auto icon_rect = QRectF(x, row.center().y() - ICON_SIZE / 2.0, ICON_SIZE, ICON_SIZE);
    if (compact)
        icon_rect.moveLeft(row.center().x() - ICON_SIZE / 2.0);

    if (kind == RowKind::Folder) {
        auto expanded = option.state & QStyle::State_Open;
        // Filled folder symbol, the same size as tab icons. Falls back to a drawn outline off macOS.
        auto device_pixel_ratio = option.widget ? option.widget->devicePixelRatioF() : 2.0;
        auto symbol = fitted_symbol_pixmap("folder", ICON_SIZE, text_color, device_pixel_ratio);
        if (!symbol.isNull()) {
            auto logical = symbol.deviceIndependentSize();
            painter->drawPixmap(QPointF(icon_rect.center().x() - logical.width() / 2.0, icon_rect.center().y() - logical.height() / 2.0), symbol);
        } else {
            auto folder_rect = QRectF(icon_rect.center().x() - FOLDER_ICON_SIZE / 2.0, icon_rect.center().y() - FOLDER_ICON_SIZE / 2.0, FOLDER_ICON_SIZE, FOLDER_ICON_SIZE);
            draw_folder_glyph(*painter, folder_rect, text_color, expanded);
        }
        if (!compact) {
            auto chevron_rect = QRectF(row.right() - 22, row.center().y() - 6, 12, 12);
            if (hovered)
                draw_chevron(*painter, chevron_rect, expanded, muted);
        }
    } else {
        auto icon = index.data(Qt::DecorationRole).value<QIcon>();
        auto loaded = kind == RowKind::TodayTab || index.data(IsLoadedRole).toBool();
        if (!icon.isNull()) {
            painter->setOpacity(loaded ? 1.0 : 0.55);
            icon.paint(painter, icon_rect.toRect());
            painter->setOpacity(1.0);
        }
    }

    if (!compact) {
        auto loaded = kind != RowKind::PinnedTab || index.data(IsLoadedRole).toBool();
        auto show_close = hovered && kind != RowKind::Folder && loaded;
        auto text_right = show_close ? row.right() - 32 : row.right() - (kind == RowKind::Folder ? 28 : 8);
        auto text_rect = QRectF(icon_rect.right() + 10, row.top(), text_right - icon_rect.right() - 10, row.height());

        auto font = option.font;
        font.setPixelSize(FONT_PIXEL_SIZE);
        font.setWeight(kind == RowKind::Folder ? QFont::Medium : QFont::Normal);
        painter->setFont(font);
        painter->setPen(kind == RowKind::PinnedTab && !loaded ? muted : text_color);
        auto text = QFontMetrics(font).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, static_cast<int>(text_rect.width()));
        painter->drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft, text);

        if (show_close) {
            auto close_rect = close_button_rect(option.rect);
            auto* view = qobject_cast<QAbstractItemView const*>(option.widget);
            auto close_hovered = view && close_rect.contains(view->viewport()->mapFromGlobal(QCursor::pos()));
            draw_close_glyph(*painter, close_rect, muted, close_hovered);
        }
    }

    painter->restore();
}

// --- View -------------------------------------------------------------------------------------

SidebarTree::SidebarTree(QWidget* parent)
    : QTreeView(parent)
    , m_delegate(new SidebarDelegate(this))
{
    setItemDelegate(m_delegate);
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(16);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setExpandsOnDoubleClick(false);
    setAnimated(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setAttribute(Qt::WA_MacShowFocusRect, false);

    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);

    setStyleSheet("QTreeView { background: transparent; border: none; outline: none; }"
                  "QTreeView::item { border: none; }");
    viewport()->setAutoFillBackground(false);
}

void SidebarTree::mousePressEvent(QMouseEvent* event)
{
    m_pressed_index = indexAt(event->position().toPoint());
    m_pressed_close = false;
    if (m_pressed_index.isValid() && event->button() == Qt::LeftButton) {
        auto kind = row_kind(m_pressed_index);
        auto loaded = kind == RowKind::TodayTab || (kind == RowKind::PinnedTab && m_pressed_index.data(IsLoadedRole).toBool());
        if (loaded && SidebarDelegate::close_button_rect(visualRect(m_pressed_index)).contains(event->position().toPoint())) {
            m_pressed_close = true;
            event->accept();
            return;
        }
    }
    QTreeView::mousePressEvent(event);
}

void SidebarTree::mouseReleaseEvent(QMouseEvent* event)
{
    auto index = indexAt(event->position().toPoint());
    if (m_pressed_close) {
        m_pressed_close = false;
        if (index.isValid() && index == m_pressed_index)
            emit close_clicked(index);
        event->accept();
        return;
    }
    QTreeView::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && index.isValid() && index == m_pressed_index)
        emit row_clicked(index);
}

SidebarTree::DropPlace SidebarTree::drop_place_for(QModelIndex const& target) const
{
    if (!target.isValid())
        return DropPlace::End;
    switch (dropIndicatorPosition()) {
    case QAbstractItemView::AboveItem:
        return DropPlace::Above;
    case QAbstractItemView::BelowItem:
        return DropPlace::Below;
    case QAbstractItemView::OnItem:
        return row_kind(target) == RowKind::Folder ? DropPlace::Into : DropPlace::Below;
    case QAbstractItemView::OnViewport:
        return DropPlace::End;
    }
    return DropPlace::End;
}

void SidebarTree::dragMoveEvent(QDragMoveEvent* event)
{
    QTreeView::dragMoveEvent(event);
    if (event->source() == this)
        event->setDropAction(Qt::CopyAction); // Never let Qt remove the dragged row; we rebuild instead.
    else if (event->mimeData()->hasUrls())
        event->setDropAction(Qt::CopyAction);
    event->accept();
}

void SidebarTree::dropEvent(QDropEvent* event)
{
    auto target = indexAt(event->position().toPoint());
    auto place = drop_place_for(target);

    // Reset the view's drag state ourselves, since the base implementation (which would edit the model) is skipped.
    setState(QAbstractItemView::NoState);
    stopAutoScroll();
    viewport()->update();

    if (event->source() == this) {
        auto source = currentIndex();
        if (source.isValid() && source != target)
            emit row_dropped(source, target, place);
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }

    if (event->mimeData()->hasUrls()) {
        emit urls_dropped(event->mimeData()->urls(), target, place);
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }

    event->ignore();
}

}
