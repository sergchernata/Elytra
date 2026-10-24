/*
 * Arc's media player at the bottom of the sidebar, for a tab playing out of sight: the site's icon (back to the tab),
 * previous, play or pause, next, and mute. Hovering it slides it up to show the title, with buttons to watch the video
 * in Picture-in-Picture again, and to stop it.
 *
 * It sits in the sidebar's bottom bar, above the space buttons, and the bar grows with it as it slides open (the list
 * above gives way), as in Arc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QAbstractButton>
#include <QIcon>
#include <QWidget>

class QVariantAnimation;

namespace Ladybird::Arc {

class PlayerButton final : public QAbstractButton {
    Q_OBJECT

public:
    explicit PlayerButton(QWidget* parent, int size);

    void set_symbol(QString const&);
    void set_picture(QIcon const&);
    QString const& symbol() const { return m_symbol; }

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void enterEvent(QEnterEvent*) override;
    virtual void leaveEvent(QEvent*) override;

private:
    QString m_symbol;
    QIcon m_picture;
    bool m_hovered { false };
};

class MiniPlayer final : public QWidget {
    Q_OBJECT

public:
    static constexpr int CONTROLS_HEIGHT = 40;
    static constexpr int TITLE_HEIGHT = 34;
    static constexpr int REVEAL_MS = 200;

    explicit MiniPlayer(QWidget* parent);

    void set_title(QString const&);
    void set_icon(QIcon const&);
    void set_playing(bool);
    void set_muted(bool);
    // Whether the page has a video to show in Picture-in-Picture.
    void set_can_picture_in_picture(bool);

    // The slot it sits in, in its parent's coordinates; it keeps to the slot's bottom, and asks for the height it needs
    // (height_changed()).
    void set_slot(QRect);

    qreal reveal() const { return m_reveal; }
    bool is_playing() const { return m_playing; }
    bool is_muted() const { return m_muted; }
    QString title() const { return m_title; }

    PlayerButton* icon_button() const { return m_icon_button; }
    PlayerButton* play_button() const { return m_play_button; }
    PlayerButton* mute_button() const { return m_mute_button; }
    PlayerButton* picture_in_picture_button() const { return m_picture_in_picture_button; }
    PlayerButton* close_button() const { return m_close_button; }

    // Slides the title row in or out, as the pointer arriving or leaving does.
    void set_revealed(bool);

signals:
    void go_to_tab();
    void toggle_playback();
    void toggle_mute();
    void picture_in_picture();
    void dismiss();
    // The height it takes as it slides open or closed: the sidebar grows its slot to match, as Arc's bottom bar grows.
    void height_changed(int);

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void enterEvent(QEnterEvent*) override;
    virtual void leaveEvent(QEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;

private:
    void set_reveal(qreal);
    void place();
    void lay_out_children();

    QRect m_slot;
    int m_requested_height { CONTROLS_HEIGHT };
    qreal m_reveal { 0 };
    QVariantAnimation* m_reveal_animation { nullptr };
    QString m_title;
    bool m_playing { false };
    bool m_muted { false };

    PlayerButton* m_icon_button { nullptr };
    PlayerButton* m_previous_button { nullptr };
    PlayerButton* m_play_button { nullptr };
    PlayerButton* m_next_button { nullptr };
    PlayerButton* m_mute_button { nullptr };
    PlayerButton* m_picture_in_picture_button { nullptr };
    PlayerButton* m_close_button { nullptr };
};

}
