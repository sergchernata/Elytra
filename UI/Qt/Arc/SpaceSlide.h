/*
 * Arc's slide between spaces: the old space's list moves out to one side as the new one comes in from the other.
 *
 * It slides pictures of the two, taken before and after the switch, over the real widgets until it ends.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QPixmap>
#include <QWidget>

class QVariantAnimation;

namespace Ladybird::Arc {

class SpaceSlide final : public QWidget {
    Q_OBJECT

public:
    static constexpr int DURATION_MS = 260;

    // Slides `from` out over `area` of `parent`, and the new space in: a positive
    // direction brings it in from the right (it's further along the switcher). Its picture is taken a moment later,
    // once the sidebar has laid it out. Any slide already running in `parent` ends first.
    static SpaceSlide* start(QWidget& parent, QRect area, QPixmap from, int direction);
    static SpaceSlide* running_in(QWidget& parent);

    // `area` of `parent` as it shows on screen, with the window's background behind it: pictures with a transparent
    // background render their text thinner and dimmer.
    static QPixmap picture(QWidget& parent, QRect area);

    // Ends the slide now, uncovering the real widgets.
    void finish();

    qreal progress() const { return m_progress; }
    bool is_running() const;
    int direction() const { return m_direction; }

signals:
    void finished();

protected:
    virtual void paintEvent(QPaintEvent*) override;

private:
    SpaceSlide(QWidget& parent, QPixmap from, int direction);
    void take_new_picture();

    QPixmap m_from;
    QPixmap m_to;
    int m_direction { 1 };
    qreal m_progress { 0 };
    QVariantAnimation* m_animation { nullptr };
    bool m_finished { false };
};

}
