/*
 * The list in the Arc sidebar: pinned folders and tabs, the "New Tab" row, then Today's tabs.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QUrl>

namespace Ladybird::Arc {

enum class RowKind : int {
    PinnedTab,
    Folder,
    NewTab, // Also the divider between pinned and Today.
    TodayTab,
};

enum Role : int {
    KindRole = Qt::UserRole + 1,
    IdRole,        // Pinned node id (pinned tabs and folders).
    TabRole,       // quintptr of the live Tab, if any.
    IsCurrentRole, // The window's current tab.
    IsLoadedRole,  // Pinned tabs: has a live tab.
};

RowKind row_kind(QModelIndex const&);

// Accepts the sidebar's own rows and URL drags from elsewhere.
class SidebarModel final : public QStandardItemModel {
    Q_OBJECT

public:
    using QStandardItemModel::QStandardItemModel;

    virtual QStringList mimeTypes() const override;
    virtual bool canDropMimeData(QMimeData const*, Qt::DropAction, int, int, QModelIndex const&) const override;
    virtual Qt::DropActions supportedDropActions() const override { return Qt::MoveAction | Qt::CopyAction; }
};

class SidebarDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QColor accent;

    static QRect close_button_rect(QRect const& row);
    static QRect chevron_rect(QRect const& row);
    // The divider row: the rule's strip (with Clear at its right end) above the New Tab button.
    static QRect divider_strip_rect(QRect const& row);
    static QRect clear_button_rect(QRect const& row);

    virtual void paint(QPainter*, QStyleOptionViewItem const&, QModelIndex const&) const override;
    virtual QSize sizeHint(QStyleOptionViewItem const&, QModelIndex const&) const override;
};

class SidebarTree final : public QTreeView {
    Q_OBJECT

public:
    explicit SidebarTree(QWidget* parent);

    SidebarDelegate& delegate() { return *m_delegate; }
    bool is_dragging() const { return state() == DraggingState; }
    void reset_drag_state() { setState(NoState); }

    enum class DropPlace {
        Above,
        Below,
        Into,
        End,
    };

signals:
    void row_clicked(QModelIndex const&);
    void close_clicked(QModelIndex const&);
    void clear_clicked();
    void background_double_clicked();
    void row_dropped(QModelIndex const& source, QModelIndex const& target, DropPlace);
    void urls_dropped(QList<QUrl> const&, QModelIndex const& target, DropPlace);

protected:
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void mouseDoubleClickEvent(QMouseEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void leaveEvent(QEvent*) override;
    virtual void dragMoveEvent(QDragMoveEvent*) override;
    virtual void dropEvent(QDropEvent*) override;
    virtual void drawBranches(QPainter*, QRect const&, QModelIndex const&) const override { }

private:
    DropPlace drop_place_for(QModelIndex const& target) const;

    SidebarDelegate* m_delegate { nullptr };
    QPersistentModelIndex m_pressed_index;
    QPersistentModelIndex m_hovered_index;
    bool m_pressed_close { false };
    bool m_pressed_clear { false };
};

}
