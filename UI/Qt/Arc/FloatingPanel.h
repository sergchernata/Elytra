/*
 * Pieces of the Arc sidebar's hidden mode: the floating host that slides over the page, and the
 * traffic-light window controls drawn inside it.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QPointer>
#include <QWidget>

namespace Ladybird::Arc {

// Close / minimize / full-screen buttons drawn like macOS's, for the slid-out sidebar. The real ones
// live in the window's title bar and can't move with the panel, so they're hidden while it's in use.
class TrafficLights final : public QWidget {
    Q_OBJECT

public:
    explicit TrafficLights(QWidget* parent);

    virtual QSize sizeHint() const override;

    // Docked, the real window controls show over this spot, so draw nothing and let clicks through.
    void set_drawn(bool);
    // Size and spacing, taken from the real controls so the two can be swapped without a visible change.
    void set_metrics(qreal diameter, qreal pitch);
    qreal diameter() const { return m_diameter; }

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void enterEvent(QEnterEvent*) override;
    virtual void leaveEvent(QEvent*) override;

private:
    int button_at(QPoint) const;
    QRectF button_rect(int index) const;

    bool m_hovered { false };
    bool m_drawn { true };
    qreal m_diameter { 12 };
    qreal m_pitch { 20 };
    int m_pressed { -1 };
};

// A native child window of the browser window, so it stacks above the page (which is a native layer
// drawn over everything Qt paints). Draws the floating card; the sidebar's panel is placed inside it.
class FloatingSidebarHost final : public QWidget {
    Q_OBJECT

public:
    static constexpr int SHADOW = 14;
    static constexpr int RADIUS = 12;

    explicit FloatingSidebarHost(QWidget& browser_window);

    QRect card_rect() const { return rect().adjusted(SHADOW, SHADOW, -SHADOW, -SHADOW); }

    // Card: rounded, bordered and shadowed (hover slide-out). Flat: looks exactly like the docked sidebar,
    // for the hide/show animations, so nothing pops when the panel changes hands.
    void set_card(bool);

signals:
    void left();

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void leaveEvent(QEvent*) override;

private:
    bool m_card { true };
};

}
