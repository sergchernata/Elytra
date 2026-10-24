/*
 * Arc-style sidebar: favorites, the current space's pinned folders and tabs, Today's tabs,
 * and a space switcher. Lives inside upstream's vertical tab column (see TabWidget hook).
 *
 * Tabs remain owned by upstream's TabWidget. Each Tab carries two dynamic properties:
 *   arc_space: the space it belongs to (empty for favorites, which show in every space)
 *   arc_node:  the pinned node id, "fav:<favorite id>", or empty for a Today tab
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QHash>
#include <QPushButton>
#include <QSet>
#include <QPointer>
#include <QWidget>
#include <UI/Qt/Arc/ArcShortcuts.h>
#include <UI/Qt/Arc/SidebarTree.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/Arc/SpaceStore.h>

class QGridLayout;
class QHBoxLayout;
class QKeyEvent;
class QLineEdit;
class QWheelEvent;
class QLabel;
class QTimer;
class QToolButton;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QParallelAnimationGroup;
class QVariantAnimation;

namespace Ladybird {

class BrowserWindow;
class Tab;
class TabWidget;

namespace Arc {

class SymbolButton;
namespace OnePassword {
class Runner;
struct Login;
}

// While alive, keeps the sidebar slid out (when hidden) even though the pointer is over a menu or dialog.
// Construct it around menu.exec() and modal dialogs opened from the sidebar.
class KeepSidebarOpen {
public:
    explicit KeepSidebarOpen(QWidget* inside_sidebar);
    ~KeepSidebarOpen();

private:
    QPointer<QWidget> m_sidebar;
};

class ArcSidebar final
    : public QWidget
    , public WebView::SettingsObserver {
    Q_OBJECT
    friend class SelfTest; // ArcSelfTest.cpp

public:
    static void attach(BrowserWindow&);
    static ArcSidebar* for_window(BrowserWindow&);

    ArcSidebar(BrowserWindow&, TabWidget&);

    QString current_space_id() const { return m_space_id; }
    void switch_to_space(QString const& id);
    QRect space_slide_area() const;
    void hide_scrollbar_for_slide();
    void show_scrollbar_briefly();
    void fade_scrollbar_to(qreal opacity, int duration_ms);
    void set_scrollbar_opacity(qreal);
    void switch_to_space_at(int index);
    bool handle_key_press(QKeyEvent const&);
    static bool is_space_shortcut(QKeyEvent const&);
    // Runs one of Arc's keyboard shortcuts (see ArcShortcuts.h) in this window. False if it didn't apply.
    bool run_shortcut(ArcShortcut const&);

    // Opens tabs in the background in the given spaces' Today lists, skipping URLs a space already has open.
    // Returns how many were opened.
    int open_background_tabs(QList<SessionEntry> const&);
    // Closes the current space's Today tabs (the divider's Clear button).
    void clear_today();
    // A tab for the command bar to load into: the spare if ready, else a new one, in the current space and its profile,
    // and shown.
    Tab& new_tab_for_input();
    void back_to_pinned_url(QString const& node_id);
    void separate_from_pinned_tab(QString const& node_id);

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void mouseDoubleClickEvent(QMouseEvent*) override;
    virtual void showEvent(QShowEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;

private:
    // Tab bookkeeping
    QList<Tab*> tabs() const;
    QList<Tab*> today_tabs(QString const& space_id) const;
    static bool is_blank(Tab const&);
    void before_tab_closes(Tab&);
    void close_stale_blank_tabs();
    Tab* tab_for_node(QString const& node_id) const;
    static QString space_of(Tab const&);
    static QString node_of(Tab const&);
    static void set_tab_space(Tab&, QString const&);
    static void set_tab_node(Tab&, QString const&);
    static QString url_of(Tab const&);
    static QString title_of(Tab const&);
    void track_tabs();
    void restore_session_assignments();
    void save_session();
    void watch_tab(Tab&);

    // What the keyboard steps through, in sidebar order: the favorites, then the list's pinned tabs (in open
    // folders) and Today's tabs.
    struct SidebarItem {
        QString favorite_id;
        QString node_id;
        QPointer<Tab> tab;
    };
    QList<SidebarItem> sidebar_items() const;
    int current_sidebar_item(QList<SidebarItem> const&) const;
    void activate_item(SidebarItem const&);
    void step_tab(int delta);
    void step_space(int delta);
    void copy_current_url(bool as_markdown);
    void fill_from_one_password();
    void choose_one_password_login(QList<OnePassword::Login> const&, QPointer<Tab>, QString const& origin);
    void fill_one_password_login(QString const& id, QPointer<Tab>, QString const& origin);
    void new_tab_from_clipboard();
    void toggle_current_pin();
    void duplicate_tab(Tab&);
    void close_tabs_below(Tab&);
    static void toggle_sidebar();

    // Two-finger swipes across the sidebar switch spaces, as in Arc.
    bool handle_wheel(QWheelEvent&);

    // Renaming in place, in the row (as in Arc), rather than in a dialog.
    void begin_inline_rename(QString const& node_id);
    void place_inline_rename();
    void finish_inline_rename(bool commit);

    // Actions
    Tab* open_tab(QString const& url); // Null for a malformed URL.
    bool is_private() const;
    void prepare_spare_tab();
    void schedule_spare_tab();
    Tab* take_spare_tab(QString const& profile);
    static bool tab_is_in_profile(Tab const&, QString const& profile);
    void activate(Tab&);
    void close_tab(Tab&);
    void open_or_activate_node(QString const& node_id);
    void open_or_activate_favorite(QString const& favorite_id);
    void pin_tab(Tab&, QString const& parent_id, int index);
    void unpin_node(QString const& node_id);
    void add_favorite(Tab&, int index = -1);
    void move_to_space(QModelIndex const&, QString const& space_id);
    void new_folder(QString const& parent_id, int index);
    void rename_node(QString const& node_id);
    void delete_folder(QString const& node_id);
    void new_space();
    void edit_space(QString const& space_id);
    void delete_space(QString const& space_id);

    // Views
    void schedule_rebuild();
    void rebuild();
    void rebuild_favorites();
    void rebuild_tree();
    void fill_tree();
    void rebuild_switcher();
    void show_shelf();
    void hide_shelf();
    virtual void tab_settings_changed() override;
    void update_mode();
    void dock_panel();
    void apply_toolbar_inset();
    void install_address_bar(QWidget& toolbar);
    void prepare_card_panel();
    void show_flat_host(QRect panel_rect);
    void begin_transition(bool expanding);
    void start_transition();
    void step_transition(qreal progress);
    void finish_transition();
    qreal fade_opacity(qreal progress) const;
    void set_transition_opacity(qreal);
    void set_panel_card(bool);
    void place_window_controls(QPoint panel_origin, bool match_native);
    void ensure_host();
    void size_host();
    void reveal();
    void retract_if_left();
    void slide_host(QPoint from, QPoint to, bool revealing);
    QPoint rest_host_position() const;
    QPoint hidden_host_position() const;
    bool pointer_is_over_floating_sidebar() const;
    bool host_is_out() const;
    void park_host();
    void update_space_header_icon();
    void position_shelf();
    void append_nodes(QStandardItem& parent, QList<PinnedNode> const&);
    void update_tab_rows(Tab&);
    void update_current_highlight();
    QColor space_color() const;
    QString current_profile() const;
    static QString profile_of_space(QString const& space_id);
    static QString profile_of_tab(Tab const&);
    void apply_profile_for_new_views();
    void style_update_button();
    void place_tabs_in_their_profiles();
    void apply_space_tint();
    void apply_tint(QColor const&, double amount, bool visible_only = false);
    void fade_to_space_tint();
    double space_tint_amount() const;
    QIcon icon_for(QString const& url, Tab const* tab) const;

    void on_current_tab_changed(int index);
    void on_row_clicked(QModelIndex const&);
    void on_close_clicked(QModelIndex const&);
    void on_row_dropped(QModelIndex const& source, QModelIndex const& target, SidebarTree::DropPlace);
    void on_urls_dropped(QList<QUrl> const&, QModelIndex const& target, SidebarTree::DropPlace);
    void show_tree_menu(QPoint const&);
    void show_space_menu(QString const& space_id, QPoint const& global_position);
    void show_favorite_menu(QString const& favorite_id, QPoint const& global_position);

    struct DropTarget {
        bool pinned { false }; // false: Today section
        QString parent_id;     // Pinned only
        int index { -1 };      // Pinned: index within parent. Today: index among Today tabs.
    };
    DropTarget resolve_drop(QModelIndex const& target, SidebarTree::DropPlace) const;

    int m_serial { 0 }; // Unique per sidebar, for marking tabs it watches.
    QPointer<BrowserWindow> m_window;
    QPointer<Tab> m_spare_tab; // See prepare_spare_tab().
    QPushButton* m_update_button { nullptr };
    QString m_tree_space_id;                // The space the list shows.
    QHash<QString, int> m_scroll_positions; // Each space's list scroll position.
    QStringList m_favorite_ids;                       // What the favorite tiles show, in order.
    QString m_favorite_style;
    QList<QPointer<QToolButton>> m_favorite_buttons;
    TabWidget& m_tabs;
    QString m_space_id;
    QHash<QString, QPointer<Tab>> m_last_tab_in_space;
    QPointer<Tab> m_current_tab;

    QWidget* m_top_strip { nullptr };
    QWidget* m_favorites { nullptr };
    QGridLayout* m_favorites_layout { nullptr };
    QWidget* m_panel { nullptr };
    enum class Mode {
        Docked,
        Hidden,
    };
    Mode m_mode { Mode::Docked };
    bool m_mode_set { false };
    QPointer<class FloatingSidebarHost> m_host;
    QPropertyAnimation* m_host_animation { nullptr };
    bool m_host_retracting { false };
    bool m_host_docking { false };
    bool m_host_lifting_off { false };
    enum class Transition {
        None,
        Hide,
        Show,
    };
    Transition m_pending_transition { Transition::None };
    Transition m_active_transition { Transition::None };
    class QVariantAnimation* m_transition { nullptr };
    QPoint m_transition_from;
    QPoint m_transition_to;
    int m_transition_width_from { 0 };
    int m_transition_width_to { 0 };
    class SymbolButton* m_sidebar_toggle { nullptr };
    QTimer* m_reveal_timer { nullptr };
    QTimer* m_retract_timer { nullptr };
    QPointer<QWidget> m_watched_column;
    QRect m_last_docked_rect;
    class TrafficLights* m_traffic_lights { nullptr };
    QWidget* m_header { nullptr };
    bool m_holding_tint { false }; // While a slide between spaces runs, the old space's tint stays.
    class QVariantAnimation* m_tint_fade { nullptr };
    class QVariantAnimation* m_scrollbar_fade { nullptr };
    QTimer* m_scrollbar_linger { nullptr };
    qreal m_scrollbar_opacity { 0 };
    class SymbolButton* m_space_icon { nullptr };
    class SymbolButton* m_space_menu_button { nullptr };
    bool m_header_hovered { false };
    QSet<QString> m_collapsed_spaces;
    class QGraphicsOpacityEffect* m_shelf_opacity { nullptr };
    class QPropertyAnimation* m_shelf_position_animation { nullptr };
    class QPropertyAnimation* m_shelf_opacity_animation { nullptr };
    class QParallelAnimationGroup* m_shelf_animation { nullptr };
    bool m_shelf_hiding { false };
    QLabel* m_space_title { nullptr };
    class SymbolButton* m_downloads_button { nullptr };
    class DownloadsShelf* m_shelf { nullptr };
    QTimer* m_shelf_show_timer { nullptr };
    QTimer* m_shelf_hide_timer { nullptr };
    QTimer* m_shelf_refresh_timer { nullptr };
    SidebarTree* m_tree { nullptr };
    SidebarModel* m_model { nullptr };
    QWidget* m_switcher { nullptr };
    QHBoxLayout* m_switcher_layout { nullptr };
    QList<QPointer<SymbolButton>> m_space_buttons;
    QStringList m_switcher_key;
    OnePassword::Runner* m_one_password { nullptr };

    bool m_rebuild_pending { false };
    qreal m_swipe_distance { 0 };
    enum class SwipeAxis {
        Undecided,
        Horizontal,
        Vertical,
    };
    SwipeAxis m_swipe_axis { SwipeAxis::Undecided };
    bool m_swipe_switched { false };
    qint64 m_swipe_last_event_ms { 0 };
    QPointer<QLineEdit> m_rename_editor;
    QString m_rename_node_id;
    bool m_switching_space { false };
};

}

}
