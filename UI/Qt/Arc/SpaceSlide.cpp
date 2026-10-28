/*
 * Arc's slide between spaces.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <UI/Qt/Arc/SpaceSlide.h>
#include <UI/Qt/ChromeStyle.h>

#include <QEasingCurve>
#include <QLayout>
#include <QPainter>
#include <QTimer>
#include <QVariantAnimation>

namespace Ladybird::Arc {

static constexpr int SETTLE_MS = 16;

SpaceSlide* SpaceSlide::running_in(QWidget& parent)
{
    for (auto* slide : parent.findChildren<SpaceSlide*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (slide->is_running())
            return slide;
    }
    return nullptr;
}

QPixmap SpaceSlide::picture(QWidget& parent, QRect area)
{
    // The window's background isn't painted by Qt, so a grab leaves it out: put it in first, so the picture is opaque.
    auto* window = parent.window();
    auto dpr = window->devicePixelRatioF();
    QPixmap pixmap(area.size() * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(ChromeStyle::chrome_background(window->palette()));
    auto source = QRect(parent.mapTo(window, area.topLeft()), area.size());
    window->render(&pixmap, QPoint(), QRegion(source), QWidget::DrawWindowBackground | QWidget::DrawChildren);
    return pixmap;
}

QPixmap SpaceSlide::picture_in(QWidget& parent, QRect area, Tint tint, Tint now)
{
    // Everything in the sidebar paints its color from the tint as it paints, so it's enough to change it just for
    // this picture; nothing is restyled.
    ChromeStyle::set_chrome_background_tint(tint.color, tint.amount);
    auto pixmap = picture(parent, area);
    ChromeStyle::set_chrome_background_tint(now.color, now.amount);
    return pixmap;
}

SpaceSlide::Pictures SpaceSlide::pictures(QWidget& parent, QRect area, Tint old_tint, Tint new_tint)
{
    return { picture(parent, area), picture_in(parent, area, new_tint, old_tint) };
}

SpaceSlide* SpaceSlide::start(QWidget& parent, QRect area, Pictures from, Tint old_tint, Tint new_tint, int direction)
{
    if (auto* running = running_in(parent))
        running->finish();
    auto* slide = new SpaceSlide(parent, AK::move(from), old_tint, new_tint, direction);
    slide->setGeometry(area);
    slide->show();
    slide->raise();
    // The new space's favorites and rows settle over the next turns of the event loop (some of its rebuilding is
    // deferred); until then the old space stays put.
    QTimer::singleShot(SETTLE_MS, slide, [slide] { slide->take_new_picture(); });
    return slide;
}

SpaceSlide::SpaceSlide(QWidget& parent, Pictures from, Tint old_tint, Tint new_tint, int direction)
    : QWidget(&parent)
    , m_from(AK::move(from))
    , m_old_tint(AK::move(old_tint))
    , m_new_tint(AK::move(new_tint))
    , m_direction(direction < 0 ? -1 : 1)
{
    setObjectName("ArcSpaceSlide");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    // The pictures are opaque and always fill it between them, so the widgets underneath needn't be hidden.
    setAttribute(Qt::WA_OpaquePaintEvent);

    m_animation = new QVariantAnimation(this);
    m_animation->setDuration(DURATION_MS);
    m_animation->setStartValue(0.0);
    m_animation->setEndValue(1.0);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_progress = value.toReal();
        update();
    });
    connect(m_animation, &QAbstractAnimation::finished, this, &SpaceSlide::finish);
}

bool SpaceSlide::is_running() const
{
    return !m_finished;
}

void SpaceSlide::take_new_picture()
{
    if (m_finished)
        return;
    if (auto* layout = parentWidget()->layout())
        layout->activate();
    // Picture the real widgets, with this slide out of the way just for the grab (nothing paints in between).
    hide();
    m_to = pictures(*parentWidget(), geometry(), m_old_tint, m_new_tint);
    show();
    m_animation->start();
    emit started();
}

void SpaceSlide::set_color_progress(qreal progress)
{
    progress = qBound<qreal>(0, progress, 1);
    if (progress == m_color_progress)
        return;
    m_color_progress = progress;
    update();
}

void SpaceSlide::finish()
{
    if (m_finished)
        return;
    m_finished = true;
    m_animation->stop();
    hide();
    emit finished();
    deleteLater();
}

void SpaceSlide::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setClipRect(rect());
    // Side by side, as one strip moving across.
    // Whole device pixels, so the two meet without a seam.
    auto dpr = devicePixelRatioF();
    auto offset = qRound(width() * m_progress * m_direction * dpr) / dpr;
    auto draw = [&](QPixmap const& from, QPixmap const& to) {
        painter.drawPixmap(QPointF(-offset, 0), from);
        if (!to.isNull())
            painter.drawPixmap(QPointF(width() * m_direction - offset, 0), to);
    };
    // The old color, faded into the new as the window is. (Painting over a background mixes linearly, so this is
    // what painting on the color in between would be.)
    if (m_color_progress < 1)
        draw(m_from.in_old_color, m_to.in_old_color);
    if (m_color_progress > 0) {
        painter.setOpacity(m_color_progress);
        draw(m_from.in_new_color, m_to.in_new_color);
    }
}

}
