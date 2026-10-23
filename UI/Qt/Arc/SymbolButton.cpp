/*
 * A square button that draws one SF Symbol (or a letter) exactly in its center, as in Arc's bottom bar.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/SymbolButton.h>
#include <UI/Qt/Arc/SymbolIcon.h>
#include <UI/Qt/ChromeStyle.h>

#include <QEnterEvent>
#include <QPainter>

namespace Ladybird::Arc {

// --- SymbolButton -----------------------------------------------------------------------------

SymbolButton::SymbolButton(QWidget* parent)
    : QAbstractButton(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    setFixedSize(sizeHint());
}

void SymbolButton::set_icon_text(QString const& icon)
{
    if (is_symbol_icon(icon)) {
        set_symbol(symbol_name(icon));
        return;
    }
    m_symbol.clear();
    m_text = icon;
    update();
}

void SymbolButton::set_symbol(QString const& name)
{
    m_symbol = name;
    m_text.clear();
    update();
}

void SymbolButton::set_dimmed(bool dimmed)
{
    m_dimmed = dimmed;
    update();
}

void SymbolButton::set_symbol_point_size(int size)
{
    m_point_size = size;
    update();
}

void SymbolButton::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void SymbolButton::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void SymbolButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    auto dark = ChromeStyle::is_dark(palette());

    if (m_hovered || isDown() || isChecked()) {
        painter.setPen(Qt::NoPen);
        auto alpha = isDown() ? 40 : (isChecked() ? 34 : 24);
        painter.setBrush(dark ? QColor(255, 255, 255, alpha) : QColor(0, 0, 0, alpha / 2));
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
    }

    auto color = ChromeStyle::chrome_text(palette());
    painter.setOpacity(m_dimmed && !m_hovered ? 0.42 : 0.92);

    if (!m_symbol.isEmpty()) {
        auto pixmap = fitted_symbol_pixmap(m_symbol, m_point_size, color, devicePixelRatioF());
        if (!pixmap.isNull()) {
            auto logical = pixmap.deviceIndependentSize();
            auto top_left = QPointF((width() - logical.width()) / 2.0, (height() - logical.height()) / 2.0);
            painter.drawPixmap(top_left, pixmap);
            return;
        }
    }

    // No symbol (or not on macOS): a centered letter or emoji.
    auto text = m_text.isEmpty() ? m_symbol.left(1).toUpper() : m_text;
    auto font = painter.font();
    font.setPixelSize(m_point_size - 1);
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(rect(), Qt::AlignCenter, text);
}

}
