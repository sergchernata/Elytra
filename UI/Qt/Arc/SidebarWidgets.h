/*
 * Small widgets for the Arc sidebar: a centered symbol button and the downloads shelf.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <UI/Qt/Arc/SymbolButton.h>
#include <QDateTime>
#include <QList>
#include <QPoint>
#include <QWidget>

class QFileSystemWatcher;

namespace Ladybird::Arc {

// Formatting for the downloads shelf ("1 day ago"), and which files in Downloads are unfinished downloads.
QString downloads_relative_time(QDateTime const& time, QDateTime const& now);
bool is_partial_download_name(QString const& file_name);

// Where a shelf row is `elapsed_ms` after the shelf opens: `offset` pixels below its place and at `opacity`. Rows rise
// in one after another, from the one nearest the button (row 0) up.
struct ShelfRowMotion {
    qreal offset { 0 };
    qreal opacity { 1 };
};
ShelfRowMotion shelf_row_motion(int row, qreal elapsed_ms);
int shelf_rows_duration_ms(int rows);

// Recent downloads shown above the sidebar's bottom bar while the downloads button is hovered.
// The top fades into the sidebar so the list seems to rise out of it.
class DownloadsShelf final : public QWidget {
    Q_OBJECT
    // Painted in, rather than through a graphics effect (which renders the shelf offscreen, and darkens it).
    Q_PROPERTY(qreal opacity READ opacity WRITE set_opacity)
    friend class SelfTest; // ArcSelfTest.cpp

public:
    explicit DownloadsShelf(QWidget* parent);

    void refresh();
    qreal opacity() const { return m_opacity; }
    void set_opacity(qreal);
    // Its rows rise in one after another (see shelf_row_motion()).
    void reveal_rows();
    bool is_empty() const { return m_items.isEmpty(); }
    int preferred_height() const;

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void leaveEvent(QEvent*) override;

private:
    struct Item {
        QString path;
        QString name;
        QDateTime time;
        int percent { -1 }; // In-progress downloads only.
        bool in_progress { false };
    };

    QRect row_rect(int index) const;
    int row_at(QPoint) const;
    void show_menu(int index, QPoint global_position);

    QList<Item> m_items;
    qreal m_opacity { 1 };
    qreal m_rows_elapsed_ms { 1e9 }; // Long since revealed.
    class QVariantAnimation* m_rows_animation { nullptr };
    // The Downloads folder's newest entries, listed again only when the folder changes (refresh() runs every second
    // while the shelf shows, for download progress).
    QList<Item> m_folder_items;
    bool m_folder_changed { true };
    QFileSystemWatcher* m_folder_watcher { nullptr };
    int m_hovered_row { -1 };
    int m_pressed_row { -1 };
    QPoint m_press_position;
};

}
