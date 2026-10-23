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

namespace Ladybird::Arc {

// Formatting for the downloads shelf ("1 day ago"), and which files in Downloads are unfinished downloads.
QString downloads_relative_time(QDateTime const& time, QDateTime const& now);
bool is_partial_download_name(QString const& file_name);

// Recent downloads shown above the sidebar's bottom bar while the downloads button is hovered.
// The top fades into the sidebar so the list seems to rise out of it.
class DownloadsShelf final : public QWidget {
    Q_OBJECT

public:
    explicit DownloadsShelf(QWidget* parent);

    void refresh();
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
    int m_hovered_row { -1 };
    int m_pressed_row { -1 };
    QPoint m_press_position;
};

}
