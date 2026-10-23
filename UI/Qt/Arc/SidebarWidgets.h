/*
 * Small widgets for the Arc sidebar: a centered symbol button and the downloads shelf.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QAbstractButton>
#include <QDateTime>
#include <QList>
#include <QPoint>
#include <QWidget>

namespace Ladybird::Arc {

// A square button that draws one SF Symbol (or a letter) exactly in its center, as in Arc's bottom bar.
class SymbolButton final : public QAbstractButton {
    Q_OBJECT

public:
    explicit SymbolButton(QWidget* parent);

    // A space icon ("sf:<name>" or text), or a bare symbol name with set_symbol().
    void set_icon_text(QString const&);
    void set_symbol(QString const& name);
    void set_dimmed(bool);
    void set_symbol_point_size(int);

    virtual QSize sizeHint() const override { return { 30, 30 }; }

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void enterEvent(QEnterEvent*) override;
    virtual void leaveEvent(QEvent*) override;

private:
    QString m_symbol;
    QString m_text;
    bool m_dimmed { false };
    bool m_hovered { false };
    int m_point_size { 15 };
};

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
