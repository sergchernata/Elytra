/*
 * Arc's update notice: a pill above the sidebar's bottom bar once a newer release is ready. It slides up into place
 * as it fades in, making room for itself as it comes, and goes the same way.
 *
 * The fade is painted (its colors carry the opacity), not an opacity effect, which would darken the see-through
 * sidebar around it on macOS.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <QAbstractButton>

class QVariantAnimation;

namespace Ladybird::Arc {

// Where the pill is at `progress` (0 to 1, eased) of coming in: how far below its place (pixels), how solid, and how
// much of its height it takes (0 to 1).
struct UpdatePillMotion {
    qreal offset { 0 };
    qreal opacity { 0 };
    qreal height { 0 };
};
UpdatePillMotion update_pill_motion(qreal progress);

class UpdatePill final : public QAbstractButton {
    Q_OBJECT

public:
    static constexpr int HEIGHT = 40;
    static constexpr int SHOW_MS = 320;
    static constexpr int HIDE_MS = 200;

    explicit UpdatePill(QWidget* parent);

    void set_text(QString const&);

    enum class Animate : u8 {
        No,
        Yes,
    };
    void set_shown(bool, Animate = Animate::Yes);
    bool is_shown() const { return m_shown; }

    // How far in it is: 0 gone, 1 in place.
    qreal reveal() const { return m_reveal; }

    virtual QSize sizeHint() const override;

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void enterEvent(QEnterEvent*) override;
    virtual void leaveEvent(QEvent*) override;
    virtual void changeEvent(QEvent*) override;

private:
    void set_reveal(qreal);

    QVariantAnimation* m_animation { nullptr };
    qreal m_reveal { 0 };
    bool m_shown { false };
    bool m_hovered { false };
};

}
