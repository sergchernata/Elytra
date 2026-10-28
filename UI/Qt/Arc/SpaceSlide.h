/*
 * Arc's slide between spaces: the old space's list moves out to one side as the new one comes in from the other.
 *
 * It slides pictures of the two, taken before and after the switch, over the real widgets until it ends. As in Arc, the
 * window fades to the new space's color meanwhile: each picture is taken in both colors, and the slide fades from the
 * one to the other in step with the window (set_color_progress()).
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

    // The chrome's tint (see ChromeStyle::set_chrome_background_tint()).
    struct Tint {
        QColor color;
        double amount { 0 };
    };
    struct Pictures {
        QPixmap in_old_color;
        QPixmap in_new_color;
    };

    // Slides `from` out over `area` of `parent`, and the new space in: a positive
    // direction brings it in from the right (it's further along the switcher). Its pictures are taken a moment later,
    // once the sidebar has laid it out, in the `old` tint (the one now) and the `new` one. Any slide already running in
    // `parent` ends first.
    static SpaceSlide* start(QWidget& parent, QRect area, Pictures from, Tint old_tint, Tint new_tint, int direction);
    static SpaceSlide* running_in(QWidget& parent);

    // `area` of `parent` as it shows on screen, with the window's background behind it: pictures with a transparent
    // background render their text thinner and dimmer.
    static QPixmap picture(QWidget& parent, QRect area);
    // The same, painted as it would be in `tint` (the window's tint stays `now`).
    static QPixmap picture_in(QWidget& parent, QRect area, Tint tint, Tint now);
    static Pictures pictures(QWidget& parent, QRect area, Tint old_tint, Tint new_tint);

    // How far the window has faded from the old space's color to the new one's.
    void set_color_progress(qreal);
    qreal color_progress() const { return m_color_progress; }

    // Ends the slide now, uncovering the real widgets.
    void finish();

    qreal progress() const { return m_progress; }
    bool is_running() const;
    int direction() const { return m_direction; }

signals:
    void started(); // Moving, now that the new space's pictures are taken.
    void finished();

protected:
    virtual void paintEvent(QPaintEvent*) override;

private:
    SpaceSlide(QWidget& parent, Pictures from, Tint old_tint, Tint new_tint, int direction);
    void take_new_picture();

    Pictures m_from;
    Pictures m_to;
    Tint m_old_tint;
    Tint m_new_tint;
    int m_direction { 1 };
    qreal m_progress { 0 };
    qreal m_color_progress { 0 };
    QVariantAnimation* m_animation { nullptr };
    bool m_finished { false };
};

}
