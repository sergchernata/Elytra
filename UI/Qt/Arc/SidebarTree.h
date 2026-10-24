/*
 * The list in the Arc sidebar: pinned folders and tabs, the "New Tab" row, then Today's tabs.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QVariantAnimation>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <QUrl>

namespace Ladybird::Arc {

enum class RowKind : int {
    PinnedTab,
    Folder,
    NewTab, // Also the divider between pinned and Today.
    TodayTab,
    BottomSpace, // Empty room after the last row, as in Arc (double-click: new tab).
};

enum Role : int {
    KindRole = Qt::UserRole + 1,
    IdRole,        // Pinned node id (pinned tabs and folders).
    TabRole,       // quintptr of the live Tab, if any.
    IsCurrentRole, // The window's current tab.
    IsLoadedRole,  // Pinned tabs: has a live tab.
    NavigatedAwayRole, // Pinned tabs: the live tab is on another page than the pinned URL.
};

// Whether a pinned tab showing `current` is still on its pinned URL (ignoring a trailing slash, the fragment and the
// host's case). An empty `current` (nothing loaded yet) counts as on it.
bool pinned_urls_match(QString const& pinned, QString const& current);

// Whether a tab at this URL shows a page yet (not a blank or new-tab page, nor nothing at all).
bool tab_shows_a_page(QString const& tab_url);
// Whether a pinned tab at `tab_url` has navigated away from its pinned URL (Arc's "|" before the title).
bool pinned_tab_is_away(QString const& pinned_url, QString const& tab_url);
// The same, for a tab that landed on `home` from its pinned URL (sites redirect: example.com to example.com/home):
// it's away only once it leaves both.
bool pinned_tab_is_away(QString const& pinned_url, QString const& tab_url, QString const& home);
// A pinned row's title: its custom title, else the live tab's once it shows its page and has one, else the title
// saved from its last visit, else its URL.
QString pinned_row_title(QString const& custom_title, QString const& saved_title, QString const& live_title, QString const& pinned_url, QString const& tab_url);

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
    // Where a field for renaming the row in place goes: over its title, with the text where the title is drawn.
    static QRect title_edit_rect(QRect const& row);
    static QRect chevron_rect(QRect const& row);
    // The divider row: the rule's strip (with Clear at its right end) above the New Tab button.
    static QRect divider_strip_rect(QRect const& row);
    static QRect clear_button_rect(QRect const& row);
    static bool divider_has_today_tabs(QModelIndex const& divider);
    // A pinned tab's favicon, a button while the tab is away from its pinned URL (as in Arc).
    static QRect pinned_icon_rect(QRect const& row);
    static QString pinned_icon_label(bool separate);

    enum class PinnedRowState {
        OnPinnedUrl,           // Or not a loaded pinned tab.
        Away,                  // "/ Title".
        BackToPinnedUrl,       // Icon hovered.
        SeparateFromPinnedTab, // Icon hovered with Shift.
    };
    static PinnedRowState pinned_row_state(QModelIndex const&, QRect const& row, Optional<QPoint> hover_position, bool shift);

    // Where the pointer is over the view (set by SidebarTree), and whether Shift is held.
    Optional<QPoint> hover_position;
    bool shift_held { false };
    // The pinned row whose icon hover is animating in or out (0: resting, 1: showing its label).
    QPersistentModelIndex pinned_hover_index;
    qreal pinned_hover_progress { 0 };
    bool pinned_hover_separate { false };

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

    // Replaces the rows (via `rebuild`) without moving the list: same scroll position, no expand animations, no
    // repaint midway.
    void rebuild_keeping_scroll_position(Function<void()> const& rebuild);
    bool is_rebuilding() const { return m_rebuilding; } // Its scroll position moves then, not by the user's hand.

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
    void pinned_row_double_clicked(QModelIndex const&); // To rename it, as in Arc.
    // An away pinned tab's favicon: back to the pinned URL, or with Shift, separate the page into its own tab.
    void pinned_icon_clicked(QModelIndex const&, bool separate);
    void row_dropped(QModelIndex const& source, QModelIndex const& target, DropPlace);
    void urls_dropped(QList<QUrl> const&, QModelIndex const& target, DropPlace);

protected:
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void mouseDoubleClickEvent(QMouseEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void enterEvent(QEnterEvent*) override;
    virtual void leaveEvent(QEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;
    virtual void dragMoveEvent(QDragMoveEvent*) override;
    virtual void dropEvent(QDropEvent*) override;
    virtual void drawBranches(QPainter*, QRect const&, QModelIndex const&) const override { }

private:
    bool m_rebuilding { false };
    DropPlace drop_place_for(QModelIndex const& target) const;

    SidebarDelegate* m_delegate { nullptr };
    QPersistentModelIndex m_pressed_index;
    QPersistentModelIndex m_hovered_index;
    bool m_pressed_close { false };
    bool m_pressed_clear { false };
    bool m_pressed_pinned_icon { false };
    QVariantAnimation* m_pinned_hover_animation { nullptr };
    void update_pinned_hover();
};

}
