/*
 * Arc's update notice.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/UpdatePill.h>
#include <UI/Qt/ChromeStyle.h>

#include <QEasingCurve>
#include <QEvent>
#include <QPainter>
#include <QVariantAnimation>
#include <QtMath>

namespace Ladybird::Arc {

static constexpr qreal SLIDE_DISTANCE = 14;
static constexpr int MARGIN_SIDE = 2;
static constexpr int MARGIN_BOTTOM = 6;

UpdatePillMotion update_pill_motion(qreal progress)
{
    progress = qBound<qreal>(0, progress, 1);
    // Room is made first, so the pill has somewhere to rise into; it fades in a little behind its rise.
    auto room = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(qMin<qreal>(1, progress * 1.4));
    auto rise = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(progress);
    auto fade = QEasingCurve(QEasingCurve::InOutQuad).valueForProgress(progress);
    return { SLIDE_DISTANCE * (1 - rise), fade, room };
}

UpdatePill::UpdatePill(QWidget* parent)
    : QAbstractButton(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    auto pill_font = font();
    pill_font.setPixelSize(13);
    pill_font.setWeight(QFont::Medium);
    setFont(pill_font);
    setFixedHeight(0);
    hide();

    m_animation = new QVariantAnimation(this);
    m_animation->setEasingCurve(QEasingCurve::Linear); // update_pill_motion() eases each part its own way.
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) { set_reveal(value.toReal()); });
    connect(m_animation, &QVariantAnimation::finished, this, [this] {
        if (!m_shown)
            hide();
    });
}

void UpdatePill::set_text(QString const& text)
{
    if (this->text() == text)
        return;
    setText(text);
    update();
}

void UpdatePill::set_shown(bool shown, Animate animate)
{
    if (m_shown == shown && (animate == Animate::Yes || m_animation->state() != QAbstractAnimation::Running))
        return;
    m_shown = shown;
    m_animation->stop();
    if (shown)
        show();

    auto target = shown ? 1.0 : 0.0;
    if (animate == Animate::No || !isVisibleTo(parentWidget()) || !window()->isVisible()) {
        set_reveal(target);
        if (!shown)
            hide();
        return;
    }
    // Coming in, the whole motion; going, the same played back, quicker.
    auto distance = qAbs(target - m_reveal);
    m_animation->setDuration(qMax(1, qRound((shown ? SHOW_MS : HIDE_MS) * distance)));
    m_animation->setStartValue(m_reveal);
    m_animation->setEndValue(target);
    m_animation->start();
}

void UpdatePill::set_reveal(qreal reveal)
{
    m_reveal = reveal;
    setFixedHeight(qRound(HEIGHT * update_pill_motion(reveal).height));
    update();
}

QSize UpdatePill::sizeHint() const
{
    return { 200, height() };
}

void UpdatePill::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void UpdatePill::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void UpdatePill::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update();
    QAbstractButton::changeEvent(event);
}

void UpdatePill::paintEvent(QPaintEvent*)
{
    auto motion = update_pill_motion(m_reveal);
    if (motion.opacity <= 0)
        return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Laid out as at full height, lower by the slide, and cut off at the bottom by the room made so far.
    auto pill = QRectF(MARGIN_SIDE, motion.offset, width() - MARGIN_SIDE * 2, HEIGHT - MARGIN_BOTTOM).adjusted(0.5, 0.5, -0.5, -0.5);
    auto dark = ChromeStyle::is_dark(palette());
    auto with_opacity = [&](QColor color) {
        color.setAlphaF(color.alphaF() * motion.opacity);
        return color;
    };
    auto pressed = isDown();
    QColor fill = dark ? QColor(255, 255, 255, m_hovered || pressed ? 41 : 26) : QColor(255, 255, 255, m_hovered || pressed ? 204 : 140);
    QColor border = dark ? QColor(255, 255, 255, 41) : QColor(0, 0, 0, 26);
    QColor ink = dark ? QColor(255, 255, 255, 235) : QColor(0, 0, 0, 204);

    auto radius = pill.height() / 2;
    painter.setPen(QPen(with_opacity(border), 1));
    painter.setBrush(with_opacity(fill));
    painter.drawRoundedRect(pill, radius, radius);
    painter.setPen(with_opacity(ink));
    painter.drawText(pill, Qt::AlignCenter, text());
}

}
