/*
 * The list in the Arc sidebar: pinned folders and tabs, the "New Tab" row, then Today's tabs.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/SidebarTree.h>
#include <UI/Qt/Arc/SymbolIcon.h>
#include <UI/Qt/ChromeStyle.h>

#include <QApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QUrl>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QEasingCurve>
#include <QPainterPath>

#include <cmath>

namespace Ladybird::Arc {

static constexpr int ROW_HEIGHT = 36;
static constexpr int DIVIDER_STRIP_HEIGHT = 22;
static constexpr int NEW_TAB_ROW_HEIGHT = DIVIDER_STRIP_HEIGHT + ROW_HEIGHT;
static constexpr int BOTTOM_SPACE_HEIGHT = 44;
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

QRect SidebarDelegate::chevron_rect(QRect const& row)
{
    return close_button_rect(row);
}

QRect SidebarDelegate::divider_strip_rect(QRect const& row)
{
    return QRect(row.left(), row.top(), row.width(), DIVIDER_STRIP_HEIGHT);
}

QRect SidebarDelegate::clear_button_rect(QRect const& row)
{
    auto strip = divider_strip_rect(row);
    return QRect(strip.right() - 64, strip.center().y() - 9, 56, 18);
}

bool pinned_urls_match(QString const& pinned, QString const& current)
{
    if (current.isEmpty())
        return true;
    auto normalized = [](QString const& text) {
        QUrl url(text);
        url.setFragment({});
        url.setHost(url.host().toLower());
        auto path = url.path();
        while (path.endsWith('/'))
            path.chop(1);
        url.setPath(path);
        return url.toString(QUrl::FullyEncoded);
    };
    return normalized(pinned) == normalized(current);
}

QRect SidebarDelegate::pinned_icon_rect(QRect const& row)
{
    // Centered on the favicon (see paint()), with room for a rounded backdrop.
    static constexpr int SIZE = 28;
    auto icon_center = QPoint(row.left() + 10 + ICON_SIZE / 2, row.center().y());
    return QRect(icon_center.x() - SIZE / 2, icon_center.y() - SIZE / 2 + 1, SIZE, SIZE);
}

QString SidebarDelegate::pinned_icon_label(bool separate)
{
    return separate ? "Separate From Pinned Tab" : "Back to Pinned URL";
}

// Arc's pinned-tab state: loaded, on another page than its pinned URL.
static bool is_away_from_pinned_url(QModelIndex const& index)
{
    return row_kind(index) == RowKind::PinnedTab && index.data(IsLoadedRole).toBool() && index.data(NavigatedAwayRole).toBool();
}

SidebarDelegate::PinnedRowState SidebarDelegate::pinned_row_state(QModelIndex const& index, QRect const& row, Optional<QPoint> hover_position, bool shift)
{
    if (!is_away_from_pinned_url(index))
        return PinnedRowState::OnPinnedUrl;
    if (!hover_position.has_value() || !pinned_icon_rect(row).contains(*hover_position))
        return PinnedRowState::Away;
    return shift ? PinnedRowState::SeparateFromPinnedTab : PinnedRowState::BackToPinnedUrl;
}

// Today's tabs follow the divider row.
bool SidebarDelegate::divider_has_today_tabs(QModelIndex const& divider)
{
    auto next = divider.siblingAtRow(divider.row() + 1);
    return next.isValid() && row_kind(next) == RowKind::TodayTab;
}

static bool has_today_tabs(QModelIndex const& divider)
{
    return SidebarDelegate::divider_has_today_tabs(divider);
}

// The soft fill under a hovered row (tabs, folders and the New Tab button alike).
static QColor row_hover_color(QPalette const& palette)
{
    return ChromeStyle::is_dark(palette) ? QColor(255, 255, 255, 16) : QColor(255, 255, 255, 120);
}

static bool cursor_in(QStyleOptionViewItem const& option, QRect const& rect)
{
    auto* view = qobject_cast<QAbstractItemView const*>(option.widget);
    return view && rect.contains(view->viewport()->mapFromGlobal(QCursor::pos()));
}

QSize SidebarDelegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    auto kind = row_kind(index);
    auto height = kind == RowKind::NewTab ? NEW_TAB_ROW_HEIGHT : kind == RowKind::BottomSpace ? BOTTOM_SPACE_HEIGHT : ROW_HEIGHT;
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
    if (kind == RowKind::BottomSpace) {
        painter->restore();
        return;
    }

    if (kind == RowKind::NewTab) {
        // A light rule with a subtle "↓ Clear" at its right end (when there are Today tabs), then the
        // "+ New Tab" button.
        auto strip = QRectF(divider_strip_rect(option.rect));
        auto rule_y = std::floor(strip.center().y()) + 0.5;
        auto rule_right = row.right() - 6;
        if (!compact && has_today_tabs(index)) {
            auto clear = QRectF(clear_button_rect(option.rect));
            rule_right = clear.left() - 4;

            auto clear_color = muted;
            clear_color.setAlphaF(cursor_in(option, clear_button_rect(option.rect)) ? 1.0 : 0.6);
            auto font = option.font;
            font.setPixelSize(12);
            font.setWeight(QFont::DemiBold);
            painter->setFont(font);
            painter->setPen(clear_color);
            auto text_width = QFontMetricsF(font).horizontalAdvance("Clear");
            auto text_left = clear.right() - text_width;
            painter->drawText(QRectF(text_left, clear.top(), text_width + 1, clear.height()), Qt::AlignVCenter | Qt::AlignLeft, "Clear");

            // Down arrow.
            painter->setPen(QPen(clear_color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            auto arrow_x = text_left - 8;
            auto top = clear.center().y() - 4.5;
            auto bottom = clear.center().y() + 4.5;
            painter->drawLine(QPointF(arrow_x, top), QPointF(arrow_x, bottom));
            painter->drawLine(QPointF(arrow_x - 3.5, bottom - 3.5), QPointF(arrow_x, bottom));
            painter->drawLine(QPointF(arrow_x + 3.5, bottom - 3.5), QPointF(arrow_x, bottom));
        }
        // A light rule, as in Arc: brighter than the background in dark mode, darker in light mode.
        painter->setPen(QPen(ChromeStyle::is_dark(palette) ? QColor(255, 255, 255, 46) : QColor(0, 0, 0, 30), 1));
        painter->drawLine(QPointF(row.left() + 6, rule_y), QPointF(rule_right, rule_y));

        auto button = row.adjusted(0, DIVIDER_STRIP_HEIGHT, 0, 0);
        if (hovered && cursor_in(option, button.toRect())) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(row_hover_color(palette));
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
        painter->setBrush(row_hover_color(palette));
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
        if (!compact && hovered) {
            auto button = chevron_rect(option.rect);
            auto* view = qobject_cast<QAbstractItemView const*>(option.widget);
            if (view && button.contains(view->viewport()->mapFromGlobal(QCursor::pos()))) {
                painter->setPen(Qt::NoPen);
                auto background = muted;
                background.setAlpha(40);
                painter->setBrush(background);
                painter->drawRoundedRect(button, 5, 5);
            }
            draw_chevron(*painter, QRectF(button).adjusted(3, 3, -3, -3), expanded, muted);
        }
    } else {
        // An away pinned tab's favicon is a button (back to the pinned URL): a rounded backdrop while hovered.
        auto icon_hover = index == pinned_hover_index ? pinned_hover_progress : 0.0;
        if (!compact && icon_hover > 0) {
            auto backdrop = ChromeStyle::is_dark(palette) ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 18);
            backdrop.setAlphaF(backdrop.alphaF() * icon_hover);
            painter->setPen(Qt::NoPen);
            painter->setBrush(backdrop);
            painter->drawRoundedRect(QRectF(pinned_icon_rect(option.rect)), 7, 7);
        }
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
        auto title = index.data(Qt::DisplayRole).toString();

        // Arc's away pinned tab: "| Title" at rest; hovering the favicon slides the title up and fades in what
        // clicking it does underneath (Shift: separate instead).
        auto progress = index == pinned_hover_index ? pinned_hover_progress : 0.0;
        if (is_away_from_pinned_url(index) || progress > 0) {
            static constexpr qreal LINE_GAP = 4;
            static constexpr int LABEL_PIXEL_SIZE = 11;
            static constexpr qreal SLIDE = 5;
            auto label_font = font;
            label_font.setPixelSize(LABEL_PIXEL_SIZE);
            QFontMetricsF title_metrics(font);
            QFontMetricsF label_metrics(label_font);
            auto block = title_metrics.height() + LINE_GAP + label_metrics.height();
            auto block_top = text_rect.center().y() - block / 2;

            // The bar fades out and the title moves into its place. Drawn rather than typed, so it's crisp and
            // centered on the title's capitals.
            static constexpr qreal BAR_ADVANCE = 9;
            auto title_left = text_rect.left() + BAR_ADVANCE * (1 - progress);
            auto resting_top = text_rect.center().y() - title_metrics.height() / 2;
            auto title_top = resting_top + (block_top - resting_top) * progress;
            if (progress < 1) {
                painter->setOpacity(1 - progress);
                painter->setPen(QPen(muted, 1.5, Qt::SolidLine, Qt::RoundCap));
                auto bar_x = text_rect.left() + 1.5;
                auto bar_center = resting_top + title_metrics.height() / 2;
                auto bar_half = title_metrics.capHeight() / 2 + 2;
                painter->drawLine(QPointF(bar_x, bar_center - bar_half), QPointF(bar_x, bar_center + bar_half));
                painter->setOpacity(1);
            }
            painter->setPen(text_color);
            auto title_width = text_rect.right() - title_left;
            painter->drawText(QRectF(title_left, title_top, title_width, title_metrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                title_metrics.elidedText(title, Qt::ElideRight, title_width));

            if (progress > 0) {
                auto label_top = block_top + title_metrics.height() + LINE_GAP + SLIDE * (1 - progress);
                painter->setOpacity(progress);
                painter->setFont(label_font);
                painter->setPen(muted);
                painter->drawText(QRectF(text_rect.left(), label_top, text_rect.width(), label_metrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                    label_metrics.elidedText(pinned_icon_label(pinned_hover_separate), Qt::ElideRight, text_rect.width()));
                painter->setOpacity(1);
                painter->setFont(font);
            }
        } else {
            auto text = QFontMetrics(font).elidedText(title, Qt::ElideRight, static_cast<int>(text_rect.width()));
            painter->drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft, text);
        }

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

    // No scrollbar track; a thin rounded handle, as in Arc.
    setStyleSheet("QTreeView { background: transparent; border: none; outline: none; }"
                  "QTreeView::item { border: none; }"
                  "QScrollBar:vertical { background: transparent; width: 6px; margin: 2px 0 2px 0; border: none; }"
                  "QScrollBar::handle:vertical { background: rgba(128, 128, 128, 0.45); border-radius: 3px; min-height: 28px; }"
                  "QScrollBar::handle:vertical:hover { background: rgba(128, 128, 128, 0.7); }"
                  "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; border: none; background: none; }"
                  "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }");
    viewport()->setAutoFillBackground(false);

    // Holding Shift over an away pinned tab's favicon changes what it says; the page has focus, so watch app-wide.
    qApp->installEventFilter(this);

    m_pinned_hover_animation = new QVariantAnimation(this);
    m_pinned_hover_animation->setDuration(180);
    m_pinned_hover_animation->setEasingCurve(QEasingCurve::InOutCubic);
    connect(m_pinned_hover_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_delegate->pinned_hover_progress = value.toReal();
        if (m_delegate->pinned_hover_index.isValid())
            viewport()->update(visualRect(m_delegate->pinned_hover_index));
    });
}

// Starts the icon-hover animation of the row under the pointer, or reverses the one showing.
void SidebarTree::update_pinned_hover()
{
    auto position = m_delegate->hover_position;
    auto index = position.has_value() ? indexAt(*position) : QModelIndex();
    auto state = index.isValid() ? SidebarDelegate::pinned_row_state(index, visualRect(index), position, m_delegate->shift_held) : SidebarDelegate::PinnedRowState::OnPinnedUrl;
    auto icon_hovered = state == SidebarDelegate::PinnedRowState::BackToPinnedUrl || state == SidebarDelegate::PinnedRowState::SeparateFromPinnedTab;

    if (icon_hovered) {
        auto separate = state == SidebarDelegate::PinnedRowState::SeparateFromPinnedTab;
        if (m_delegate->pinned_hover_separate != separate) {
            m_delegate->pinned_hover_separate = separate;
            viewport()->update(visualRect(index));
        }
    }

    auto target = icon_hovered ? 1.0 : 0.0;
    if (icon_hovered && m_delegate->pinned_hover_index != QPersistentModelIndex(index)) {
        // Another row: let the old one go at once, then animate this one in.
        if (m_delegate->pinned_hover_index.isValid())
            viewport()->update(visualRect(m_delegate->pinned_hover_index));
        m_delegate->pinned_hover_index = index;
        m_delegate->pinned_hover_progress = 0;
    }
    if (!m_delegate->pinned_hover_index.isValid())
        return;
    auto animating_to = m_pinned_hover_animation->state() == QAbstractAnimation::Running ? m_pinned_hover_animation->endValue().toReal() : m_delegate->pinned_hover_progress;
    if (qFuzzyCompare(animating_to + 1, target + 1))
        return;
    m_pinned_hover_animation->stop();
    m_pinned_hover_animation->setStartValue(m_delegate->pinned_hover_progress);
    m_pinned_hover_animation->setEndValue(target);
    m_pinned_hover_animation->start();
}

bool SidebarTree::eventFilter(QObject* watched, QEvent* event)
{
    if ((event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Shift) {
        m_delegate->shift_held = event->type() == QEvent::KeyPress;
        update_pinned_hover();
    }
    return QTreeView::eventFilter(watched, event);
}

void SidebarTree::mousePressEvent(QMouseEvent* event)
{
    m_pressed_index = indexAt(event->position().toPoint());
    m_pressed_close = false;
    m_pressed_clear = false;
    m_pressed_pinned_icon = false;
    if (m_pressed_index.isValid() && event->button() == Qt::LeftButton) {
        auto kind = row_kind(m_pressed_index);
        auto row = visualRect(m_pressed_index);
        auto point = event->position().toPoint();
        // The rule's strip isn't part of the New Tab button: only Clear responds there.
        if (kind == RowKind::NewTab && SidebarDelegate::divider_strip_rect(row).contains(point)) {
            m_pressed_clear = has_today_tabs(m_pressed_index) && SidebarDelegate::clear_button_rect(row).contains(point);
            m_pressed_index = QPersistentModelIndex();
            event->accept();
            return;
        }
        auto loaded = kind == RowKind::TodayTab || (kind == RowKind::PinnedTab && m_pressed_index.data(IsLoadedRole).toBool());
        if (is_away_from_pinned_url(m_pressed_index) && SidebarDelegate::pinned_icon_rect(row).contains(point)) {
            m_pressed_pinned_icon = true;
            event->accept();
            return;
        }
        if (loaded && SidebarDelegate::close_button_rect(visualRect(m_pressed_index)).contains(event->position().toPoint())) {
            m_pressed_close = true;
            event->accept();
            return;
        }
    }
    QTreeView::mousePressEvent(event);
}

void SidebarTree::mouseMoveEvent(QMouseEvent* event)
{
    QTreeView::mouseMoveEvent(event);
    // Rows only repaint when the hovered row changes; the close and chevron buttons need a repaint as the
    // pointer moves within a row so their hover backgrounds track it.
    auto index = indexAt(event->position().toPoint());
    if (m_hovered_index.isValid() && m_hovered_index != index)
        viewport()->update(visualRect(m_hovered_index));
    if (index.isValid())
        viewport()->update(visualRect(index));
    m_hovered_index = index;
    m_delegate->hover_position = event->position().toPoint();
    m_delegate->shift_held = event->modifiers() & Qt::ShiftModifier;
    update_pinned_hover();
}

void SidebarTree::leaveEvent(QEvent* event)
{
    QTreeView::leaveEvent(event);
    if (m_hovered_index.isValid())
        viewport()->update(visualRect(m_hovered_index));
    m_hovered_index = QPersistentModelIndex();
    m_delegate->hover_position = {};
    update_pinned_hover();
}

void SidebarTree::mouseReleaseEvent(QMouseEvent* event)
{
    auto index = indexAt(event->position().toPoint());
    if (m_pressed_pinned_icon) {
        m_pressed_pinned_icon = false;
        if (index.isValid() && index == m_pressed_index && SidebarDelegate::pinned_icon_rect(visualRect(index)).contains(event->position().toPoint()))
            emit pinned_icon_clicked(index, event->modifiers() & Qt::ShiftModifier);
        event->accept();
        return;
    }
    if (m_pressed_clear) {
        m_pressed_clear = false;
        if (index.isValid() && row_kind(index) == RowKind::NewTab && SidebarDelegate::clear_button_rect(visualRect(index)).contains(event->position().toPoint()))
            emit clear_clicked();
        event->accept();
        return;
    }
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

// As in Arc: double-clicking empty sidebar space opens the command bar.
void SidebarTree::mouseDoubleClickEvent(QMouseEvent* event)
{
    auto index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && (!index.isValid() || row_kind(index) == RowKind::BottomSpace)) {
        emit background_double_clicked();
        event->accept();
        return;
    }
    QTreeView::mouseDoubleClickEvent(event);
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
