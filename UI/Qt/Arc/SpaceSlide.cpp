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

SpaceSlide* SpaceSlide::start(QWidget& parent, QRect area, QPixmap from, int direction)
{
    if (auto* running = running_in(parent))
        running->finish();
    auto* slide = new SpaceSlide(parent, AK::move(from), direction);
    slide->setGeometry(area);
    slide->show();
    slide->raise();
    // The new space's favorites and rows settle over the next turns of the event loop (some of its rebuilding is
    // deferred); until then the old space stays put.
    QTimer::singleShot(SETTLE_MS, slide, [slide] { slide->take_new_picture(); });
    return slide;
}

SpaceSlide::SpaceSlide(QWidget& parent, QPixmap from, int direction)
    : QWidget(&parent)
    , m_from(AK::move(from))
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
    m_to = picture(*parentWidget(), geometry());
    show();
    m_animation->start();
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
    painter.drawPixmap(QPointF(-offset, 0), m_from);
    if (!m_to.isNull())
        painter.drawPixmap(QPointF(width() * m_direction - offset, 0), m_to);
}

}
