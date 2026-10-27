/*
 * Arc's favorites: a grid of four tiles to a row above the space's list.
 *
 * Each tile is backed, as in Arc, by its own favicon blown up and blurred into a wash of color, under a see-through
 * white veil that is lighter at the middle, so the color shows most at the rim.
 *
 * A tile dragged along the grid lifts and follows the pointer while the others slide out of its way; let go, it
 * settles into the gap. A tab or link dragged over the grid from elsewhere opens a gap where it would land.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QHash>
#include <QImage>
#include <QPointer>
#include <QToolButton>

class QPropertyAnimation;
class QVariantAnimation;

namespace Ladybird::Arc {

class FavoriteTile final : public QToolButton {
    Q_OBJECT

public:
    explicit FavoriteTile(QWidget* parent);

    void set_dark(bool);
    // How lifted the tile is, 0 to 1: lifted, it's brighter and casts a shadow, as while dragged.
    void set_lifted(bool);
    qreal lift() const { return m_lift; }

    // The blurred wash of the tile's favicon, at `size` (device pixels). Empty when there's no icon.
    static QImage backdrop_for(QIcon const&, QSize);

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void enterEvent(QEnterEvent*) override;
    virtual void leaveEvent(QEvent*) override;
    virtual void checkStateSet() override;
    virtual void nextCheckState() override;

private:
    void animate_emphasis();
    qreal target_emphasis() const;

    bool m_dark { false };
    bool m_hovered { false };
    qreal m_emphasis { 0 }; // 0 at rest, 0.5 hovered, 1 checked: how much color and light the tile shows.
    qreal m_lift { 0 };
    QVariantAnimation* m_emphasis_animation { nullptr };
    QVariantAnimation* m_lift_animation { nullptr };

    qint64 m_backdrop_key { 0 };
    QSize m_backdrop_size;
    QImage m_backdrop;
};

class FavoritesGrid final : public QWidget {
    Q_OBJECT

public:
    static constexpr int COLUMNS = 4;
    static constexpr int SPACING = 8;
    static constexpr int BOTTOM_MARGIN = 6;
    static constexpr int MOVE_MS = 220;

    FavoritesGrid(int tile_height, QWidget* parent);

    // The tiles to show, in order. Tiles already shown keep their place until they glide to their new one; new ones
    // show in place at once. Tiles left out are no longer the grid's.
    void set_tiles(QList<FavoriteTile*> const&);
    QList<FavoriteTile*> tiles() const;

    QRect slot_rect(int slot) const;
    // Where in the order something dropped at `position` would go.
    int insertion_index_at(QPoint position) const;
    // A drag from outside the grid hovering at `position`: the tiles from its landing place on move along one.
    void show_insertion_gap(QPoint position);
    enum class Animate {
        No,
        Yes,
    };
    // Animate::No leaves the tiles where they are, for a new tile about to fill the gap.
    void hide_insertion_gap(Animate);
    // Every tile glides to its place.
    void settle() { place_tiles(Animate::Yes); }

    bool is_dragging_tile() const { return m_dragging; }

    virtual bool hasHeightForWidth() const override { return true; }
    virtual int heightForWidth(int width) const override;
    virtual QSize sizeHint() const override;
    virtual QSize minimumSizeHint() const override;

signals:
    // A tile was dragged from one place in the order to another.
    void tile_moved(int from, int to);

protected:
    virtual bool eventFilter(QObject*, QEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;

private:
    int slot_count() const;
    int height_for(int width, int slot_total) const;
    int slot_at(QPoint position, int slot_total) const;
    void place_tiles(Animate);
    void move_tile(FavoriteTile&, QRect const&, Animate);

    void begin_drag(FavoriteTile&);
    void drag_to(QPoint position);
    void end_drag();

    int m_tile_height { 0 };
    QList<QPointer<FavoriteTile>> m_tiles;
    QHash<FavoriteTile*, QPropertyAnimation*> m_animations;
    int m_gap { -1 };

    QPointer<FavoriteTile> m_pressed;
    QPoint m_press_position;
    QPoint m_grab_offset;
    bool m_dragging { false };
    int m_drag_from { -1 };
    int m_drag_to { -1 };
};

}
