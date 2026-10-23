/*
 * Pieces of the Arc sidebar's hidden mode: the floating host that slides over the page, and the
 * traffic-light window controls drawn inside it.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/FloatingPanel.h>
#include <UI/Qt/ChromeStyle.h>

#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWindow>

namespace Ladybird::Arc {

// --- TrafficLights ----------------------------------------------------------------------------

static constexpr qreal LIGHT_DIAMETER = 12;
static constexpr qreal LIGHT_PITCH = 20;

TrafficLights::TrafficLights(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFixedSize(sizeHint());
}

QSize TrafficLights::sizeHint() const
{
    return { static_cast<int>(LIGHT_PITCH * 2 + LIGHT_DIAMETER) + 2, static_cast<int>(LIGHT_DIAMETER) + 4 };
}

void TrafficLights::set_drawn(bool drawn)
{
    m_drawn = drawn;
    setAttribute(Qt::WA_TransparentForMouseEvents, !drawn);
    update();
}

QRectF TrafficLights::button_rect(int index) const
{
    return QRectF(1 + index * LIGHT_PITCH, (height() - LIGHT_DIAMETER) / 2.0, LIGHT_DIAMETER, LIGHT_DIAMETER);
}

int TrafficLights::button_at(QPoint position) const
{
    for (int i = 0; i < 3; ++i) {
        if (button_rect(i).adjusted(-2, -2, 2, 2).contains(position))
            return i;
    }
    return -1;
}

void TrafficLights::paintEvent(QPaintEvent*)
{
    if (!m_drawn)
        return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // macOS colors; grey when the window isn't active, unless hovered (as AppKit does).
    static QColor const colors[] = { QColor(0xff, 0x5f, 0x57), QColor(0xfe, 0xbc, 0x2e), QColor(0x28, 0xc8, 0x40) };
    static QColor const edges[] = { QColor(0xe1, 0x46, 0x40), QColor(0xdf, 0xa1, 0x23), QColor(0x1a, 0xab, 0x29) };
    // Colored while the browser window is active (the floating panel itself never takes focus).
    auto* target = property("arcBrowserWindow").value<QWidget*>();
    auto active = (target ? target->isActiveWindow() : window()->isActiveWindow()) || m_hovered;
    auto dark = ChromeStyle::is_dark(palette());
    auto inactive = dark ? QColor(0x55, 0x58, 0x5e) : QColor(0xcd, 0xcd, 0xcd);

    for (int i = 0; i < 3; ++i) {
        auto rect = button_rect(i);
        auto fill = active ? colors[i] : inactive;
        if (m_pressed == i)
            fill = fill.darker(120);
        painter.setPen(QPen(active ? edges[i] : inactive.darker(110), 0.6));
        painter.setBrush(fill);
        painter.drawEllipse(rect.adjusted(0.3, 0.3, -0.3, -0.3));

        if (!m_hovered)
            continue;

        // Glyphs: close (x), minimize (-), full screen (two triangles).
        painter.setPen(QPen(QColor(0, 0, 0, 150), 1.1, Qt::SolidLine, Qt::RoundCap));
        auto c = rect.center();
        if (i == 0) {
            painter.drawLine(c + QPointF(-2.6, -2.6), c + QPointF(2.6, 2.6));
            painter.drawLine(c + QPointF(2.6, -2.6), c + QPointF(-2.6, 2.6));
        } else if (i == 1) {
            painter.drawLine(c + QPointF(-3.2, 0), c + QPointF(3.2, 0));
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 150));
            QPainterPath path;
            path.moveTo(c + QPointF(-3.2, -3.2));
            path.lineTo(c + QPointF(1.4, -3.2));
            path.lineTo(c + QPointF(-3.2, 1.4));
            path.closeSubpath();
            path.moveTo(c + QPointF(3.2, 3.2));
            path.lineTo(c + QPointF(-1.4, 3.2));
            path.lineTo(c + QPointF(3.2, -1.4));
            path.closeSubpath();
            painter.drawPath(path);
        }
    }
}

void TrafficLights::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QWidget::enterEvent(event);
}

void TrafficLights::leaveEvent(QEvent* event)
{
    m_hovered = false;
    m_pressed = -1;
    update();
    QWidget::leaveEvent(event);
}

void TrafficLights::mouseMoveEvent(QMouseEvent* event)
{
    QWidget::mouseMoveEvent(event);
}

void TrafficLights::mousePressEvent(QMouseEvent* event)
{
    m_pressed = event->button() == Qt::LeftButton ? button_at(event->position().toPoint()) : -1;
    update();
}

void TrafficLights::mouseReleaseEvent(QMouseEvent* event)
{
    auto button = button_at(event->position().toPoint());
    auto pressed = m_pressed;
    m_pressed = -1;
    update();
    if (button < 0 || button != pressed)
        return;

    // The browser window this panel belongs to (the floating host is a separate native window).
    auto* target = property("arcBrowserWindow").value<QWidget*>();
    if (!target)
        target = window();
    switch (button) {
    case 0:
        target->close();
        break;
    case 1:
        target->showMinimized();
        break;
    case 2:
        target->isFullScreen() ? target->showNormal() : target->showFullScreen();
        break;
    }
}

// --- FloatingSidebarHost ----------------------------------------------------------------------

FloatingSidebarHost::FloatingSidebarHost(QWidget& browser_window)
    : QWidget(&browser_window, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setMouseTracking(true);

    // As upstream's address-bar suggestions do: embed the native window in the browser window so it
    // stacks above the page and moves with the window.
    (void)winId();
    if (auto* parent_handle = browser_window.windowHandle())
        windowHandle()->setParent(parent_handle);
}

void FloatingSidebarHost::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    auto card = QRectF(card_rect());

    // Soft shadow under the card.
    painter.setPen(Qt::NoPen);
    for (int i = SHADOW; i > 0; i -= 2) {
        auto alpha = static_cast<int>(26.0 * (1.0 - static_cast<double>(i) / SHADOW)) + 2;
        painter.setBrush(QColor(0, 0, 0, alpha / 2));
        painter.drawRoundedRect(card.adjusted(-i * 0.6, -i * 0.3, i * 0.6, i * 0.9), RADIUS + i * 0.6, RADIUS + i * 0.6);
    }

    painter.setBrush(ChromeStyle::chrome_background(palette()));
    painter.setPen(QPen(ChromeStyle::is_dark(palette()) ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 34), 1));
    painter.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), RADIUS, RADIUS);
}

void FloatingSidebarHost::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    emit left();
}

}
