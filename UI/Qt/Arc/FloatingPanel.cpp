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
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QWindow>

#include <cmath>

namespace Ladybird::Arc {

// --- TrafficLights ----------------------------------------------------------------------------


TrafficLights::TrafficLights(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFixedSize(sizeHint());
}

QSize TrafficLights::sizeHint() const
{
    return { static_cast<int>(std::ceil(m_pitch * 2 + m_diameter)) + 2, static_cast<int>(std::ceil(m_diameter)) + 4 };
}

void TrafficLights::set_metrics(qreal diameter, qreal pitch)
{
    if (diameter <= 0 || pitch <= 0)
        return;
    m_diameter = diameter;
    m_pitch = pitch;
    setFixedSize(sizeHint());
    update();
}

void TrafficLights::set_drawn(bool drawn)
{
    m_drawn = drawn;
    setAttribute(Qt::WA_TransparentForMouseEvents, !drawn);
    update();
}

QRectF TrafficLights::button_rect(int index) const
{
    return QRectF(1 + index * m_pitch, (height() - m_diameter) / 2.0, m_diameter, m_diameter);
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

    // macOS colors under the pointer; grey otherwise, as Arc shows them (and as the real ones are, see ArcChrome.cpp).
    static QColor const colors[] = { QColor(0xff, 0x5f, 0x57), QColor(0xfe, 0xbc, 0x2e), QColor(0x28, 0xc8, 0x40) };
    static QColor const edges[] = { QColor(0xe1, 0x46, 0x40), QColor(0xdf, 0xa1, 0x23), QColor(0x1a, 0xab, 0x29) };
    auto active = m_hovered;
    auto dark = ChromeStyle::is_dark(palette());
    auto inactive = dark ? QColor(255, 255, 255, 26) : QColor(0, 0, 0, 26); // As the real ones (see MacWindow.mm).

    for (int i = 0; i < 3; ++i) {
        auto rect = button_rect(i);
        auto fill = active ? colors[i] : inactive;
        if (m_pressed == i)
            fill = fill.darker(120);
        if (active) {
            painter.setPen(QPen(edges[i], 0.6));
        } else {
            // A rim that catches the light at the top and bottom, as AppKit's inactive buttons have.
            auto tint = dark ? QColor(255, 255, 255) : QColor(0, 0, 0);
            QLinearGradient rim(rect.topLeft(), rect.bottomLeft());
            auto edge = tint, side = tint;
            edge.setAlphaF(dark ? 0.19 : 0.14);
            side.setAlphaF(dark ? 0.03 : 0.04);
            rim.setColorAt(0, edge);
            rim.setColorAt(0.5, side);
            rim.setColorAt(1, edge);
            painter.setPen(QPen(QBrush(rim), 1));
        }
        painter.setBrush(fill);
        painter.drawEllipse(rect.adjusted(0.5, 0.5, -0.5, -0.5));

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

void FloatingSidebarHost::set_card(bool card)
{
    if (m_card == card)
        return;
    m_card = card;
    update();
}

void FloatingSidebarHost::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    auto card = QRectF(card_rect());

    if (!m_card) {
        painter.fillRect(card, ChromeStyle::chrome_background(palette()));
        return;
    }

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
