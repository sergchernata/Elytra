/*
 * Arc-style sidebar: favorites, the current space's pinned folders and tabs, Today's tabs,
 * and a space switcher.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWebView/Application.h>
#include <UI/Qt/Arc/AddressBar.h>
#include <UI/Qt/Arc/ArcIcons.h>
#include <LibURL/Parser.h>
#include <LibWebView/SessionHistory.h>
#include <UI/Qt/Arc/ArcSidebar.h>
#include <UI/Qt/Arc/ProfileSessions.h>
#include <UI/Qt/Arc/UpdateChecker.h>
#include <LibWebView/BrowsingSession.h>
#include <UI/Qt/Arc/CommandBar.h>
#include <UI/Qt/Arc/FloatingPanel.h>
#include <UI/Qt/Arc/OnePassword.h>
#include <UI/Qt/Arc/SidebarWidgets.h>
#include <UI/Qt/Arc/SpaceSlide.h>
#include <UI/Qt/Arc/SymbolIcon.h>
#include <UI/Qt/Arc/Toast.h>
#include <UI/Qt/Arc/WindowControls.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/WebContentView.h>

#include <QButtonGroup>
#include <QClipboard>
#include <QDateTime>
#include <QWheelEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QKeyEvent>
#include <QScopeGuard>
#include <QMouseEvent>
#include <QWindow>
#include <QApplication>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>
#include <iterator>

namespace Ladybird::Arc {


static constexpr char const* SPACE_PROPERTY = "arc_space";
static constexpr char const* BLANK_PROPERTY = "arc_blank";
static constexpr int UPDATE_BUTTON_HEIGHT = 40;
static constexpr int SPARE_TAB_DELAY_MS = 1200;
static constexpr char const* NODE_PROPERTY = "arc_node";
static constexpr char const* LAST_TITLE_PROPERTY = "arc_last_title"; // See live_title().
static constexpr char const* PINNED_HOME_PROPERTY = "arc_pinned_home";       // See begin_pinned_landing().
static constexpr char const* PINNED_LANDING_PROPERTY = "arc_pinned_landing"; // Still landing: its home isn't settled.
static constexpr char const* LANDING_FROM_STEP_PROPERTY = "arc_landing_from_step";
static constexpr char const* LANDING_STEP_PROPERTY = "arc_landing_step";
static constexpr auto FAVORITE_PREFIX = "fav:";
static constexpr int ExpandedRole = Qt::UserRole + 20;
static constexpr int FAVORITE_TILE_HEIGHT = 54;
static constexpr int TOP_STRIP_HEIGHT = 34;
static constexpr int FLOATING_MARGIN = 6;
static constexpr int FLOATING_PADDING = 8;
static constexpr int FLOATING_LIGHTS_INSET = 6;
static constexpr int SHELF_SLIDE_DISTANCE = 14;
static constexpr int SHELF_ANIMATION_MS = 190;
static constexpr int REVEAL_DELAY_MS = 60;
static constexpr int TRANSITION_MS = 300;
static constexpr int SHOW_SETTLE_MS = 34;
static constexpr int HANDOFF_MS = 34; // Two frames, for the window server to show one before the other goes.
static constexpr qreal FADE_SPEEDUP = 1.6;
static constexpr int HIDDEN_TOOLBAR_LEFT_MARGIN = 2;
static constexpr int RETRACT_DELAY_MS = 280;
static constexpr int REVEAL_ANIMATION_MS = 260;
static constexpr int RETRACT_ANIMATION_MS = 220;

#if defined(AK_OS_MACOS)
static constexpr auto SPACE_SHORTCUT_MODIFIER = Qt::META; // The Control key on macOS, as in Arc.
#else
static constexpr auto SPACE_SHORTCUT_MODIFIER = Qt::ALT;
#endif

struct ColorPreset {
    char const* name;
    char const* hex;
};
static constexpr ColorPreset COLOR_PRESETS[] = {
    { "Blue", "#6b8fd6" },
    { "Purple", "#9b7fd4" },
    { "Pink", "#d47fb0" },
    { "Red", "#d46f6f" },
    { "Orange", "#d9965b" },
    { "Yellow", "#cdb85c" },
    { "Green", "#6fb37f" },
    { "Teal", "#5fb3b3" },
    { "Gray", "#8a8f98" },
};

static QString favorite_node(QString const& favorite_id) { return FAVORITE_PREFIX + favorite_id; }
static bool is_favorite_node(QString const& node) { return node.startsWith(FAVORITE_PREFIX); }

static Tab* tab_in_row(QModelIndex const& index)
{
    return reinterpret_cast<Tab*>(index.data(TabRole).value<quintptr>());
}

template<typename Callback>
static void for_each_item(QStandardItem& parent, Callback const& callback)
{
    for (int row = 0; row < parent.rowCount(); ++row) {
        auto* child = parent.child(row);
        callback(*child);
        for_each_item(*child, callback);
    }
}

// --- Keeping the slid-out sidebar open ---

KeepSidebarOpen::KeepSidebarOpen(QWidget* inside_sidebar)
{
    // The sidebar's panel lives either in the sidebar (docked) or in the floating host (hidden mode).
    for (auto* widget = inside_sidebar; widget; widget = widget->parentWidget()) {
        if (widget->property("arcSidebarRoot").toBool()) {
            m_sidebar = widget;
            break;
        }
    }
    if (m_sidebar)
        m_sidebar->setProperty("arcKeepOpen", true);
}

KeepSidebarOpen::~KeepSidebarOpen()
{
    if (!m_sidebar)
        return;
    m_sidebar->setProperty("arcKeepOpen", false);
    // Re-check the pointer now that the menu is gone; the floating panel slides away if it left.
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(m_sidebar, &leave);
}

// --- Attaching ----------------------------------------------------------------------------------

void ArcSidebar::attach(BrowserWindow& window)
{
    if (for_window(window))
        return;
    // A popup (e.g. a sign-in window) is the page's, not a browsing window: no spaces, and window.close() closes it.
    if (window.is_popup_window())
        return;
    auto* tabs = window.findChild<TabWidget*>();
    if (!tabs)
        return;
    auto* sidebar = new ArcSidebar(window, *tabs);
    tabs->set_vertical_sidebar_widget(sidebar);
    window.setProperty("arc_sidebar", QVariant::fromValue(static_cast<QObject*>(sidebar)));
}

ArcSidebar* ArcSidebar::for_window(BrowserWindow& window)
{
    // Remembered on the window: searching its whole widget tree was too slow for per-keystroke use.
    return qobject_cast<ArcSidebar*>(window.property("arc_sidebar").value<QObject*>());
}

static int s_next_sidebar_serial = 0;

ArcSidebar::ArcSidebar(BrowserWindow& window, TabWidget& tabs)
    : QWidget(&tabs)
    , m_serial(++s_next_sidebar_serial)
    , m_window(&window)
    , m_tabs(tabs)
{
    auto& store = SpaceStore::the();

    // Everything lives in a panel, so that when the sidebar is hidden and slides out over the page
    // the panel can keep its full width and move in from the edge (see dock_panel() and prepare_card_panel()).
    m_panel = new QWidget(this);
    auto* layout = new QVBoxLayout(m_panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // The window buttons (traffic lights) sit over this strip; dragging it moves the window.
    m_top_strip = new QWidget(this);
    m_top_strip->setFixedHeight(TOP_STRIP_HEIGHT);
    m_top_strip->installEventFilter(this);
    // Drawn window controls for the floating panel (hidden mode); docked, the real ones sit over this strip.
    // Both are placed by hand (place_window_controls()) so they can line up exactly with the real controls.
    m_traffic_lights = new TrafficLights(m_top_strip);
    m_traffic_lights->setProperty("arcBrowserWindow", QVariant::fromValue<QWidget*>(&window));
    m_traffic_lights->set_drawn(false);
    // Sidebar toggle, right of the window controls as in Arc (floating panel only).
    m_sidebar_toggle = new SymbolButton(m_top_strip);
    m_sidebar_toggle->set_symbol("sidebar.left");
    m_sidebar_toggle->set_symbol_point_size(18); // Same size as the toolbar's sidebar button.
    m_sidebar_toggle->setFixedSize(28, 28);
    m_sidebar_toggle->setToolTip("Toggle Sidebar");
    m_sidebar_toggle->hide();
    connect(m_sidebar_toggle, &QAbstractButton::clicked, this, [] { toggle_sidebar(); });
    layout->addWidget(m_top_strip);

    m_favorites = new QWidget(this);
    m_favorites_layout = new QGridLayout(m_favorites);
    m_favorites_layout->setContentsMargins(0, 0, 0, 6);
    m_favorites_layout->setSpacing(8);
    m_favorites->setAcceptDrops(true);
    m_favorites->installEventFilter(this);
    layout->addWidget(m_favorites);

    // Space header: icon and name. On hover (as in Arc) the icon becomes a chevron that folds the
    // pinned section away, and a "..." button opens the space menu.
    m_header = new QWidget(this);
    m_header->setObjectName("ArcSpaceHeader");
    m_header->setAttribute(Qt::WA_StyledBackground);
    m_header->setFixedHeight(32);
    auto* header_layout = new QHBoxLayout(m_header);
    header_layout->setContentsMargins(2, 0, 2, 0);
    header_layout->setSpacing(4);
    m_space_icon = new SymbolButton(m_header);
    m_space_icon->setFixedSize(28, 28);
    m_space_title = new QLabel(m_header);
    m_space_menu_button = new SymbolButton(m_header);
    m_space_menu_button->set_symbol("ellipsis");
    m_space_menu_button->setFixedSize(26, 26);
    m_space_menu_button->setToolTip("Space Options");
    m_space_menu_button->hide();
    header_layout->addWidget(m_space_icon);
    header_layout->addWidget(m_space_title, 1);
    header_layout->addWidget(m_space_menu_button);
    m_header->installEventFilter(this);
    m_header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_header, &QWidget::customContextMenuRequested, this, [this](QPoint const& position) {
        show_space_menu(m_space_id, m_header->mapToGlobal(position));
    });
    connect(m_space_menu_button, &QAbstractButton::clicked, this, [this] {
        show_space_menu(m_space_id, m_space_menu_button->mapToGlobal(QPoint(0, m_space_menu_button->height() + 2)));
    });
    connect(m_space_icon, &QAbstractButton::clicked, this, [this] {
        if (m_collapsed_spaces.contains(m_space_id))
            m_collapsed_spaces.remove(m_space_id);
        else
            m_collapsed_spaces.insert(m_space_id);
        rebuild();
    });
    layout->addWidget(m_header);

    m_model = new SidebarModel(this);
    m_tree = new SidebarTree(this);
    m_tree->setModel(m_model);
    layout->addWidget(m_tree, 1);

    // Arc's update notice: a pill above the bottom bar once a newer release is ready (see UpdateChecker).
    m_update_button = new QPushButton(this);
    m_update_button->setObjectName("ArcUpdateButton");
    m_update_button->setCursor(Qt::PointingHandCursor);
    m_update_button->setFocusPolicy(Qt::NoFocus);
    m_update_button->setFixedHeight(UPDATE_BUTTON_HEIGHT);
    m_update_button->hide();
    layout->addWidget(m_update_button);
    connect(m_update_button, &QPushButton::clicked, this, [this] {
        auto& updates = UpdateChecker::the();
        if (updates.state() == UpdateChecker::State::Ready)
            updates.restart_into_update();
        else if (updates.state() == UpdateChecker::State::PageOnly && updates.release().has_value())
            open_tab(updates.release()->page_url);
    });
    auto show_update = [this] {
        auto state = UpdateChecker::the().state();
        // Downloading happens quietly; the pill shows once there's something to click.
        m_update_button->setText(state == UpdateChecker::State::Ready ? "Restart to Update" : "New Version Available");
        style_update_button();
        m_update_button->setVisible(state == UpdateChecker::State::Ready || state == UpdateChecker::State::PageOnly);
    };
    connect(&UpdateChecker::the(), &UpdateChecker::state_changed, this, show_update);
    show_update();

    m_switcher = new QWidget(this);
    m_switcher_layout = new QHBoxLayout(m_switcher);
    // No bottom margin: the panel's own padding below the buttons then matches the padding at its sides.
    m_switcher_layout->setContentsMargins(0, 4, 0, 0);
    m_switcher_layout->setSpacing(0);
    layout->addWidget(m_switcher);

    // Downloads: hovering the left-most button raises a shelf of recent files, as in Arc.
    m_downloads_button = new SymbolButton(m_switcher);
    m_downloads_button->set_dimmed(true); // Faded like the + button.
    m_downloads_button->set_symbol("tray.and.arrow.down");
    m_downloads_button->setToolTip("Downloads");
    m_downloads_button->installEventFilter(this);
    connect(m_downloads_button, &QAbstractButton::clicked, this, [this] {
        if (m_shelf->isVisible() && !m_shelf_hiding)
            QDesktopServices::openUrl(QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)));
        else
            show_shelf();
    });

    m_shelf = new DownloadsShelf(m_panel);
    m_shelf->hide();
    m_shelf->installEventFilter(this);
    m_shelf->set_opacity(0);
    m_shelf_position_animation = new QPropertyAnimation(m_shelf, "pos", this);
    m_shelf_position_animation->setDuration(SHELF_ANIMATION_MS);
    m_shelf_position_animation->setEasingCurve(QEasingCurve::InOutCubic);
    m_shelf_opacity_animation = new QPropertyAnimation(m_shelf, "opacity", this);
    m_shelf_opacity_animation->setDuration(SHELF_ANIMATION_MS);
    m_shelf_opacity_animation->setEasingCurve(QEasingCurve::InOutCubic);
    m_shelf_animation = new QParallelAnimationGroup(this);
    m_shelf_animation->addAnimation(m_shelf_position_animation);
    m_shelf_animation->addAnimation(m_shelf_opacity_animation);
    connect(m_shelf_animation, &QAbstractAnimation::finished, this, [this] {
        if (m_shelf_hiding) {
            m_shelf->hide();
            m_shelf_hiding = false;
        }
    });

    m_shelf_show_timer = new QTimer(this);
    m_shelf_show_timer->setSingleShot(true);
    m_shelf_show_timer->setInterval(140);
    connect(m_shelf_show_timer, &QTimer::timeout, this, &ArcSidebar::show_shelf);
    m_shelf_hide_timer = new QTimer(this);
    m_shelf_hide_timer->setSingleShot(true);
    m_shelf_hide_timer->setInterval(320);
    connect(m_shelf_hide_timer, &QTimer::timeout, this, &ArcSidebar::hide_shelf);
    m_shelf_refresh_timer = new QTimer(this);
    m_shelf_refresh_timer->setInterval(1000);
    connect(m_shelf_refresh_timer, &QTimer::timeout, this, [this] {
        if (!m_shelf->isVisible()) {
            m_shelf_refresh_timer->stop();
            return;
        }
        if (m_shelf_animation->state() == QAbstractAnimation::Running)
            return;
        m_shelf->refresh(); // Keeps in-progress percentages moving.
        position_shelf();
    });

    connect(m_tree, &SidebarTree::row_clicked, this, &ArcSidebar::on_row_clicked);
    connect(m_tree, &SidebarTree::close_clicked, this, &ArcSidebar::on_close_clicked);
    connect(m_tree, &SidebarTree::clear_clicked, this, &ArcSidebar::clear_today);
    connect(m_tree, &SidebarTree::pinned_icon_clicked, this, [this](QModelIndex const& index, bool separate) {
        auto node_id = index.data(IdRole).toString();
        if (separate)
            separate_from_pinned_tab(node_id);
        else
            back_to_pinned_url(node_id);
    });
    connect(m_tree, &SidebarTree::pinned_row_double_clicked, this, [this](QModelIndex const& index) {
        begin_inline_rename(index.data(IdRole).toString());
    });
    m_tree->viewport()->installEventFilter(this); // Swipes between spaces.
    set_scrollbar_opacity(0);
    m_tree->verticalScrollBar()->installEventFilter(this);
    connect(m_tree->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!m_tree->is_rebuilding())
            show_scrollbar_briefly();
    });
    m_panel->installEventFilter(this);
    connect(m_tree, &SidebarTree::background_double_clicked, this, [this] {
        if (m_window)
            CommandBar::toggle_for(*m_window);
    });
    connect(m_tree, &SidebarTree::row_dropped, this, &ArcSidebar::on_row_dropped);
    connect(m_tree, &SidebarTree::urls_dropped, this, &ArcSidebar::on_urls_dropped);
    connect(m_tree, &QWidget::customContextMenuRequested, this, &ArcSidebar::show_tree_menu);
    connect(m_tree, &QTreeView::expanded, this, [](QModelIndex const& index) {
        SpaceStore::the().set_folder_expanded(index.data(IdRole).toString(), true);
    });
    connect(m_tree, &QTreeView::collapsed, this, [](QModelIndex const& index) {
        SpaceStore::the().set_folder_expanded(index.data(IdRole).toString(), false);
    });

    connect(&m_tabs, &TabWidget::tabs_changed, this, [this] {
        apply_toolbar_inset(true); // Now, before the new tab's toolbar is first shown.
        track_tabs();
        schedule_rebuild();
        save_session();
    });
    connect(&m_tabs, &TabWidget::current_tab_changed, this, &ArcSidebar::on_current_tab_changed);
    m_window->on_tab_will_close = [this](Tab& tab) { before_tab_closes(tab); };
    connect(&store, &SpaceStore::changed, this, &ArcSidebar::schedule_rebuild);

    m_space_id = store.space(store.last_active_space_id()) ? store.last_active_space_id() : store.first_space_id();
    restore_session_assignments();
    schedule_spare_tab();
    track_tabs();

    if (auto* current = m_window->current_tab()) {
        auto space = space_of(*current);
        if (!space.isEmpty() && store.space(space))
            m_space_id = space;
        // The tab shown from the start is the one left by the first switch (for the recent tabs, and Picture-in-Picture).
        m_current_tab = current;
        m_recent_tabs.append(current);
    }

    // Hidden-mode plumbing: edge hover reveals the floating panel, leaving it retracts it.
    setProperty("arcSidebarRoot", true);
    m_reveal_timer = new QTimer(this);
    m_reveal_timer->setSingleShot(true);
    m_reveal_timer->setInterval(REVEAL_DELAY_MS);
    connect(m_reveal_timer, &QTimer::timeout, this, [this] { reveal(); });
    m_retract_timer = new QTimer(this);
    m_retract_timer->setSingleShot(true);
    m_retract_timer->setInterval(RETRACT_DELAY_MS);
    connect(m_retract_timer, &QTimer::timeout, this, &ArcSidebar::retract_if_left);
    window.installEventFilter(this);

    // Docking and hiding animate the page width together with the sidebar: upstream tells us just before
    // it changes the layout, so the page can be held in place and then moved by our animation.
    m_tabs.on_vertical_sidebar_expanded_will_change = [this](bool expanded) { begin_transition(expanded); };
    m_transition = new QVariantAnimation(this);
    m_transition->setStartValue(0.0);
    m_transition->setEndValue(1.0);
    m_transition->setDuration(TRANSITION_MS);
    m_transition->setEasingCurve(QEasingCurve::InOutCubic);
    connect(m_transition, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) { step_transition(value.toReal()); });
    connect(m_transition, &QAbstractAnimation::finished, this, &ArcSidebar::finish_transition);

    // Development aid: with ARC_DEV_AUTOTOGGLE=<ms> set, the sidebar docks and hides on that interval, so
    // its animations can be recorded without driving the mouse. Does nothing otherwise.
    if (auto interval = qEnvironmentVariableIntValue("ARC_DEV_AUTOTOGGLE"); interval > 0) {
        auto* timer = new QTimer(this);
        timer->setInterval(interval);
        connect(timer, &QTimer::timeout, this, [] {
            auto& settings = WebView::Application::settings();
            auto tab_settings = settings.tab_settings();
            tab_settings.vertical_tabs_expanded = !tab_settings.vertical_tabs_expanded;
            settings.set_tab_settings(tab_settings);
        });
        timer->start();
    }
    // Likewise, ARC_DEV_CYCLE_SPACES=<ms> steps through the spaces on that interval, to the last and back.
    if (auto interval = qEnvironmentVariableIntValue("ARC_DEV_CYCLE_SPACES"); interval > 0) {
        auto* timer = new QTimer(this);
        timer->setInterval(interval);
        connect(timer, &QTimer::timeout, this, [this, step = 1]() mutable {
            auto const& spaces = SpaceStore::the().spaces();
            for (int i = 0; i < spaces.size(); ++i) {
                if (spaces[i].id != m_space_id)
                    continue;
                if (i + step < 0 || i + step >= spaces.size())
                    step = -step;
                if (i + step >= 0 && i + step < spaces.size())
                    switch_to_space(spaces[i + step].id);
                return;
            }
        });
        timer->start();
    }
    // And ARC_DEV_TOGGLE_FOLDERS=<ms> opens and closes the list's first folder on that interval.
    if (auto interval = qEnvironmentVariableIntValue("ARC_DEV_TOGGLE_FOLDERS"); interval > 0) {
        auto* timer = new QTimer(this);
        timer->setInterval(interval);
        connect(timer, &QTimer::timeout, this, [this] {
            for (int row = 0; row < m_model->rowCount(); ++row) {
                auto index = m_model->index(row, 0);
                if (row_kind(index) == RowKind::Folder && !m_tree->isRowHidden(row, {})) {
                    on_row_clicked(index);
                    return;
                }
            }
        });
        timer->start();
    }

    rebuild();
}

bool ArcSidebar::is_space_shortcut(QKeyEvent const& event)
{
    // Control+1..9 (Alt elsewhere) switches spaces, as in Arc.
    auto modifiers = event.modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    auto expected = SPACE_SHORTCUT_MODIFIER == Qt::META ? Qt::MetaModifier : Qt::AltModifier;
    return modifiers == expected && event.key() >= Qt::Key_1 && event.key() <= Qt::Key_9;
}

bool ArcSidebar::handle_key_press(QKeyEvent const& event)
{
    // Called from an app-wide event filter so it works even while the page has focus (the web view gets first claim
    // on most shortcuts).
    if (!is_space_shortcut(event))
        return false;
    switch_to_space_at(event.key() - Qt::Key_1);
    return true;
}

static Optional<URL::URL> parse_stored_url(QString const& text);

void ArcSidebar::toggle_sidebar()
{
    auto& settings = WebView::Application::settings();
    auto tab_settings = settings.tab_settings();
    tab_settings.vertical_tabs_expanded = !tab_settings.vertical_tabs_expanded;
    settings.set_tab_settings(tab_settings);
}

bool ArcSidebar::run_shortcut(ArcShortcut const& shortcut)
{
    if (!m_window)
        return false;
    switch (shortcut.command) {
    case ArcCommand::ToggleSidebar:
        toggle_sidebar();
        return true;
    case ArcCommand::CopyUrl:
        copy_current_url(false);
        return true;
    case ArcCommand::CopyUrlAsMarkdown:
        copy_current_url(true);
        return true;
    case ArcCommand::NewTabFromClipboard:
        new_tab_from_clipboard();
        return true;
    case ArcCommand::TogglePin:
        toggle_current_pin();
        return true;
    case ArcCommand::ClearToday:
        clear_today();
        return true;
    case ArcCommand::NextTab:
        step_tab(1);
        return true;
    case ArcCommand::PreviousTab:
        step_tab(-1);
        return true;
    case ArcCommand::NextSpace:
        step_space(1);
        return true;
    case ArcCommand::PreviousSpace:
        step_space(-1);
        return true;
    case ArcCommand::FillFromOnePassword:
        fill_from_one_password();
        return true;
    case ArcCommand::NewLittleArc:
    case ArcCommand::OpenInSpace:
    case ArcCommand::ChooseSpace:
        return false; // ArcChrome.cpp handles Little Arc's.
    case ArcCommand::SidebarItem: {
        auto items = sidebar_items();
        if (items.isEmpty())
            return true;
        auto index = shortcut.item < 0 ? items.size() - 1 : shortcut.item;
        if (index < items.size())
            activate_item(items[index]);
        return true;
    }
    }
    return false;
}

QList<ArcSidebar::SidebarItem> ArcSidebar::sidebar_items() const
{
    QList<SidebarItem> items;
    for (auto const& id : m_favorite_ids)
        items.append({ id, {}, tab_for_node(favorite_node(id)) });

    Function<void(QModelIndex const&)> walk = [&](QModelIndex const& parent) {
        for (int row = 0; row < m_model->rowCount(parent); ++row) {
            auto index = m_model->index(row, 0, parent);
            if (m_tree->isRowHidden(row, parent))
                continue;
            switch (row_kind(index)) {
            case RowKind::PinnedTab:
                items.append({ {}, index.data(IdRole).toString(), tab_in_row(index) });
                break;
            case RowKind::TodayTab:
                if (auto* tab = tab_in_row(index); tab && m_tabs.index_of(tab) >= 0)
                    items.append({ {}, {}, tab });
                break;
            case RowKind::Folder:
                if (m_tree->isExpanded(index))
                    walk(index);
                break;
            case RowKind::NewTab:
            case RowKind::BottomSpace:
                break;
            }
        }
    };
    walk({});
    return items;
}

int ArcSidebar::current_sidebar_item(QList<SidebarItem> const& items) const
{
    auto* current = m_window ? m_window->current_tab() : nullptr;
    if (!current)
        return -1;
    for (int i = 0; i < items.size(); ++i) {
        if (items[i].tab == current)
            return i;
    }
    return -1;
}

void ArcSidebar::activate_item(SidebarItem const& item)
{
    if (!item.favorite_id.isEmpty())
        open_or_activate_favorite(item.favorite_id);
    else if (!item.node_id.isEmpty())
        open_or_activate_node(item.node_id);
    else if (item.tab && m_tabs.index_of(item.tab) >= 0)
        activate(*item.tab);
}

// Up and down the sidebar (not upstream's tab order, which mixes in other spaces' tabs), wrapping around.
void ArcSidebar::step_tab(int delta)
{
    auto items = sidebar_items();
    if (items.isEmpty())
        return;
    auto count = static_cast<int>(items.size());
    auto current = current_sidebar_item(items);
    auto next = current < 0 ? (delta > 0 ? 0 : count - 1) : (current + delta % count + count) % count;
    if (next != current)
        activate_item(items[next]);
}

// To the neighboring space; no wrapping around, as in Arc.
void ArcSidebar::step_space(int delta)
{
    auto const& spaces = SpaceStore::the().spaces();
    for (int i = 0; i < spaces.size(); ++i) {
        if (spaces[i].id != m_space_id)
            continue;
        auto next = i + delta;
        if (next >= 0 && next < spaces.size())
            switch_to_space(spaces[next].id);
        return;
    }
}

void ArcSidebar::copy_current_url(bool as_markdown)
{
    auto* tab = m_window ? m_window->current_tab() : nullptr;
    if (!tab || is_blank(*tab))
        return;
    auto url = url_of(*tab);
    if (url.isEmpty() || url == "about:blank")
        return;
    if (as_markdown) {
        QGuiApplication::clipboard()->setText(url_as_markdown(tab->title(), url));
        Toast::show_in(*m_window, "Copied URL as Markdown", "link");
    } else {
        QGuiApplication::clipboard()->setText(url);
        Toast::show_in(*m_window, "Copied URL", "link");
    }
}

// --- Fill from 1Password (Cmd+\\) ----------------------------------------------------------------------------------
//
// The logins saved for the current site come from 1Password's `op` tool (see OnePassword.h). One fills straight away;
// several open a menu to choose from. The page is filled only if it's still on the origin the login was chosen for.

void ArcSidebar::fill_from_one_password()
{
    QPointer<Tab> tab = m_window ? m_window->current_tab() : nullptr;
    if (!tab || is_blank(*tab))
        return;
    auto page = QUrl(url_of(*tab));
    if (!OnePassword::can_fill(page)) {
        Toast::show_in(*m_window, "1Password fills only secure (https) pages", "key");
        return;
    }
    auto origin = OnePassword::origin_of(page);
    if (!m_one_password)
        m_one_password = new OnePassword::Runner(this);
    QPointer<ArcSidebar> self = this;
    m_one_password->list_logins(
        [self, tab, page, origin](QList<OnePassword::Login> logins) {
            if (!self || !self->m_window || !tab)
                return;
            auto matches = OnePassword::logins_for(logins, page);
            if (matches.isEmpty()) {
                Toast::show_in(*self->m_window, QString("No 1Password logins for %1").arg(page.host()), "key");
                return;
            }
            if (matches.size() == 1) {
                self->fill_one_password_login(matches.first().id, tab, origin);
                return;
            }
            self->choose_one_password_login(matches, tab, origin);
        },
        [self](QString message) {
            if (self && self->m_window)
                Toast::show_in(*self->m_window, message, "key");
        });
}

void ArcSidebar::choose_one_password_login(QList<OnePassword::Login> const& logins, QPointer<Tab> tab, QString const& origin)
{
    if (!tab)
        return;
    auto* menu = new QMenu(this);
    menu->setObjectName("ArcOnePasswordMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addSection("Fill from 1Password");
    for (auto const& login : logins) {
        auto text = login.username.isEmpty() ? login.title : QString("%1  —  %2").arg(login.title, login.username);
        auto* action = menu->addAction(text);
        auto id = login.id;
        connect(action, &QAction::triggered, this, [this, id, tab, origin] { fill_one_password_login(id, tab, origin); });
    }
    menu->setActiveAction(menu->actions().value(1));
    // At the top of the page, where sign-in forms usually are.
    auto& view = tab->view();
    menu->popup(view.mapToGlobal(QPoint(view.width() / 2 - menu->sizeHint().width() / 2, 12)));
}

void ArcSidebar::fill_one_password_login(QString const& id, QPointer<Tab> tab, QString const& origin)
{
    QPointer<ArcSidebar> self = this;
    if (!m_one_password)
        m_one_password = new OnePassword::Runner(this);
    m_one_password->get_credentials(id,
        [self, tab, origin](OnePassword::Credentials credentials) {
            // The tab may have moved on while 1Password asked for Touch ID; its script checks the origin again.
            auto same_page = tab && OnePassword::origin_of(QUrl(url_of(*tab))) == origin;
            if (same_page)
                tab->view().run_javascript(ak_string_from_qstring(OnePassword::fill_script(origin, credentials)));
            credentials.password.fill(QChar(0));
            if (self && self->m_window)
                Toast::show_in(*self->m_window, same_page ? "Filled from 1Password" : "The page changed, so nothing was filled", "key");
        },
        [self](QString message) {
            if (self && self->m_window)
                Toast::show_in(*self->m_window, message, "key");
        });
}

// Whatever is on the clipboard, as if typed into the command bar: a URL opens, anything else is searched for.
void ArcSidebar::new_tab_from_clipboard()
{
    auto text = QGuiApplication::clipboard()->text().simplified();
    if (text.isEmpty() || !m_window)
        return;
    auto& tab = new_tab_for_input();
    auto& view = tab.view();
    view.set_next_history_visit_transition(WebView::HistoryVisitTransition::Omnibox);
    view.load_from_user_input(ak_string_from_qstring(text));
    view.setFocus();
}

// Pins a Today tab to the top of the list, or moves a pinned tab back to Today. Favorites stay as they are.
void ArcSidebar::toggle_current_pin()
{
    auto* tab = m_window ? m_window->current_tab() : nullptr;
    if (!tab || is_blank(*tab))
        return;
    auto node = node_of(*tab);
    if (node.isEmpty())
        pin_tab(*tab, {}, -1);
    else if (!is_favorite_node(node))
        unpin_node(node);
}

// A copy of the tab's page just below it, in Today.
void ArcSidebar::duplicate_tab(Tab& tab)
{
    auto url = parse_stored_url(url_of(tab));
    if (!url.has_value() || !m_window)
        return;
    auto space = space_of(tab).isEmpty() ? m_space_id : space_of(tab);
    ScopedProfileForNewViews profile(profile_of_tab(tab));
    auto location = node_of(tab).isEmpty() ? BrowserWindow::TabLocation::after_tab(tab) : BrowserWindow::TabLocation::end();
    auto& copy = m_window->new_tab_from_url(*url, Web::HTML::ActivateTab::Yes, location);
    set_tab_space(copy, space);
}

void ArcSidebar::close_tabs_below(Tab& tab)
{
    auto today = today_tabs(space_of(tab));
    auto index = today.indexOf(&tab);
    if (index < 0)
        return;
    for (auto* below : today.mid(index + 1))
        close_tab(*below);
}

bool ArcSidebar::handle_wheel(QWheelEvent& event)
{
    static constexpr qreal SWITCH_DISTANCE = 40;
    static constexpr qint64 GESTURE_GAP_MS = 300; // Wheels without phases (mice): a pause starts a new gesture.

    auto now = QDateTime::currentMSecsSinceEpoch();
    auto phase = event.phase();
    auto new_gesture = phase == Qt::ScrollBegin || (phase == Qt::NoScrollPhase && now - m_swipe_last_event_ms > GESTURE_GAP_MS);
    m_swipe_last_event_ms = now;
    if (new_gesture) {
        m_swipe_axis = SwipeAxis::Undecided;
        m_swipe_distance = 0;
        m_swipe_switched = false;
    }
    auto horizontal = m_swipe_axis == SwipeAxis::Horizontal;
    if (phase == Qt::ScrollEnd || phase == Qt::ScrollMomentum)
        return horizontal; // A horizontal swipe's glide doesn't scroll the list either.

    auto delta = event.pixelDelta().isNull() ? QPointF(event.angleDelta()) / 4.0 : QPointF(event.pixelDelta());
    if (m_swipe_axis == SwipeAxis::Undecided) {
        if (delta.isNull())
            return false;
        m_swipe_axis = qAbs(delta.x()) > qAbs(delta.y()) ? SwipeAxis::Horizontal : SwipeAxis::Vertical;
    }
    if (m_swipe_axis != SwipeAxis::Horizontal)
        return false;

    // Which way the fingers moved: with natural scrolling the deltas already follow them.
    m_swipe_distance += event.inverted() ? delta.x() : -delta.x();
    if (!m_swipe_switched && qAbs(m_swipe_distance) >= SWITCH_DISTANCE) {
        m_swipe_switched = true;
        // Fingers to the left bring in the space to the right, as when swiping between pages.
        step_space(m_swipe_distance < 0 ? 1 : -1);
    }
    return true;
}

static QModelIndex index_of_node(QStandardItemModel& model, QString const& node_id)
{
    QModelIndex found;
    for_each_item(*model.invisibleRootItem(), [&](QStandardItem& item) {
        if (!found.isValid() && item.data(IdRole).toString() == node_id)
            found = item.index();
    });
    return found;
}

void ArcSidebar::begin_inline_rename(QString const& node_id)
{
    finish_inline_rename(true);
    auto const* node = SpaceStore::the().node(node_id);
    auto index = index_of_node(*m_model, node_id);
    if (!node || !index.isValid() || m_tree->isRowHidden(index.row(), index.parent())) {
        rename_node(node_id);
        return;
    }
    auto current = node->title;
    if (current.isEmpty()) {
        auto* tab = tab_for_node(node_id);
        current = tab ? title_of(*tab) : node->saved_title;
    }

    m_tree->scrollTo(index);
    m_rename_node_id = node_id;
    auto* editor = new QLineEdit(current, m_tree->viewport());
    m_rename_editor = editor;
    editor->setObjectName("ArcInlineRename");
    editor->setFrame(false);
    auto font = editor->font();
    font.setPixelSize(14);
    editor->setFont(font);
    auto dark = ChromeStyle::is_dark(palette());
    editor->setStyleSheet(QString("QLineEdit#ArcInlineRename { background: %1; border: 1px solid %2; border-radius: 6px; padding: 0 5px; }")
            .arg(dark ? "rgba(255, 255, 255, 0.12)" : "rgba(255, 255, 255, 0.75)", dark ? "rgba(255, 255, 255, 0.28)" : "rgba(0, 0, 0, 0.18)"));
    editor->installEventFilter(this);
    connect(editor, &QLineEdit::editingFinished, this, [this] { finish_inline_rename(true); });
    place_inline_rename();
    editor->selectAll();
    editor->show();
    editor->setFocus();
}

// Over the row's title (the rows are rebuilt whenever anything changes; follow it).
void ArcSidebar::place_inline_rename()
{
    if (!m_rename_editor)
        return;
    auto index = index_of_node(*m_model, m_rename_node_id);
    if (!index.isValid() || m_tree->isRowHidden(index.row(), index.parent())) {
        finish_inline_rename(false);
        return;
    }
    m_rename_editor->setGeometry(SidebarDelegate::title_edit_rect(m_tree->visualRect(index)));
}

void ArcSidebar::finish_inline_rename(bool commit)
{
    QPointer<QLineEdit> editor = m_rename_editor;
    if (!editor)
        return;
    m_rename_editor = nullptr; // First: hiding it below loses focus, which finishes again.
    auto title = editor->text().trimmed();
    auto modified = editor->isModified();
    auto had_focus = editor->hasFocus();
    editor->hide();
    editor->deleteLater();

    if (commit && modified) {
        if (auto const* node = SpaceStore::the().node(m_rename_node_id); node && !(node->is_folder() && title.isEmpty()))
            SpaceStore::the().rename_node(m_rename_node_id, title); // Empty: a pinned tab goes back to the page title.
    }
    if (had_focus && m_window) {
        if (auto* tab = m_window->current_tab())
            tab->view().setFocus();
    }
}

// --- Tab bookkeeping ----------------------------------------------------------------------------

QList<Tab*> ArcSidebar::tabs() const
{
    QList<Tab*> result;
    for (int i = 0; i < m_tabs.count(); ++i) {
        if (auto* tab = m_tabs.tab(i))
            result.append(tab);
    }
    return result;
}

QList<Tab*> ArcSidebar::today_tabs(QString const& space_id) const
{
    QList<Tab*> result;
    for (auto* tab : tabs()) {
        if (space_of(*tab) == space_id && node_of(*tab).isEmpty() && !is_blank(*tab))
            result.append(tab);
    }
    return result;
}

Tab* ArcSidebar::tab_for_node(QString const& node_id) const
{
    if (node_id.isEmpty())
        return nullptr;
    // While the list is filled, once per pinned row: from a map made once, rather than a search of every tab each time.
    if (m_node_tabs.has_value())
        return m_node_tabs->value(node_id);
    for (auto* tab : tabs()) {
        if (node_of(*tab) == node_id)
            return tab;
    }
    return nullptr;
}

QString ArcSidebar::space_of(Tab const& tab) { return tab.property(SPACE_PROPERTY).toString(); }
QString ArcSidebar::node_of(Tab const& tab) { return tab.property(NODE_PROPERTY).toString(); }
void ArcSidebar::set_tab_space(Tab& tab, QString const& space) { tab.setProperty(SPACE_PROPERTY, space); }
void ArcSidebar::set_tab_node(Tab& tab, QString const& node)
{
    if (tab.property(NODE_PROPERTY).toString() != node)
        tab.setProperty(PINNED_HOME_PROPERTY, QString());
    tab.setProperty(NODE_PROPERTY, node);
}

// A pinned tab opening its pinned URL lands where the site sends it (example.com redirects to example.com/home):
// that page is its home, not "away" (no "|" or Back to Pinned URL), until it leaves it.
//
// Redirects replace the tab's history entry; following a link, typing an address or going back moves to another.
// So the landing is the history entry the pinned URL opened in, and its home is that entry's page, read from the
// tab's history (the tab's URL and its place in history update separately, in either order).
static int history_index(Tab& tab)
{
    auto& view = tab.view();
    return view.has_traversable() ? static_cast<int>(view.traversable().session_history().current_used_step_index()) : -1;
}

static QString url_at_history_index(Tab& tab, int index)
{
    auto& view = tab.view();
    if (!view.has_traversable() || index < 0)
        return {};
    auto const& history = view.traversable().session_history();
    auto steps = history.used_steps();
    if (index >= static_cast<int>(steps.size()))
        return {};
    // The top-level entry shown at a step is the latest one at or before it.
    Optional<URL::URL> url;
    i32 best = -1;
    for (auto const& entry : history.entries()) {
        if (entry.step <= steps[index] && entry.step > best) {
            best = entry.step;
            url = WebView::display_url_for_session_history_entry(entry);
        }
    }
    return url.has_value() ? qstring_from_ak_string(url->serialize()) : QString();
}

void ArcSidebar::begin_pinned_landing(Tab& tab)
{
    tab.setProperty(PINNED_HOME_PROPERTY, QString());
    tab.setProperty(PINNED_LANDING_PROPERTY, true);
    tab.setProperty(LANDING_FROM_STEP_PROPERTY, history_index(tab));
    tab.setProperty(LANDING_STEP_PROPERTY, -2);
}

// The landing's entry, once the pinned URL has opened in one: its first new entry, or (when it replaced the one it
// started in) the one it's in after its first load.
void ArcSidebar::settle_pinned_landing(Tab& tab, bool loaded)
{
    if (!tab.property(PINNED_LANDING_PROPERTY).toBool() || tab.property(LANDING_STEP_PROPERTY).toInt() != -2)
        return;
    auto index = history_index(tab);
    if (index != tab.property(LANDING_FROM_STEP_PROPERTY).toInt() || (loaded && tab_shows_a_page(url_of(tab))))
        tab.setProperty(LANDING_STEP_PROPERTY, index);
}

bool ArcSidebar::pinned_is_away(Tab& tab, QString const& pinned_url)
{
    auto url = url_of(tab);
    if (tab.property(PINNED_LANDING_PROPERTY).toBool()) {
        settle_pinned_landing(tab, false);
        auto landing = tab.property(LANDING_STEP_PROPERTY).toInt();
        if (landing == -2)
            return false;
        auto home = url_at_history_index(tab, landing);
        if (tab_shows_a_page(home) && !pinned_urls_match(pinned_url, home))
            tab.setProperty(PINNED_HOME_PROPERTY, home);
        if (history_index(tab) == landing)
            return false;
        // It moved on to another entry: its home is settled.
        tab.setProperty(PINNED_LANDING_PROPERTY, false);
    }
    return pinned_tab_is_away(pinned_url, url, tab.property(PINNED_HOME_PROPERTY).toString());
}

QString ArcSidebar::url_of(Tab const& tab)
{
    auto const& url = tab.view().url();
    return qstring_from_ak_string(url.serialize());
}

// As a page starts loading, its tab's title is its address until the page's own title arrives. Rows keep the title
// they had through that (and a pinned row its saved title) instead of flashing the address.
static bool title_is_placeholder(QString const& title, QString const& url)
{
    if (title.trimmed().isEmpty())
        return true;
    return title == url || (title.contains("://") && pinned_urls_match(title, url));
}

QString ArcSidebar::live_title(Tab const& tab)
{
    auto title = tab.title();
    if (tab.is_loading() && title_is_placeholder(title, url_of(tab)))
        return tab.property(LAST_TITLE_PROPERTY).toString();
    return title;
}

void ArcSidebar::remember_title(Tab& tab)
{
    if (auto title = tab.title(); !title_is_placeholder(title, url_of(tab)))
        tab.setProperty(LAST_TITLE_PROPERTY, title);
}

QString ArcSidebar::title_of(Tab const& tab)
{
    auto title = live_title(tab);
    return title.isEmpty() ? "New Tab" : title;
}

void ArcSidebar::track_tabs()
{
    for (auto* tab : tabs()) {
        if (!tab->property(SPACE_PROPERTY).isValid())
            set_tab_space(*tab, m_space_id);
        watch_tab(*tab);
    }
}

void ArcSidebar::watch_tab(Tab& tab)
{
    // One set of connections per sidebar; a tab dragged to another window gets new ones there.
    // Keyed by this sidebar's serial, not its address, which a later sidebar can reuse.
    auto key = QString("arc_watched_%1").arg(m_serial);
    if (tab.property(key.toUtf8().constData()).toBool())
        return;
    tab.setProperty(key.toUtf8().constData(), true);

    QPointer<Tab> guarded = &tab;
    auto refresh = [this, guarded] {
        if (!guarded)
            return;
        remember_title(*guarded);
        update_tab_rows(*guarded);
    };
    connect(&tab, &Tab::title_changed, this, refresh);
    connect(&tab, &Tab::favicon_changed, this, refresh);
    connect(&tab, &Tab::loading_changed, this, [this, guarded, refresh] {
        // The empty page (shown when a space empties) navigated somewhere is a tab like any other now.
        if (guarded && guarded != m_spare_tab && guarded->property(BLANK_PROPERTY).toBool() && tab_shows_a_page(url_of(*guarded))) {
            guarded->setProperty(BLANK_PROPERTY, false);
            schedule_rebuild();
        }
        refresh(); // Navigations: a pinned tab may have left its URL.
    });
    connect(&tab, &Tab::loading_finished, this, [this, guarded, refresh] {
        if (guarded)
            settle_pinned_landing(*guarded, true);
        refresh();
        save_session();
    });
}

// Puts restored tabs back in their spaces and pinned slots, matching by URL in order.
void ArcSidebar::restore_session_assignments()
{
    static bool restored = false;
    if (restored)
        return;
    restored = true;

    auto& store = SpaceStore::the();
    auto entries = store.session();
    QList<bool> used(entries.size(), false);

    for (auto* tab : tabs()) {
        auto url = url_of(*tab);
        for (int i = 0; i < entries.size(); ++i) {
            if (used[i] || entries[i].url != url)
                continue;
            used[i] = true;
            auto const& entry = entries[i];
            if (store.space(entry.space_id))
                set_tab_space(*tab, entry.space_id);

            if (!entry.node_id.isEmpty() && !tab_for_node(entry.node_id)) {
                if (is_favorite_node(entry.node_id) && store.favorite(entry.node_id.mid(4))) {
                    set_tab_node(*tab, entry.node_id);
                    set_tab_space(*tab, {});
                } else if (store.node(entry.node_id)) {
                    set_tab_node(*tab, entry.node_id);
                    tab->setProperty(PINNED_HOME_PROPERTY, entry.home);
                }
            }
            break;
        }
    }
}

void ArcSidebar::save_session()
{
    // Debounced: page loads and tab changes come in bursts.
    static QTimer* timer = nullptr;
    if (!timer) {
        timer = new QTimer;
        timer->setSingleShot(true);
        timer->setInterval(1500);
    }
    // One save for every window (each used to overwrite the others' tabs); private windows' tabs are never saved.
    if (!timer->property("arc_connected").toBool()) {
        timer->setProperty("arc_connected", true);
        QObject::connect(timer, &QTimer::timeout, [] {
            QList<SessionEntry> entries;
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* window = qobject_cast<BrowserWindow*>(widget);
                auto* sidebar = window ? ArcSidebar::for_window(*window) : nullptr;
                if (!sidebar || sidebar->is_private())
                    continue;
                for (auto* tab : sidebar->tabs()) {
                    if (tab != sidebar->m_spare_tab && !tab->property(BLANK_PROPERTY).toBool())
                        entries.append({ url_of(*tab), space_of(*tab), node_of(*tab), tab->property(PINNED_HOME_PROPERTY).toString() });
                }
            }
            SpaceStore::the().set_session(std::move(entries));
        });
    }
    timer->start();
}

// --- Actions ------------------------------------------------------------------------------------

// A stored URL (pinned, favorite, imported) as a URL to load; nothing if it isn't one, rather than aborting.
static Optional<URL::URL> parse_stored_url(QString const& text)
{
    auto encoded = QUrl(text).toEncoded();
    if (encoded.isEmpty())
        return {};
    return URL::Parser::basic_parse(StringView(encoded.constData(), static_cast<size_t>(encoded.size())));
}

Tab* ArcSidebar::open_tab(QString const& text)
{
    auto url = parse_stored_url(text);
    if (!url.has_value()) {
        warnln("Arc sidebar: not opening malformed URL {}", text.toUtf8().constData());
        return nullptr;
    }
    // A spare tab is ready for this: navigating it is much quicker than building a new tab's widgets.
    if (auto* spare = take_spare_tab(current_profile())) {
        spare->setProperty(BLANK_PROPERTY, false);
        set_tab_space(*spare, m_space_id);
        spare->navigate(*url);
        activate(*spare);
        return spare;
    }
    ScopedProfileForNewViews profile(current_profile());
    auto& tab = m_window->new_tab_from_url(*url, Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
    set_tab_space(tab, m_space_id);
    return &tab;
}

Tab& ArcSidebar::new_tab_for_input()
{
    if (auto* spare = take_spare_tab(current_profile())) {
        spare->setProperty(BLANK_PROPERTY, false);
        set_tab_space(*spare, m_space_id);
        activate(*spare);
        return *spare;
    }
    ScopedProfileForNewViews profile(current_profile());
    auto& tab = m_window->create_new_tab(Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
    set_tab_space(tab, m_space_id);
    return tab;
}

// A blank tab built ahead of time, while the app is idle, in the current space's profile. Not listed anywhere; used
// by the next tab the sidebar opens, or as the empty page when a space's last tab closes.
void ArcSidebar::prepare_spare_tab()
{
    // Private windows open few tabs, and their spare could never match a profile's session: none there.
    if (is_private())
        return;
    auto profile = current_profile();
    if (m_spare_tab) {
        if (tab_is_in_profile(*m_spare_tab, profile))
            return;
        auto old = m_spare_tab;
        m_spare_tab = nullptr;
        if (auto index = m_tabs.index_of(old); index >= 0 && m_window->current_tab() != old)
            m_window->request_to_close_tab(index);
    }
    ScopedProfileForNewViews scope(profile);
    auto& spare = m_window->new_tab_from_url(URL::about_blank(), Web::HTML::ActivateTab::No, BrowserWindow::TabLocation::end());
    spare.setProperty(BLANK_PROPERTY, true);
    set_tab_space(spare, {}); // In no space.
    m_spare_tab = &spare;
}

void ArcSidebar::schedule_spare_tab()
{
    QTimer::singleShot(SPARE_TAB_DELAY_MS, this, [guarded = QPointer(this)] {
        if (guarded && guarded->m_window && guarded->m_window->isVisible())
            guarded->prepare_spare_tab();
    });
}

Tab* ArcSidebar::take_spare_tab(QString const& profile)
{
    if (!m_spare_tab || m_tabs.index_of(m_spare_tab) < 0 || !tab_is_in_profile(*m_spare_tab, profile)) {
        schedule_spare_tab();
        return nullptr;
    }
    auto* spare = m_spare_tab.data();
    m_spare_tab = nullptr;
    schedule_spare_tab();
    return spare;
}

bool ArcSidebar::tab_is_in_profile(Tab const& tab, QString const& profile)
{
    auto expected = session_for_profile(profile);
    return &tab.view().session() == (expected ? expected.ptr() : &WebView::Application::default_session());
}

// Arc: an away pinned tab's favicon takes it back to its pinned URL.
void ArcSidebar::back_to_pinned_url(QString const& node_id)
{
    auto const* node = SpaceStore::the().node(node_id);
    auto* tab = tab_for_node(node_id);
    if (!node || !tab)
        return;
    if (auto url = parse_stored_url(node->url); url.has_value()) {
        begin_pinned_landing(*tab);
        tab->navigate(*url);
    }
    activate(*tab);
}

// Arc: Shift-clicking it keeps the page it's on in a tab of its own (in Today, next to it), and takes the pinned tab
// back to its pinned URL.
void ArcSidebar::separate_from_pinned_tab(QString const& node_id)
{
    auto const* node = SpaceStore::the().node(node_id);
    auto* tab = tab_for_node(node_id);
    if (!node || !tab)
        return;
    auto space = space_of(*tab);
    Tab* separated = nullptr;
    {
        ScopedProfileForNewViews profile(profile_of_space(space));
        separated = &m_window->new_tab_from_url(tab->view().url(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::after_tab(*tab));
    }
    set_tab_space(*separated, space);
    if (auto url = parse_stored_url(node->url); url.has_value()) {
        begin_pinned_landing(*tab);
        tab->navigate(*url);
    }
    schedule_rebuild();
}

int ArcSidebar::open_background_tabs(QList<SessionEntry> const& entries)
{
    int opened = 0;
    for (auto const& entry : entries) {
        if (!SpaceStore::the().space(entry.space_id))
            continue;
        auto open_tabs = tabs();
        auto already_open = std::any_of(open_tabs.begin(), open_tabs.end(), [&](Tab* tab) {
            return space_of(*tab) == entry.space_id && node_of(*tab).isEmpty() && url_of(*tab) == entry.url;
        });
        if (already_open)
            continue;
        auto url = parse_stored_url(entry.url);
        if (!url.has_value())
            continue;
        ScopedProfileForNewViews profile(profile_of_space(entry.space_id));
        auto& tab = m_window->new_tab_from_url(*url, Web::HTML::ActivateTab::No, BrowserWindow::TabLocation::end());
        set_tab_space(tab, entry.space_id);
        ++opened;
    }
    if (opened > 0) {
        schedule_rebuild();
        save_session();
    }
    return opened;
}

void ArcSidebar::clear_today()
{
    auto today = today_tabs(m_space_id);
    if (today.isEmpty())
        return;

    for (auto* tab : today) {
        if (auto index = m_tabs.index_of(tab); index >= 0)
            m_window->request_to_close_tab(index);
    }
}

void ArcSidebar::activate(Tab& tab)
{
    if (auto index = m_tabs.index_of(&tab); index >= 0)
        m_window->activate_tab(index);
}

void ArcSidebar::close_tab(Tab& tab)
{
    if (auto index = m_tabs.index_of(&tab); index >= 0)
        m_window->request_to_close_tab(index);
}

// A blank page (the new tab page, or the empty space shown after the last tab closes). Not listed in Today.
bool ArcSidebar::is_private() const
{
    return m_window && m_window->is_private() == WebView::IsPrivate::Yes;
}

bool ArcSidebar::is_blank(Tab const& tab)
{
    // The sidebar's own empty pages (the spare tab, the page shown when a space empties) are flagged. A new tab
    // still loading its first page shows about:blank too, but it's a tab: it's listed, and never closed as stale.
    if (tab.property(BLANK_PROPERTY).toBool())
        return true;
    auto url = url_of(tab);
    return url == "about:newtab" || url == qstring_from_ak_string(WebView::Application::settings().new_tab_page_url().serialize());
}

// Every close (sidebar, Cmd+W, Clear) comes through here. Like Arc: the next tab comes from the same space,
// and closing a space's last tab leaves an empty page rather than another space's tab or a closed window.
// As in Arc: leaving a tab (for another tab or space) while its video plays shows the video in a floating player, which
// keeps playing; coming back to the tab puts it back in the page. Not for a muted tab, or one that's closing.
static constexpr char const* AUTO_PICTURE_IN_PICTURE_PROPERTY = "arc_auto_picture_in_picture";
static constexpr char const* CLOSING_PROPERTY = "arc_closing";

bool ArcSidebar::should_open_picture_in_picture_leaving(Tab& tab)
{
    auto& view = tab.view();
    return !tab.property(CLOSING_PROPERTY).toBool() && m_tabs.index_of(&tab) >= 0
        && view.audio_play_state() == Web::HTML::AudioPlayState::Playing
        && view.page_mute_state() == Web::HTML::MuteState::Unmuted
        && !view.has_picture_in_picture_window();
}

void ArcSidebar::update_auto_picture_in_picture(Tab* previous, Tab& current)
{
    if (previous && previous != &current && should_open_picture_in_picture_leaving(*previous)) {
        previous->setProperty(AUTO_PICTURE_IN_PICTURE_PROPERTY, true);
        previous->view().enter_picture_in_picture_for_playing_video();
    }
    if (current.property(AUTO_PICTURE_IN_PICTURE_PROPERTY).toBool()) {
        current.setProperty(AUTO_PICTURE_IN_PICTURE_PROPERTY, false);
        current.view().exit_picture_in_picture();
    }
}

void ArcSidebar::before_tab_closes(Tab& tab)
{
    tab.setProperty(CLOSING_PROPERTY, true);
    if (m_window->current_tab() != &tab)
        return;
    auto space = space_of(tab);
    // As in Arc: back to the tab shown before this one, if it's still open; else a neighbor.
    Tab* next = most_recent_tab_in_space(space.isEmpty() ? m_space_id : space, &tab);
    auto same_space = today_tabs(space);
    if (auto i = same_space.indexOf(&tab); !next && i >= 0) {
        if (i + 1 < same_space.size())
            next = same_space[i + 1];
        else if (i > 0)
            next = same_space[i - 1];
    }
    if (!next) {
        // A favorite's tab is in no space: fall back within the space shown, or to this profile's favorites, never
        // to a page shown nowhere in the sidebar (another profile's favorite).
        auto shown_space = space.isEmpty() ? m_space_id : space;
        for (auto* other : tabs()) {
            if (other == &tab || is_blank(*other))
                continue;
            auto other_space = space_of(*other);
            auto shown = other_space.isEmpty() ? profile_of_tab(*other) == current_profile() : other_space == shown_space;
            if (shown) {
                next = other;
                break;
            }
        }
    }
    if (next) {
        activate(*next);
        return;
    }
    if (is_blank(tab))
        return; // Closing the empty page itself: upstream behavior (another tab, or the window closes).

    // The spare tab, if ready, is that empty page; any profile will do, as a blank page has no site data.
    Tab* blank = m_spare_tab && m_tabs.index_of(m_spare_tab) >= 0 ? m_spare_tab.data() : nullptr;
    if (blank) {
        m_spare_tab = nullptr;
        schedule_spare_tab();
    } else {
        blank = &m_window->new_tab_from_url(URL::about_blank(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
    }
    blank->setProperty(BLANK_PROPERTY, true);
    set_tab_space(*blank, space.isEmpty() ? m_space_id : space);
    activate(*blank);
}

// Once a real tab is showing, empty pages left over from closing tabs aren't needed.
void ArcSidebar::close_stale_blank_tabs()
{
    auto* current = m_window->current_tab();
    if (!current || is_blank(*current))
        return;
    for (auto* tab : tabs()) {
        if (tab != current && tab != m_spare_tab && tab->property(BLANK_PROPERTY).toBool()) {
            if (auto index = m_tabs.index_of(tab); index >= 0)
                m_window->request_to_close_tab(index);
        }
    }
}

void ArcSidebar::open_or_activate_node(QString const& node_id)
{
    if (auto* tab = tab_for_node(node_id)) {
        activate(*tab);
        return;
    }
    auto const* node = SpaceStore::the().node(node_id);
    if (!node || node->is_folder())
        return;
    auto* tab = open_tab(node->url);
    if (!tab)
        return;
    set_tab_node(*tab, node_id);
    begin_pinned_landing(*tab);
    schedule_rebuild();
}

void ArcSidebar::open_or_activate_favorite(QString const& favorite_id)
{
    auto node = favorite_node(favorite_id);
    if (auto* tab = tab_for_node(node)) {
        activate(*tab);
        return;
    }
    auto const* favorite = SpaceStore::the().favorite(favorite_id);
    if (!favorite)
        return;
    auto* tab = open_tab(favorite->url);
    if (!tab)
        return;
    set_tab_node(*tab, node);
    set_tab_space(*tab, {});
    schedule_rebuild();
}

void ArcSidebar::pin_tab(Tab& tab, QString const& parent_id, int index)
{
    auto id = SpaceStore::the().add_pinned_tab(m_space_id, parent_id, index, url_of(tab), title_of(tab));
    if (id.isEmpty())
        return;
    set_tab_node(tab, id);
    set_tab_space(tab, m_space_id);
    schedule_rebuild();
    save_session();
}

void ArcSidebar::unpin_node(QString const& node_id)
{
    auto location = SpaceStore::the().location_of(node_id);
    if (auto* tab = tab_for_node(node_id)) {
        set_tab_node(*tab, {});
        set_tab_space(*tab, location.is_valid() ? location.space_id : m_space_id);
    }
    SpaceStore::the().remove_node(node_id);
    save_session();
}

void ArcSidebar::add_favorite(Tab& tab, int index)
{
    auto id = SpaceStore::the().add_favorite(url_of(tab), title_of(tab), index, current_profile());
    // A Today tab becomes the favorite's tab; a pinned tab stays pinned.
    if (node_of(tab).isEmpty()) {
        set_tab_node(tab, favorite_node(id));
        set_tab_space(tab, {});
    }
    schedule_rebuild();
    save_session();
}

void ArcSidebar::move_to_space(QModelIndex const& index, QString const& space_id)
{
    auto& store = SpaceStore::the();
    auto kind = row_kind(index);
    Tab* current = m_window->current_tab();
    bool moved_current = false;

    if (kind == RowKind::TodayTab) {
        if (auto* tab = tab_in_row(index)) {
            set_tab_space(*tab, space_id);
            moved_current = tab == current;
        }
    } else if (kind == RowKind::PinnedTab || kind == RowKind::Folder) {
        store.move_node(index.data(IdRole).toString(), space_id, {}, -1);
        // Tabs bound to anything that moved follow it.
        for (auto* tab : tabs()) {
            auto node = node_of(*tab);
            if (node.isEmpty() || is_favorite_node(node))
                continue;
            auto location = store.location_of(node);
            if (location.is_valid() && location.space_id != space_of(*tab)) {
                set_tab_space(*tab, location.space_id);
                moved_current |= tab == current;
            }
        }
    }

    save_session();
    if (moved_current)
        switch_to_space(space_id);
    else
        schedule_rebuild();
}

void ArcSidebar::new_folder(QString const& parent_id, int index)
{
    bool ok = false;
    auto title = QInputDialog::getText(m_window, "New Folder", "Folder name:", QLineEdit::Normal, "New Folder", &ok);
    if (!ok)
        return;
    SpaceStore::the().add_folder(m_space_id, parent_id, index, title.trimmed().isEmpty() ? "New Folder" : title.trimmed());
}

void ArcSidebar::rename_node(QString const& node_id)
{
    auto const* node = SpaceStore::the().node(node_id);
    if (!node)
        return;
    auto current = node->title;
    if (current.isEmpty()) {
        auto* tab = tab_for_node(node_id);
        current = tab ? title_of(*tab) : node->saved_title;
    }
    bool ok = false;
    auto label = node->is_folder() ? "Folder name:" : "Tab name (leave empty to use the page title):";
    auto title = QInputDialog::getText(m_window, "Rename", label, QLineEdit::Normal, current, &ok);
    // The dialog runs its own event loop, in which the store can change: look the node up again.
    node = SpaceStore::the().node(node_id);
    if (!ok || !node)
        return;
    if (node->is_folder() && title.trimmed().isEmpty())
        return;
    SpaceStore::the().rename_node(node_id, title.trimmed());
}

static int count_tabs(QList<PinnedNode> const& nodes)
{
    int count = 0;
    for (auto const& node : nodes)
        count += node.is_folder() ? count_tabs(node.children) : 1;
    return count;
}

void ArcSidebar::delete_folder(QString const& node_id)
{
    auto& store = SpaceStore::the();
    auto const* node = store.node(node_id);
    if (!node || !node->is_folder())
        return;

    auto tab_count = count_tabs(node->children);
    if (tab_count > 0) {
        auto answer = QMessageBox::question(m_window, "Delete Folder",
            QString("Delete \"%1\" and the %2 pinned tab(s) inside it?\n\nOpen tabs from this folder move to Today.").arg(node->title).arg(tab_count));
        if (answer != QMessageBox::Yes)
            return;
    }

    // Open tabs from the folder become Today tabs.
    for (auto* tab : tabs()) {
        auto bound = node_of(*tab);
        if (bound.isEmpty() || is_favorite_node(bound))
            continue;
        for (auto location = store.location_of(bound); location.is_valid() && !location.parent_id.isEmpty(); location = store.location_of(location.parent_id)) {
            if (location.parent_id == node_id) {
                set_tab_node(*tab, {});
                break;
            }
        }
    }
    store.remove_node(node_id);
    save_session();
}

// A grid of monochrome symbols to pick a space icon from (plus "first letter").
class IconPicker final : public QWidget {
public:
    IconPicker(QString const& current, QWidget* parent)
        : QWidget(parent)
    {
        auto* grid = new QGridLayout(this);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(2);
        m_group = new QButtonGroup(this);
        m_group->setExclusive(true);

        int index = 0;
        auto add = [&](QString const& icon, QString const& label, QString const& tooltip) {
            auto* button = new SymbolButton(this);
            button->setCheckable(true);
            button->set_icon_text(label);
            button->setToolTip(tooltip);
            button->setProperty("arc_icon", icon);
            if (icon == current)
                button->setChecked(true);
            m_group->addButton(button);
            grid->addWidget(button, index / 10, index % 10);
            ++index;
        };

        add({}, "Aa", "First letter of the name");
        if (!current.isEmpty() && !is_symbol_icon(current))
            add(current, current, "Current icon");
        for (auto const& name : space_symbol_names()) {
            if (fitted_symbol_pixmap(name, ICON_BOX, Qt::black, 1).isNull())
                continue; // Not available on this macOS version.
            add("sf:" + name, "sf:" + name, name);
        }
        if (!m_group->checkedButton() && !m_group->buttons().isEmpty())
            m_group->buttons().first()->setChecked(true);
    }

    QString icon() const
    {
        auto* button = m_group->checkedButton();
        return button ? button->property("arc_icon").toString() : QString();
    }

private:
    QButtonGroup* m_group { nullptr };
};

static QDialog* make_space_dialog(QWidget* parent, QString const& title, QString const& icon, QColor const& color, QLineEdit*& name_edit, IconPicker*& icon_picker, QComboBox*& color_box)
{
    auto* dialog = new QDialog(parent);
    dialog->setWindowTitle("Space");
    auto* form = new QFormLayout(dialog);

    name_edit = new QLineEdit(title, dialog);
    icon_picker = new IconPicker(icon, dialog);
    color_box = new QComboBox(dialog);
    for (auto const& preset : COLOR_PRESETS) {
        QPixmap swatch(14, 14);
        swatch.fill(QColor(preset.hex));
        color_box->addItem(QIcon(swatch), preset.name, QString(preset.hex));
        if (QColor(preset.hex) == color)
            color_box->setCurrentIndex(color_box->count() - 1);
    }

    form->addRow("Name", name_edit);
    form->addRow("Icon", icon_picker);
    form->addRow("Color", color_box);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    form->addRow(buttons);
    return dialog;
}

void ArcSidebar::new_space()
{
    KeepSidebarOpen keep_open(this);
    auto& store = SpaceStore::the();
    auto preset = COLOR_PRESETS[store.spaces().size() % std::size(COLOR_PRESETS)];

    QLineEdit* name_edit = nullptr;
    IconPicker* icon_picker = nullptr;
    QComboBox* color_box = nullptr;
    auto* dialog = make_space_dialog(m_window, "New Space", "sf:star", QColor(preset.hex), name_edit, icon_picker, color_box);
    dialog->setWindowTitle("New Space");
    name_edit->selectAll();
    if (dialog->exec() == QDialog::Accepted) {
        auto name = name_edit->text().trimmed();
        auto id = store.add_space(name.isEmpty() ? "New Space" : name, icon_picker->icon(), QColor(color_box->currentData().toString()));
        switch_to_space(id);
    }
    dialog->deleteLater();
}

void ArcSidebar::edit_space(QString const& space_id)
{
    auto& store = SpaceStore::the();
    auto const* space = store.space(space_id);
    if (!space)
        return;

    QLineEdit* name_edit = nullptr;
    IconPicker* icon_picker = nullptr;
    QComboBox* color_box = nullptr;
    auto* dialog = make_space_dialog(m_window, space->title, space->icon, space->color, name_edit, icon_picker, color_box);
    auto accepted = dialog->exec() == QDialog::Accepted;
    space = store.space(space_id); // The dialog's event loop can change the store.
    if (accepted && space) {
        auto name = name_edit->text().trimmed();
        store.update_space(space_id, name.isEmpty() ? space->title : name, icon_picker->icon(), QColor(color_box->currentData().toString()));
    }
    dialog->deleteLater();
}

void ArcSidebar::delete_space(QString const& space_id)
{
    auto& store = SpaceStore::the();
    auto const* space = store.space(space_id);
    if (!space || store.spaces().size() <= 1)
        return;

    auto answer = QMessageBox::question(m_window, "Delete Space",
        QString("Delete the space \"%1\" and its pinned tabs and folders?\n\nIts open tabs move to another space.").arg(space->title));
    // The dialog runs its own event loop, in which the store can change.
    if (answer != QMessageBox::Yes || !store.space(space_id) || store.spaces().size() <= 1)
        return;

    QString fallback;
    for (auto const& other : store.spaces()) {
        if (other.id != space_id) {
            fallback = other.id;
            break;
        }
    }

    for (auto* tab : tabs()) {
        if (space_of(*tab) == space_id) {
            set_tab_space(*tab, fallback);
            if (!is_favorite_node(node_of(*tab)))
                set_tab_node(*tab, {});
        }
    }

    store.remove_space(space_id);
    save_session();
    if (m_space_id == space_id)
        switch_to_space(fallback);
}

// --- Spaces -------------------------------------------------------------------------------------

void ArcSidebar::switch_to_space_at(int index)
{
    auto const& spaces = SpaceStore::the().spaces();
    if (index >= 0 && index < spaces.size())
        switch_to_space(spaces[index].id);
}

// The tint the windows show, set by apply_tint().
static QColor s_applied_tint;
static double s_applied_tint_amount = 0;
static bool s_tint_initialized = false;

// Where the slide between two spaces happens: the favorites and the list, which move as one page.
QRect ArcSidebar::space_slide_area() const
{
    auto* top_widget = m_favorites->isVisible() ? m_favorites : m_header;
    auto top = top_widget->y();
    return QRect(0, top, m_panel->width(), m_tree->geometry().bottom() + 1 - top);
}

// The list's scroll bar shows only while it's in use, as macOS's overlay scroll bars (and Arc's) do: scrolling or
// pointing at it brings it in, and it fades a moment after.
static constexpr int SCROLLBAR_FADE_IN_MS = 120;
static constexpr int SCROLLBAR_FADE_OUT_MS = 300;
static constexpr int SCROLLBAR_LINGER_MS = 900;

// Its handle's color carries the fade (an opacity effect would darken the see-through sidebar around it on macOS).
void ArcSidebar::set_scrollbar_opacity(qreal opacity)
{
    m_scrollbar_opacity = opacity;
    if (auto* bar = qobject_cast<SidebarScrollBar*>(m_tree->verticalScrollBar()))
        bar->set_opacity(opacity);
}

void ArcSidebar::fade_scrollbar_to(qreal opacity, int duration_ms)
{
    if (!m_scrollbar_fade) {
        m_scrollbar_fade = new QVariantAnimation(this);
        connect(m_scrollbar_fade, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
            set_scrollbar_opacity(value.toReal());
        });
    }
    m_scrollbar_fade->stop();
    m_scrollbar_fade->setDuration(duration_ms);
    m_scrollbar_fade->setStartValue(m_scrollbar_opacity);
    m_scrollbar_fade->setEndValue(opacity);
    m_scrollbar_fade->start();
}

void ArcSidebar::show_scrollbar_briefly()
{
    if (SpaceSlide::running_in(*m_panel))
        return; // It comes in once the slide is over.
    if (!m_scrollbar_linger) {
        m_scrollbar_linger = new QTimer(this);
        m_scrollbar_linger->setSingleShot(true);
        m_scrollbar_linger->setInterval(SCROLLBAR_LINGER_MS);
        connect(m_scrollbar_linger, &QTimer::timeout, this, [this] {
            if (m_tree->verticalScrollBar()->underMouse())
                return; // Leave restarts it.
            fade_scrollbar_to(0, SCROLLBAR_FADE_OUT_MS);
        });
    }
    if (m_scrollbar_opacity < 1 && (!m_scrollbar_fade || m_scrollbar_fade->state() != QAbstractAnimation::Running || m_scrollbar_fade->endValue().toReal() < 1))
        fade_scrollbar_to(1, SCROLLBAR_FADE_IN_MS);
    m_scrollbar_linger->start();
}

void ArcSidebar::hide_scrollbar_for_slide()
{
    if (m_scrollbar_fade)
        m_scrollbar_fade->stop();
    if (m_scrollbar_linger)
        m_scrollbar_linger->stop();
    set_scrollbar_opacity(0);
}

void ArcSidebar::switch_to_space(QString const& id)
{
    auto& store = SpaceStore::the();
    if (!store.space(id))
        return;

    // Slide from the old space to the new one, when there's a sidebar to see it in.
    if (auto* running = SpaceSlide::running_in(*m_panel))
        running->finish();
    QPixmap before;
    QRect slide_area;
    auto slide = id != m_space_id && m_window && m_window->isVisible() && m_panel->isVisible() && m_panel->width() > 0;
    if (slide) {
        slide_area = space_slide_area();
        slide = slide_area.isValid();
        if (slide) {
            hide_scrollbar_for_slide();
            before = SpaceSlide::picture(*m_panel, slide_area);
        }
    }
    int direction = 1;
    for (auto const& space : store.spaces()) {
        if (space.id == m_space_id)
            break; // The old space comes first: the new one is to its right.
        if (space.id == id) {
            direction = -1;
            break;
        }
    }

    // The window keeps the old space's tint while the slide runs, matching the pictures' backgrounds; the new one's
    // comes in when it ends.
    if (slide) {
        m_holding_tint = true;
        if (m_tint_fade)
            m_tint_fade->stop(); // The window stays at whatever tint it got to; the next fade starts from there.
    }

    m_space_id = id;
    store.set_last_active_space_id(id);
    apply_profile_for_new_views(); // Before a new-tab page might open below.

    // Show the tab last used in this space, else any of its tabs, else a fresh new-tab page.
    Tab* target = m_last_tab_in_space.value(id);
    if (target && (m_tabs.index_of(target) < 0 || space_of(*target) != id))
        target = nullptr;
    if (!target) {
        auto today = today_tabs(id);
        if (!today.isEmpty())
            target = today.last();
    }
    if (!target) {
        for (auto* tab : tabs()) {
            if (space_of(*tab) == id) {
                target = tab;
                break;
            }
        }
    }

    m_switching_space = true;
    if (target) {
        activate(*target);
    } else {
        auto& tab = m_window->new_tab_from_url(WebView::Application::settings().new_tab_page_url(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
        set_tab_space(tab, id);
        tab.set_url_is_hidden(true);
        tab.focus_location_editor();
    }
    m_switching_space = false;

    rebuild();

    if (slide) {
        auto* running = SpaceSlide::start(*m_panel, slide_area, before, direction);
        connect(running, &SpaceSlide::finished, this, [this] {
            m_holding_tint = false;
            fade_to_space_tint();
            QTimer::singleShot(0, this, [this] { show_scrollbar_briefly(); }); // As in Arc, a new space's scroll bar flashes in, then fades.
        });
    }
}

void ArcSidebar::adopt_tab(BrowserWindow& from, Tab& tab, QString const& space_id)
{
    auto index = from.tab_index(&tab);
    if (index < 0 || !m_window)
        return;
    QPointer<Tab> guarded = &tab;
    from.move_tab_to_window(index, *m_window, m_tabs.count());
    if (!guarded)
        return;
    set_tab_node(*guarded, {});
    set_tab_space(*guarded, space_id);
    // Shown first: the sidebar follows it into its space (and, for another profile's space, reopens it there, shown).
    activate(*guarded);
    m_window->show();
    m_window->raise();
    m_window->activateWindow();
    schedule_rebuild();
}

// The most recently shown tab still open in `space` (or a favorite's), other than `except`.
Tab* ArcSidebar::most_recent_tab_in_space(QString const& space, Tab const* except)
{
    m_recent_tabs.removeAll(nullptr);
    for (auto const& recent : m_recent_tabs) {
        if (!recent || recent == except || m_tabs.index_of(recent) < 0 || is_blank(*recent))
            continue;
        auto recent_space = space_of(*recent);
        // A favorite's tab is in no space: one of this profile's favorites will do.
        if (recent_space == space || (recent_space.isEmpty() && is_favorite_node(node_of(*recent)) && profile_of_tab(*recent) == current_profile()))
            return recent;
    }
    return nullptr;
}

void ArcSidebar::on_current_tab_changed(int index)
{
    auto* tab = m_tabs.tab(index);
    if (!tab)
        return;
    if (!tab->property(SPACE_PROPERTY).isValid())
        set_tab_space(*tab, m_space_id);

    // Upstream removes a closing tab from the page stack before picking a neighbor, so a previous tab
    // that's gone from the stack means this change came from a close (or a move to another window).
    QPointer<Tab> previous = m_current_tab;
    m_current_tab = tab;
    auto previous_was_removed = previous && m_tabs.index_of(previous) < 0;

    m_recent_tabs.removeAll(tab);
    m_recent_tabs.prepend(tab);
    restyle_if_tint_changed(*tab);
    update_auto_picture_in_picture(previous, *tab);

    auto space = space_of(*tab);
    if (!m_switching_space && !space.isEmpty() && space != m_space_id && SpaceStore::the().space(space)) {
        if (previous_was_removed) {
            // Arc stays in the current space when its last tab closes, rather than jumping to a neighbor's space.
            QTimer::singleShot(0, this, [this, stay = m_space_id] { switch_to_space(stay); });
            return;
        }
        // Otherwise follow the tab into its space (e.g. Ctrl+Tab onto a tab from another space).
        m_space_id = space;
        SpaceStore::the().set_last_active_space_id(space);
        schedule_rebuild();
    }
    if (!space.isEmpty())
        m_last_tab_in_space[space] = tab;
    QTimer::singleShot(0, this, [guarded = QPointer(this)] {
        if (guarded)
            guarded->close_stale_blank_tabs();
    });

    update_current_highlight();
    rebuild_favorites();
}

// --- Views --------------------------------------------------------------------------------------

void ArcSidebar::style_update_button()
{
    auto dark = ChromeStyle::is_dark(palette());
    m_update_button->setStyleSheet(QString(
        "QPushButton#ArcUpdateButton { background: %1; border: 1px solid %2; border-radius: %3px; color: %4;"
        " font-size: 13px; font-weight: 500; margin: 0 2px 6px 2px; }"
        "QPushButton#ArcUpdateButton:hover { background: %5; }")
            .arg(dark ? "rgba(255,255,255,0.10)" : "rgba(255,255,255,0.55)",
                dark ? "rgba(255,255,255,0.16)" : "rgba(0,0,0,0.10)",
                QString::number((UPDATE_BUTTON_HEIGHT - 6) / 2),
                dark ? "rgba(255,255,255,0.92)" : "rgba(0,0,0,0.80)",
                dark ? "rgba(255,255,255,0.16)" : "rgba(255,255,255,0.80)"));
}

QString ArcSidebar::current_profile() const
{
    return profile_of_space(m_space_id);
}

QString ArcSidebar::profile_of_space(QString const& space_id)
{
    auto const* space = SpaceStore::the().space(space_id);
    return space ? space->profile : QString();
}

// A tab's profile: its favorite's (favorites belong to a profile), else its space's.
QString ArcSidebar::profile_of_tab(Tab const& tab)
{
    auto node = node_of(tab);
    if (is_favorite_node(node)) {
        if (auto const* favorite = SpaceStore::the().favorite(node.mid(4)))
            return favorite->profile;
    }
    return profile_of_space(space_of(tab));
}

// New tabs (links, the command bar, Cmd+T) open in the profile of the space shown in the active window.
void ArcSidebar::apply_profile_for_new_views()
{
    if (m_window && (m_window->isActiveWindow() || !QApplication::activeWindow()))
        set_profile_for_new_views(current_profile());
}

// A tab's cookies and site data come from the session its page was created in, which can't change. A tab whose
// profile differs from its page's (moved to another profile's space, or opened before its space was known) is
// reopened in its profile, at the same place. Blank pages hold no site data, so they stay.
void ArcSidebar::place_tabs_in_their_profiles()
{
    for (auto* tab : tabs()) {
        auto profile = profile_of_tab(*tab);
        auto expected = session_for_profile(profile);
        auto* expected_session = expected ? expected.ptr() : &WebView::Application::default_session();
        if (&tab->view().session() == expected_session || tab->view().is_private() == WebView::IsPrivate::Yes || is_blank(*tab) || tab->property("arc_replaced").toBool())
            continue;

        auto url = tab->view().url();
        auto index = m_tabs.index_of(tab);
        auto was_current = m_window->current_tab() == tab;
        auto space = space_of(*tab);
        auto node = node_of(*tab);
        auto home = tab->property(PINNED_HOME_PROPERTY);
        set_tab_node(*tab, {}); // So the reopened tab is the one bound to the node.
        // Reopen it once: its close can wait on the page (a "leave page?" prompt), and until then every rebuild would
        // reopen it again.
        tab->setProperty("arc_replaced", true);

        Tab* reopened = nullptr;
        {
            ScopedProfileForNewViews scope(profile);
            reopened = &m_window->new_tab_from_url(url, was_current ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No, BrowserWindow::TabLocation::at_index(index + 1));
        }
        set_tab_space(*reopened, space);
        set_tab_node(*reopened, node);
        reopened->setProperty(PINNED_HOME_PROPERTY, home);
        if (auto old_index = m_tabs.index_of(tab); old_index >= 0)
            m_window->request_to_close_tab(old_index);
    }
}

QColor ArcSidebar::space_color() const
{
    auto const* space = SpaceStore::the().space(m_space_id);
    return space ? space->color : QColor();
}


static QIcon letter_icon(QString const& host)
{
    static QHash<QString, QIcon> cache;
    if (auto it = cache.find(host); it != cache.end())
        return *it;

    auto letter = host.isEmpty() ? QString("?") : host.section('.', -2, -2).left(1).toUpper();
    if (letter.isEmpty())
        letter = host.left(1).toUpper();
    auto hue = static_cast<int>(qHash(host) % 360);

    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor::fromHsl(hue, 110, 120));
    painter.drawRoundedRect(QRectF(0, 0, 64, 64), 14, 14);
    auto font = painter.font();
    font.setPixelSize(38);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRect(0, 0, 64, 64), Qt::AlignCenter, letter);
    painter.end();

    QIcon icon(pixmap);
    cache.insert(host, icon);
    return icon;
}

QIcon ArcSidebar::icon_for(QString const& url, Tab const* tab) const
{
    // A loaded tab's live favicon (remembered for its site, so the site keeps it once unloaded); otherwise the
    // site's cached icon (from browsing, or imported from Arc); otherwise a letter tile.
    if (tab && !tab->favicon().isNull()) {
        if (!is_private()) // Private browsing leaves nothing behind.
            ArcIcons::store(url, tab->favicon());
        return tab->favicon();
    }
    if (auto cached = ArcIcons::icon_for_url(url); !cached.isNull())
        return cached;
    return letter_icon(QUrl(url).host());
}

// Empty sidebar space outside the list (between sections, around the favorites): also the command bar.
void ArcSidebar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_window) {
        CommandBar::toggle_for(*m_window);
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void ArcSidebar::schedule_rebuild()
{
    if (m_rebuild_pending)
        return;
    m_rebuild_pending = true;
    QTimer::singleShot(0, this, [this] { rebuild(); });
}

void ArcSidebar::rebuild()
{
    // Don't pull the rows out from under an active drag.
    if (m_tree->is_dragging()) {
        m_rebuild_pending = true;
        QTimer::singleShot(100, this, [this] { rebuild(); });
        return;
    }
    m_rebuild_pending = false;

    place_tabs_in_their_profiles();
    apply_profile_for_new_views();

    auto const* space = SpaceStore::the().space(m_space_id);
    if (!space) {
        m_space_id = SpaceStore::the().first_space_id();
        space = SpaceStore::the().space(m_space_id);
    }

    // Tabs whose space went away (deleted, or the starter space an import replaced) join the current one. Tabs whose
    // pinned tab or favorite went away (removed in another window, replaced by a re-import) become Today tabs, rather
    // than running on unreachable.
    for (auto* tab : tabs()) {
        auto tab_space = space_of(*tab);
        if (!tab_space.isEmpty() && !SpaceStore::the().space(tab_space)) {
            set_tab_space(*tab, m_space_id);
            if (!is_favorite_node(node_of(*tab)))
                set_tab_node(*tab, {});
        }
        auto node = node_of(*tab);
        if (node.isEmpty())
            continue;
        if (is_favorite_node(node) ? !SpaceStore::the().favorite(node.mid(4)) : !SpaceStore::the().node(node)) {
            set_tab_node(*tab, {});
            if (space_of(*tab).isEmpty())
                set_tab_space(*tab, m_space_id);
        }
    }

    auto muted = ChromeStyle::chrome_muted_text(palette());
    m_space_title->setText(space ? space->title : QString());
    // Only when they change: setting a style sheet re-polishes the header, and this runs on every rebuild.
    auto title_style = QString("QLabel { color: %1; font-weight: 600; font-size: 13px; }").arg(muted.name());
    if (m_space_title->styleSheet() != title_style)
        m_space_title->setStyleSheet(title_style);
    auto hover = ChromeStyle::is_dark(palette()) ? "rgba(255,255,255,0.07)" : "rgba(0,0,0,0.05)";
    auto header_style = QString("QWidget#ArcSpaceHeader { border-radius: 9px; background: transparent; }"
                                "QWidget#ArcSpaceHeader[hovered=\"true\"] { background: %1; }")
                            .arg(hover);
    if (m_header->styleSheet() != header_style)
        m_header->setStyleSheet(header_style);
    update_space_header_icon();
    m_tree->delegate().accent = space_color();

    rebuild_favorites();
    rebuild_tree();
    rebuild_switcher();
    update();
    if (window()->isActiveWindow() || !qApp->activeWindow())
        apply_space_tint();
}

void ArcSidebar::update_space_header_icon()
{
    auto const* space = SpaceStore::the().space(m_space_id);
    if (!space)
        return;
    auto collapsed = m_collapsed_spaces.contains(m_space_id);
    if (m_header_hovered || collapsed) {
        m_space_icon->set_symbol(collapsed ? "chevron.right" : "chevron.down");
        m_space_icon->setToolTip(collapsed ? "Show Pinned Tabs" : "Hide Pinned Tabs");
    } else {
        m_space_icon->set_icon_text(space->icon.isEmpty() ? space->title.left(1).toUpper() : space->icon);
        m_space_icon->setToolTip({});
    }
    m_space_icon->set_dimmed(true);
}

void ArcSidebar::rebuild_favorites()
{
    auto favorites = SpaceStore::the().favorites_for_profile(current_profile());
    m_favorites->setVisible(!favorites.isEmpty());

    auto dark = ChromeStyle::is_dark(palette());
    auto tile = dark ? "rgba(255,255,255,0.09)" : "rgba(255,255,255,0.45)";
    auto tile_hover = dark ? "rgba(255,255,255,0.14)" : "rgba(255,255,255,0.65)";
    auto tile_checked = dark ? "rgba(255,255,255,0.24)" : "rgba(255,255,255,0.95)";
    auto style = QString("QToolButton { background: %1; border: none; border-radius: 12px; }"
                         "QToolButton:hover { background: %2; }"
                         "QToolButton:checked { background: %3; }")
                     .arg(tile, tile_hover, tile_checked);

    QStringList ids;
    for (auto const& favorite : favorites)
        ids.append(favorite.id);

    // Rebuilding the tiles empties the grid for a moment, which resizes the list below it and can move its scroll
    // position. Only rebuild when the tiles themselves change; otherwise update them in place.
    if (ids != m_favorite_ids || style != m_favorite_style || m_favorite_buttons.size() != ids.size()) {
        // Hidden now, not when they're deleted: until then they'd show under the new tiles.
        while (auto* item = m_favorites_layout->takeAt(0)) {
            if (auto* widget = item->widget()) {
                widget->hide();
                widget->deleteLater();
            }
            delete item;
        }
        m_favorite_buttons.clear();
        m_favorite_ids = ids;
        m_favorite_style = style;

        // Always four equal columns, like Arc, so one favorite is a tile rather than a full-width bar.
        constexpr int columns = 4;
        for (int column = 0; column < columns; ++column)
            m_favorites_layout->setColumnStretch(column, 1);

        for (int i = 0; i < favorites.size(); ++i) {
            auto* button = new QToolButton(m_favorites);
            button->setIconSize(QSize(16, 16));
            button->setCheckable(true);
            button->setFixedHeight(FAVORITE_TILE_HEIGHT);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            button->setFocusPolicy(Qt::NoFocus);
            button->setStyleSheet(style);
            button->setContextMenuPolicy(Qt::CustomContextMenu);

            auto id = favorites[i].id;
            connect(button, &QToolButton::clicked, this, [this, id] { open_or_activate_favorite(id); });
            connect(button, &QToolButton::customContextMenuRequested, this, [this, id, button](QPoint const& position) {
                show_favorite_menu(id, button->mapToGlobal(position));
            });

            m_favorites_layout->addWidget(button, i / columns, i % columns);
            m_favorite_buttons.append(button);
        }
        // Shown and placed now, rather than on a later turn of the event loop at a default position.
        if (m_favorites->isVisible()) {
            for (auto const& button : m_favorite_buttons)
                button->show();
            m_favorites_layout->activate();
        }
    }

    auto* current = m_window ? m_window->current_tab() : nullptr;
    for (int i = 0; i < favorites.size(); ++i) {
        auto const& favorite = favorites[i];
        auto* button = m_favorite_buttons[i].data();
        if (!button)
            continue;
        auto* tab = tab_for_node(favorite_node(favorite.id));
        // Only what changed: this runs on every tab switch, and a new icon relays out the tiles.
        auto icon = icon_for(favorite.url, tab && tab_shows_a_page(url_of(*tab)) ? tab : nullptr);
        if (button->icon().cacheKey() != icon.cacheKey())
            button->setIcon(icon);
        auto tooltip = favorite.title.isEmpty() ? favorite.url : favorite.title;
        if (button->toolTip() != tooltip)
            button->setToolTip(tooltip);
        auto checked = tab && tab == current;
        if (button->isChecked() != checked)
            button->setChecked(checked);
    }
}

void ArcSidebar::append_nodes(QStandardItem& parent, QList<PinnedNode> const& nodes)
{
    for (auto const& node : nodes) {
        auto* item = new QStandardItem;
        item->setData(node.id, IdRole);

        if (node.is_folder()) {
            item->setText(node.title.isEmpty() ? "Untitled" : node.title);
            item->setData(static_cast<int>(RowKind::Folder), KindRole);
            item->setData(node.expanded, ExpandedRole);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            append_nodes(*item, node.children);
        } else {
            auto* tab = tab_for_node(node.id);
            auto tab_url = tab ? url_of(*tab) : QString();
            // Until a loading tab shows its page, keep what the row showed unloaded (no "New Tab" flash).
            item->setText(pinned_row_title(node.title, node.saved_title, tab ? live_title(*tab) : QString(), node.url, tab_url));
            item->setIcon(icon_for(tab_shows_a_page(tab_url) ? tab_url : node.url, tab_shows_a_page(tab_url) ? tab : nullptr));
            item->setData(static_cast<int>(RowKind::PinnedTab), KindRole);
            item->setData(tab != nullptr, IsLoadedRole);
            item->setData(tab && pinned_is_away(*tab, node.url), NavigatedAwayRole);
            item->setData(QVariant::fromValue(reinterpret_cast<quintptr>(tab)), TabRole);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        }
        parent.appendRow(item);
    }
}

void ArcSidebar::rebuild_tree()
{
    // Each space keeps its own scroll position, as in Arc; refreshing the same space never moves the list.
    auto switching_space = m_tree_space_id != m_space_id;
    if (switching_space && !m_tree_space_id.isEmpty())
        m_scroll_positions.insert(m_tree_space_id, m_tree->verticalScrollBar()->value());
    m_tree_space_id = m_space_id;

    m_tree->rebuild_keeping_scroll_position([this] { fill_tree(); });
    if (switching_space)
        m_tree->verticalScrollBar()->setValue(m_scroll_positions.value(m_space_id, 0));
}

void ArcSidebar::fill_tree()
{
    m_model->clear();
    m_node_tabs = QHash<QString, Tab*> {};
    for (auto* tab : tabs()) {
        if (auto node = node_of(*tab); !node.isEmpty() && !m_node_tabs->contains(node))
            m_node_tabs->insert(node, tab);
    }
    ScopeGuard forget_node_tabs = [this] { m_node_tabs.clear(); };

    auto const* space = SpaceStore::the().space(m_space_id);
    if (!space)
        return;

    auto& root = *m_model->invisibleRootItem();
    append_nodes(root, space->pinned);

    auto pinned_rows = root.rowCount();
    auto* new_tab = new QStandardItem;
    new_tab->setData(static_cast<int>(RowKind::NewTab), KindRole);
    new_tab->setFlags(Qt::ItemIsEnabled);
    root.appendRow(new_tab);

    for (auto* tab : today_tabs(m_space_id)) {
        auto* item = new QStandardItem(title_of(*tab));
        item->setIcon(icon_for(url_of(*tab), tab));
        item->setData(static_cast<int>(RowKind::TodayTab), KindRole);
        item->setData(QVariant::fromValue(reinterpret_cast<quintptr>(tab)), TabRole);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        root.appendRow(item);
    }

    // Empty room after the last row, scrolling with the list, to double-click for a new tab (as in Arc).
    auto* bottom_space = new QStandardItem;
    bottom_space->setData(static_cast<int>(RowKind::BottomSpace), KindRole);
    bottom_space->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
    root.appendRow(bottom_space);

    for_each_item(root, [&](QStandardItem& item) {
        if (row_kind(item.index()) == RowKind::Folder)
            m_tree->setExpanded(item.index(), item.data(ExpandedRole).toBool());
    });

    auto collapsed = m_collapsed_spaces.contains(m_space_id);
    for (int row = 0; row < pinned_rows; ++row)
        m_tree->setRowHidden(row, QModelIndex(), collapsed);

    update_current_highlight();
    if (m_rename_editor)
        QTimer::singleShot(0, this, [this] { place_inline_rename(); }); // Once the rows are laid out.
}

void ArcSidebar::show_shelf()
{
    m_shelf->refresh();
    if (m_shelf->is_empty())
        return;

    // Slide up a little and fade in, like Arc.
    position_shelf();
    auto end = m_shelf->pos();
    m_shelf_animation->stop();
    m_shelf_position_animation->setStartValue(end + QPoint(0, SHELF_SLIDE_DISTANCE));
    m_shelf_position_animation->setEndValue(end);
    m_shelf_opacity_animation->setStartValue(m_shelf->isVisible() ? m_shelf->opacity() : 0.0);
    m_shelf_opacity_animation->setEndValue(1.0);
    m_shelf_animation->setDirection(QAbstractAnimation::Forward);
    m_shelf_hiding = false;
    // Its rows rise in one by one as it opens (not again while it's already showing).
    if (!m_shelf->isVisible() || m_shelf->opacity() < 0.5)
        m_shelf->reveal_rows();
    m_shelf->move(end + QPoint(0, SHELF_SLIDE_DISTANCE));
    m_shelf->raise();
    m_shelf->show();
    m_shelf_animation->start();
    m_shelf_refresh_timer->start();
}

void ArcSidebar::hide_shelf()
{
    if (!m_shelf->isVisible() || m_shelf_hiding)
        return;
    m_shelf_hiding = true;
    auto start = m_shelf->pos();
    m_shelf_animation->stop();
    m_shelf_position_animation->setStartValue(start);
    m_shelf_position_animation->setEndValue(start + QPoint(0, SHELF_SLIDE_DISTANCE / 2));
    m_shelf_opacity_animation->setStartValue(m_shelf->opacity());
    m_shelf_opacity_animation->setEndValue(0.0);
    m_shelf_animation->start();
}

void ArcSidebar::position_shelf()
{
    auto switcher_top = m_switcher->mapTo(m_panel, QPoint(0, 0)).y();
    auto height = qMin(m_shelf->preferred_height(), switcher_top - TOP_STRIP_HEIGHT);
    m_shelf->setGeometry(m_switcher->x(), switcher_top - height, m_switcher->width(), height);
}

void ArcSidebar::rebuild_switcher()
{
    auto const& spaces = SpaceStore::the().spaces();

    // Switching spaces only changes which button is lit: the buttons stay put. Rebuilding them on every switch
    // briefly showed the new ones at the wrong places, lighting up whichever was under the pointer.
    QStringList key;
    for (auto const& space : spaces)
        key << QString("%1\n%2\n%3").arg(space.id, space.icon, space.title);
    if (key == m_switcher_key && m_space_buttons.size() == spaces.size()) {
        for (int i = 0; i < spaces.size(); ++i) {
            if (m_space_buttons[i])
                m_space_buttons[i]->set_dimmed(spaces[i].id != m_space_id);
        }
        return;
    }
    m_switcher_key = key;
    m_space_buttons.clear();

    // Everything but the downloads button is rebuilt.
    while (auto* item = m_switcher_layout->takeAt(0)) {
        if (auto* widget = item->widget(); widget && widget != m_downloads_button) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    // [downloads]  [space] [space] ...  [+], spread across the bar like Arc's.
    m_switcher_layout->addWidget(m_downloads_button);
    m_switcher_layout->addStretch(1);

    QList<QWidget*> added;
    for (int i = 0; i < spaces.size(); ++i) {
        auto const& space = spaces[i];
        auto active = space.id == m_space_id;

        auto* button = new SymbolButton(m_switcher);
        button->setObjectName("ArcSpaceButton");
        button->set_icon_text(space.icon.isEmpty() ? space.title.left(1).toUpper() : space.icon);
        button->set_dimmed(!active);
        button->setToolTip(i < 9 ? QString("%1  (%2%3)").arg(space.title, SPACE_SHORTCUT_MODIFIER == Qt::META ? "\u2303" : "Alt+").arg(i + 1) : space.title);
        button->setContextMenuPolicy(Qt::CustomContextMenu);

        auto id = space.id;
        connect(button, &QAbstractButton::clicked, this, [this, id] { switch_to_space(id); });
        connect(button, &QWidget::customContextMenuRequested, this, [this, id, button](QPoint const& position) {
            show_space_menu(id, button->mapToGlobal(position));
        });
        m_switcher_layout->addWidget(button);
        m_switcher_layout->addStretch(1);
        m_space_buttons.append(button);
        added.append(button);
    }

    auto* add = new SymbolButton(m_switcher);
    add->set_symbol("plus");
    add->set_dimmed(true);
    add->setToolTip("New Space");
    connect(add, &QAbstractButton::clicked, this, &ArcSidebar::new_space);
    m_switcher_layout->addWidget(add);
    added.append(add);

    // Shown and placed now, rather than on a later turn of the event loop at a default position.
    if (m_switcher->isVisible()) {
        for (auto* widget : added)
            widget->show();
        m_switcher_layout->activate();
    }
}

void ArcSidebar::update_tab_rows(Tab& tab)
{
    if (m_tabs.index_of(&tab) < 0)
        return;

    auto node = node_of(tab);
    if (is_favorite_node(node)) {
        rebuild_favorites();
        return;
    }

    auto title = title_of(tab);
    auto icon = icon_for(url_of(tab), &tab);
    // Only what changed: each change repaints the row, and this runs on every title, icon and load change.
    auto set_text = [](QStandardItem& item, QString const& text) {
        if (item.text() != text)
            item.setText(text);
    };
    auto set_icon = [](QStandardItem& item, QIcon const& icon) {
        if (item.icon().cacheKey() != icon.cacheKey())
            item.setIcon(icon);
    };
    for_each_item(*m_model->invisibleRootItem(), [&](QStandardItem& item) {
        if (tab_in_row(item.index()) != &tab)
            return;
        if (row_kind(item.index()) == RowKind::PinnedTab) {
            auto const* pinned = SpaceStore::the().node(node);
            if (pinned) {
                auto tab_url = url_of(tab);
                set_text(item, pinned_row_title(pinned->title, pinned->saved_title, live_title(tab), pinned->url, tab_url));
                if (!tab_shows_a_page(tab_url)) {
                    set_icon(item, icon_for(pinned->url, nullptr));
                    item.setData(false, NavigatedAwayRole);
                    return;
                }
            }
            auto away = pinned && pinned_is_away(tab, pinned->url);
            if (item.data(NavigatedAwayRole).toBool() != away)
                item.setData(away, NavigatedAwayRole);
        } else {
            set_text(item, title);
        }
        set_icon(item, icon);
    });

    // Not the address shown while it loads: that isn't the page's title.
    if (!node.isEmpty() && !title_is_placeholder(tab.title(), url_of(tab)) && tab_shows_a_page(url_of(tab)) && !is_private())
        SpaceStore::the().set_saved_title(node, tab.title());
}

void ArcSidebar::update_current_highlight()
{
    auto* current = m_window ? m_window->current_tab() : nullptr;
    for_each_item(*m_model->invisibleRootItem(), [&](QStandardItem& item) {
        auto is_current = current && tab_in_row(item.index()) == current;
        if (item.data(IsCurrentRole).toBool() != is_current)
            item.setData(is_current, IsCurrentRole);
    });
}

// --- Docked and hidden modes ---------------------------------------------------------------------
//
// Docked: the panel fills this widget inside upstream's sidebar column, and the real window controls show.
// Hidden (toolbar toggle): upstream shrinks the column to a thin edge. The panel moves into a floating
// host, a native child window that stacks above the page, which slides in when that edge is hovered.
// The real window controls are hidden; the panel draws its own, which slide with it.

void ArcSidebar::tab_settings_changed()
{
    // Upstream relayouts on the same notification; act once it's done.
    QTimer::singleShot(0, this, [this] { update_mode(); });
}

void ArcSidebar::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (auto* column = parentWidget(); column && column != m_watched_column) {
        if (m_watched_column)
            m_watched_column->removeEventFilter(this);
        m_watched_column = column;
        column->installEventFilter(this);
    }
    // The native window may not exist on the first pass; settle the mode once shown.
    QTimer::singleShot(0, this, [this] { update_mode(); });
}

void ArcSidebar::update_mode()
{
    if (!m_window)
        return;
    auto hidden = !WebView::Application::settings().tab_settings().vertical_tabs_expanded;
    auto mode = hidden ? Mode::Hidden : Mode::Docked;
    auto was_set = m_mode_set;
    auto previous = m_mode;
    m_mode_set = true;
    m_mode = mode;
    apply_toolbar_inset();

    if (m_active_transition != Transition::None)
        return; // The animation finishes the switch (dock_panel / prepare_card_panel).
    if (m_pending_transition != Transition::None && was_set && mode != previous) {
        start_transition();
        return;
    }
    m_pending_transition = Transition::None;

    if (was_set && mode == previous) {
        // Re-assert the real window controls' visibility (AppKit/Qt can restore them), unless an animation owns them.
        if (!m_host_docking && !m_host_lifting_off)
            set_window_controls_hidden(*window(), hidden);
        return;
    }

    if (mode == Mode::Docked) {
        dock_panel();
        return;
    }
    prepare_card_panel();
    set_window_controls_hidden(*window(), true);
}

// Upstream's toolbar has a 12px left margin, which leaves its sidebar button sitting in from the page's
// left edge. Trim it so the button lines up with the page, docked or hidden.
// Upstream sets each tab's toolbar margins itself (e.g. on tab switches). Correcting them afterwards let a new tab's
// toolbar show at upstream's margin for a frame, then jump. So each toolbar is watched and corrected as it asks for
// a layout, before it's laid out and painted.
static void inset_toolbar(QWidget& toolbar)
{
    auto* layout = toolbar.layout();
    if (!layout)
        return;
    auto margins = layout->contentsMargins();
    if (margins.left() != HIDDEN_TOOLBAR_LEFT_MARGIN) {
        margins.setLeft(HIDDEN_TOOLBAR_LEFT_MARGIN);
        layout->setContentsMargins(margins);
    }
}

// Arc's address bar in place of upstream's location field, once per toolbar. Clicking the URL edits it in the
// command bar (as Cmd+L does; see ArcChrome.cpp).
void ArcSidebar::install_address_bar(QWidget& toolbar)
{
    if (!m_window || toolbar.property("arc_address_bar").toBool())
        return;
    auto* edit = toolbar.findChild<LocationEdit*>();
    // Toolbars live in the tab widget's toolbar stack, not in their tabs.
    Tab* tab = nullptr;
    for (auto* candidate : m_window->findChildren<Tab*>()) {
        if (candidate->toolbar_container() && candidate->toolbar_container()->isAncestorOf(&toolbar))
            tab = candidate;
    }
    if (!edit || !tab)
        return;
    auto* bar = AddressBar::replace(*edit);
    if (!bar)
        return;
    toolbar.setProperty("arc_address_bar", true);
    QPointer<Tab> guarded_tab = tab;
    connect(bar, &AddressBar::edit_requested, this, [this, guarded_tab] {
        if (m_window && guarded_tab)
            CommandBar::toggle_for(*m_window, CommandBar::Mode::EditCurrentTab, guarded_tab);
    });
}

// Each tab's toolbar, looked up in that tab's toolbar container (searching the whole window was slow, and ran on every
// tab opened or closed). `new_tabs_only`: tabs this sidebar hasn't done yet.
void ArcSidebar::apply_toolbar_inset(bool new_tabs_only)
{
    if (!m_window)
        return;
    auto done_key = QString("arc_toolbar_inset_%1").arg(m_serial).toUtf8();
    for (auto* tab : tabs()) {
        if (new_tabs_only && tab->property(done_key.constData()).toBool())
            continue;
        auto* container = tab->toolbar_container();
        auto* toolbar = container ? container->findChild<QWidget*>("LadybirdNavigationToolbar") : nullptr;
        if (!toolbar)
            continue;
        if (!toolbar->property("arc_inset_watched").toBool()) {
            toolbar->setProperty("arc_inset_watched", true);
            toolbar->installEventFilter(this);
        }
        inset_toolbar(*toolbar);
        install_address_bar(*toolbar);
        tab->setProperty(done_key.constData(), true);
    }
}

// Places the drawn window controls (and the panel's sidebar toggle beside them). With match_native, they
// sit exactly where the real controls are when the panel's top-left is at panel_origin (window coords),
// so swapping one for the other is invisible. Otherwise they sit at the floating card's own inset.
void ArcSidebar::place_window_controls(QPoint panel_origin, bool match_native)
{
    m_panel->layout()->activate();
    auto strip_origin = m_top_strip->mapTo(m_panel, QPoint(0, 0));

    auto rects = m_window ? native_window_control_rects(*m_window) : QList<QRectF> {};
    QPointF first_center(FLOATING_LIGHTS_INSET + 7, TOP_STRIP_HEIGHT / 2.0 + 2);
    if (rects.size() == 3) {
        auto diameter = qMin(rects[0].width(), rects[0].height());
        m_traffic_lights->set_metrics(diameter, rects[1].center().x() - rects[0].center().x());
        if (match_native)
            first_center = rects[0].center() - QPointF(panel_origin) - QPointF(strip_origin);
        else
            first_center = QPointF(FLOATING_LIGHTS_INSET + diameter / 2.0, first_center.y());
    }

    auto diameter = m_traffic_lights->diameter();
    m_traffic_lights->move(qRound(first_center.x() - 1 - diameter / 2.0), qRound(first_center.y() - m_traffic_lights->height() / 2.0));
    m_sidebar_toggle->move(m_traffic_lights->geometry().right() + 12, qRound(first_center.y() - m_sidebar_toggle->height() / 2.0));
}

void ArcSidebar::set_panel_card(bool card)
{
    // Card: inner padding, rounded and shadowed (hover slide-out). Flat: identical to the docked sidebar.
    m_panel->layout()->setContentsMargins(card ? FLOATING_PADDING : 0, 0, card ? FLOATING_PADDING : 0, card ? FLOATING_PADDING : 0);
    if (m_host)
        m_host->set_card(card);
}

void ArcSidebar::dock_panel()
{
    m_tabs.set_vertical_sidebar_reserved_width({});
    m_host_docking = false;
    m_host_lifting_off = false;
    m_retract_timer->stop();
    m_reveal_timer->stop();
    if (m_host_animation)
        m_host_animation->stop();
    // Real controls back first, then drop the panel copy, in the same turn: no frame without controls.
    set_window_controls_hidden(*window(), false);
    if (m_host)
        m_host->hide();
    m_panel->setParent(this);
    m_panel->layout()->setContentsMargins(0, 0, 0, 0);
    m_panel->setGeometry(rect());
    m_panel->show();
    m_traffic_lights->set_drawn(false);
    m_sidebar_toggle->hide(); // Docked, the toolbar's own sidebar button does this.
    apply_toolbar_inset();
    update();
}

void ArcSidebar::ensure_host()
{
    if (m_host || !m_window)
        return;
    m_host = new FloatingSidebarHost(*m_window);
    keep_native_widget_above_page(*m_host);
    m_host->setProperty("arcSidebarRoot", true);
    m_host->installEventFilter(this);
    connect(m_host, &FloatingSidebarHost::left, this, [this] { m_retract_timer->start(); });

    m_host_animation = new QPropertyAnimation(m_host, "pos", this);
    connect(m_host_animation, &QAbstractAnimation::finished, this, [this] {
        if (m_host_retracting) {
            park_host();
            m_host_retracting = false;
            m_host_lifting_off = false;
            // Out of sight now: switch to the hover card for next time.
            if (m_mode == Mode::Hidden)
                prepare_card_panel();
        }
    });
}

void ArcSidebar::size_host()
{
    auto card_width = m_tabs.vertical_sidebar_expanded_width() - FLOATING_MARGIN;
    auto card_height = m_window->height() - FLOATING_MARGIN * 2;
    m_host->resize(card_width + FloatingSidebarHost::SHADOW * 2, card_height + FloatingSidebarHost::SHADOW * 2);
    m_panel->setGeometry(m_host->card_rect());
}

// Hidden mode at rest: the panel waits, out of sight, in the floating host as a hover card.
void ArcSidebar::prepare_card_panel()
{
    ensure_host();
    if (!m_host)
        return;
    m_panel->setParent(m_host);
    set_panel_card(true);
    size_host();
    m_panel->show();
    place_window_controls({}, false);
    m_traffic_lights->set_drawn(true);
    m_sidebar_toggle->show();
    // Parked just past the window's left edge rather than hidden: re-showing a native window takes AppKit a
    // frame or two, which would make the panel lag behind the page when it slides in.
    park_host();
    update();
}

void ArcSidebar::park_host()
{
    m_host->move(hidden_host_position());
    if (!m_host->isVisible())
        m_host->show();
}

// Puts the panel, flat (looking exactly like the docked sidebar), into the floating host with its
// top-left at panel_rect's, and the drawn window controls exactly over the real ones.
void ArcSidebar::show_flat_host(QRect panel_rect)
{
    ensure_host();
    m_reveal_timer->stop();
    m_retract_timer->stop();
    if (m_host_animation)
        m_host_animation->stop();
    m_panel->setParent(m_host);
    set_panel_card(false);
    m_host->resize(panel_rect.width() + FloatingSidebarHost::SHADOW * 2, panel_rect.height() + FloatingSidebarHost::SHADOW * 2);
    m_panel->setGeometry(m_host->card_rect());
    m_panel->show();
    place_window_controls(panel_rect.topLeft(), true);
    m_traffic_lights->set_drawn(true);
    m_sidebar_toggle->hide();
    m_host->move(panel_rect.topLeft() - QPoint(FloatingSidebarHost::SHADOW, FloatingSidebarHost::SHADOW));
}

// Called by upstream right before it docks or hides the sidebar (any route: toolbar, panel, menu). Both
// animations start right here, in the same turn, so the page and the sidebar move together from frame one.
void ArcSidebar::begin_transition(bool expanding)
{
    if (!m_mode_set || !m_window || !isVisible())
        return;
    if (m_transition->state() == QAbstractAnimation::Running) {
        m_transition->stop();
        finish_transition();
    }

    auto hidden_width = m_tabs.collapsed_vertical_tabs_width();
    auto docked_width = m_tabs.vertical_sidebar_expanded_width();

    if (!expanding) {
        // Measure where the docked sidebar is right now (upstream hasn't narrowed the column yet).
        if (m_panel->parentWidget() == this && width() > 80)
            m_last_docked_rect = QRect(mapTo(m_window, QPoint(0, 0)), size());
        if (m_last_docked_rect.isEmpty())
            return;
        // Hold the page where it is, and lift the sidebar into the host before upstream narrows its column,
        // drawn controls first, then hide the real ones: nothing visibly changes yet.
        m_tabs.set_vertical_sidebar_reserved_width(docked_width);
        show_flat_host(m_last_docked_rect);
        m_host->show();
        m_host->raise();
        // The drawn lights are on screen over the real ones now; hide the real ones a frame later, so there's
        // never a frame with neither.
        QTimer::singleShot(HANDOFF_MS, this, [this] {
            if (m_mode == Mode::Hidden || m_active_transition == Transition::Hide)
                set_window_controls_hidden(*window(), true);
        });
        m_pending_transition = Transition::Hide;
        start_transition();
    } else {
        // Hold the page at its hidden width; the animation widens it as the sidebar slides in.
        m_tabs.set_vertical_sidebar_reserved_width(hidden_width);
        m_pending_transition = Transition::Show;
        // Docked, the sidebar lands where it last was docked; first time, wait for upstream's layout to know.
        if (!m_last_docked_rect.isEmpty())
            start_transition();
    }
}

void ArcSidebar::start_transition()
{
    auto hidden_width = m_tabs.collapsed_vertical_tabs_width();
    auto docked_width = m_tabs.vertical_sidebar_expanded_width();

    if (m_pending_transition == Transition::Hide) {
        m_transition_from = m_host->pos();
        m_transition_to = QPoint(-m_host->width() - 4, m_transition_from.y());
        m_transition_width_from = docked_width;
        m_transition_width_to = hidden_width;
        m_host_lifting_off = true;
    } else {
        auto target = m_last_docked_rect;
        if (target.isEmpty() || width() > 80)
            target = QRect(mapTo(m_window, QPoint(0, 0)), size()); // After upstream's layout: this widget is the spot.
        m_last_docked_rect = target;
        auto was_visible = host_is_out() && !m_host_retracting;
        auto from = was_visible ? m_host->pos() : QPoint();
        show_flat_host(target);
        m_transition_to = m_host->pos();
        m_transition_from = was_visible ? from : QPoint(-m_host->width() - 4, m_transition_to.y());
        m_transition_width_from = hidden_width;
        m_transition_width_to = docked_width;
        m_host_docking = true;
    }

    m_active_transition = m_pending_transition;
    m_pending_transition = Transition::None;
    step_transition(0);
    m_host->show();
    m_host->raise();
    // The panel just changed size (card -> docked); lay it out and paint it now, so its first frames on
    // screen aren't blank while Qt gets around to it.
    m_panel->layout()->activate();
    m_host->repaint();

    if (m_active_transition == Transition::Hide) {
        m_transition->start();
        return;
    }
    // Showing resized the floating panel, and AppKit takes a couple of frames to put a freshly resized native
    // window on screen. Start once it has, page and panel together (the page is held where it is meanwhile).
    QTimer::singleShot(SHOW_SETTLE_MS, this, [this] {
        if (m_active_transition == Transition::Show && m_transition->state() != QAbstractAnimation::Running)
            m_transition->start();
    });
}

void ArcSidebar::step_transition(qreal progress)
{
    if (m_active_transition == Transition::None || !m_host)
        return;
    auto lerp = [progress](qreal from, qreal to) { return from + (to - from) * progress; };
    // One clock for both, so the page's edge travels with the sidebar's.
    m_tabs.set_vertical_sidebar_reserved_width(qRound(lerp(m_transition_width_from, m_transition_width_to)));
    // The page's own layout would only follow on the next event-loop pass, a frame behind the panel; apply
    // it now so both land in the same frame.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    m_host->move(qRound(lerp(m_transition_from.x(), m_transition_to.x())), qRound(lerp(m_transition_from.y(), m_transition_to.y())));
    set_transition_opacity(fade_opacity(progress));
}

// The sidebar fades a little faster than it slides: fully there before it lands, gone before it leaves.
qreal ArcSidebar::fade_opacity(qreal progress) const
{
    auto faded = qBound(0.0, progress * FADE_SPEEDUP, 1.0);
    return m_active_transition == Transition::Show ? faded : 1.0 - qBound(0.0, progress * FADE_SPEEDUP - (FADE_SPEEDUP - 1.0), 1.0);
}

void ArcSidebar::set_transition_opacity(qreal opacity)
{
    // The whole floating window's layer: composited on the GPU, nothing is re-rendered per frame.
    if (m_host)
        set_native_widget_opacity(*m_host, opacity);
}

void ArcSidebar::finish_transition()
{
    auto finished = m_active_transition;
    m_active_transition = Transition::None;
    set_transition_opacity(1.0); // Back to fully opaque for the hover slide-out and the docked sidebar.
    m_tabs.set_vertical_sidebar_reserved_width({});
    if (finished == Transition::Hide) {
        m_host_lifting_off = false;
        if (m_mode == Mode::Hidden)
            prepare_card_panel();
    } else if (finished == Transition::Show) {
        // Real lights back first, under the drawn ones; drop the panel copy a frame later.
        if (m_mode == Mode::Docked) {
            set_window_controls_hidden(*window(), false);
            QTimer::singleShot(HANDOFF_MS, this, [this] {
                m_host_docking = false;
                if (m_mode == Mode::Docked)
                    dock_panel();
            });
        } else {
            m_host_docking = false;
        }
    }
}

QPoint ArcSidebar::rest_host_position() const
{
    return QPoint(FLOATING_MARGIN - FloatingSidebarHost::SHADOW, FLOATING_MARGIN - FloatingSidebarHost::SHADOW);
}

QPoint ArcSidebar::hidden_host_position() const
{
    return QPoint(-(m_host ? m_host->width() : 300) - 4, FLOATING_MARGIN - FloatingSidebarHost::SHADOW);
}

void ArcSidebar::slide_host(QPoint from, QPoint to, bool revealing)
{
    m_host_animation->stop();
    m_host_retracting = !revealing;
    m_host_animation->setDuration(revealing ? REVEAL_ANIMATION_MS : RETRACT_ANIMATION_MS);
    m_host_animation->setEasingCurve(QEasingCurve::InOutCubic);
    m_host_animation->setStartValue(from);
    m_host_animation->setEndValue(to);
    m_host->move(from);
    m_host->show();
    m_host->raise();
    m_host_animation->start();
}

void ArcSidebar::reveal()
{
    if (m_mode != Mode::Hidden || !m_host || m_host_lifting_off || m_active_transition != Transition::None)
        return;
    m_retract_timer->stop();
    if (host_is_out() && !m_host_retracting)
        return;
    size_host();
    auto from = host_is_out() ? m_host->pos() : hidden_host_position();
    slide_host(from, rest_host_position(), true);
}

bool ArcSidebar::host_is_out() const
{
    // Parked hosts sit entirely left of the window.
    return m_host && m_host->isVisible() && m_host->geometry().right() > 0;
}

bool ArcSidebar::pointer_is_over_floating_sidebar() const
{
    if (!host_is_out())
        return false;
    auto in_window = m_window->mapFromGlobal(QCursor::pos());
    return m_host->geometry().adjusted(FloatingSidebarHost::SHADOW, 0, -FloatingSidebarHost::SHADOW, 0).contains(in_window)
        || (m_watched_column && m_watched_column->rect().contains(m_watched_column->mapFromGlobal(QCursor::pos())));
}

void ArcSidebar::retract_if_left()
{
    if (m_mode != Mode::Hidden || !host_is_out() || m_host_retracting || m_host_docking
        || m_active_transition != Transition::None || m_pending_transition != Transition::None)
        return;
    // Stay out while a menu or dialog from the panel is open; check again shortly.
    if (m_host->property("arcKeepOpen").toBool() || QApplication::activePopupWidget() || QApplication::activeModalWidget()) {
        m_retract_timer->start();
        return;
    }
    if (pointer_is_over_floating_sidebar())
        return;
    slide_host(m_host->pos(), hidden_host_position(), false);
}

void ArcSidebar::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // Only lay the panel out while it lives here; mid-animation it belongs to the floating host.
    if (m_panel->parentWidget() == this && (m_mode != Mode::Hidden || !m_mode_set)) {
        m_panel->setGeometry(rect());
        // Remembered so hiding can lift the sidebar off from exactly where it was.
        if (width() > 80 && m_window)
            m_last_docked_rect = QRect(mapTo(m_window, QPoint(0, 0)), size());
    }
    if (m_shelf && m_shelf->isVisible())
        position_shelf();
}

void ArcSidebar::paintEvent(QPaintEvent*)
{
    // Hidden or mid-animation the panel is in the floating host; painting here would cover the toolbar.
    if (m_panel->parentWidget() != this)
        return;
    // Docked, the window's own (tinted) background shows through, so the two can never differ by a shade; see
    // apply_space_tint(). Painting the same color here came out one step off on macOS.
}

// Arc paints the space color across the whole window frame. Upstream derives every chrome surface from
// chrome_background(), so tint that and ask the app to restyle.
double ArcSidebar::space_tint_amount() const
{
    return ChromeStyle::is_dark(palette()) ? 0.26 : 0.38;
}

void ArcSidebar::apply_space_tint()
{
    // A slide between spaces keeps the old tint until it's over, then fades to the new one.
    if (m_holding_tint || (m_tint_fade && m_tint_fade->state() == QAbstractAnimation::Running))
        return;
    apply_tint(space_color(), space_tint_amount());
}

// The tint partway from one color to another: a space without a color has none to fade from or to.
static std::pair<QColor, double> blend_tints(QColor from, double from_amount, QColor to, double to_amount, qreal progress)
{
    if (!from.isValid()) {
        from = to;
        from_amount = 0;
    }
    if (!to.isValid()) {
        to = from;
        to_amount = 0;
    }
    if (!from.isValid())
        return { QColor(), 0 };
    auto mix = [progress](qreal a, qreal b) { return a + (b - a) * progress; };
    return { QColor::fromRgbF(mix(from.redF(), to.redF()), mix(from.greenF(), to.greenF()), mix(from.blueF(), to.blueF())), mix(from_amount, to_amount) };
}

// After a slide between spaces, the whole window fades from the old space's color to the new one's. (Not during:
// each step restyles the window, and would make the slide stutter.)
void ArcSidebar::fade_to_space_tint()
{
    static constexpr int TINT_FADE_MS = 240;
    auto to_color = space_color();
    auto to_amount = space_tint_amount();
    auto from_color = s_applied_tint;
    auto from_amount = s_applied_tint_amount;
    if (!m_tint_fade) {
        m_tint_fade = new QVariantAnimation(this);
        m_tint_fade->setDuration(TINT_FADE_MS);
        m_tint_fade->setStartValue(0.0);
        m_tint_fade->setEndValue(1.0);
        m_tint_fade->setEasingCurve(QEasingCurve::InOutQuad);
    }
    m_tint_fade->stop();
    m_tint_fade->disconnect(this);
    connect(m_tint_fade, &QVariantAnimation::valueChanged, this, [this, from_color, from_amount, to_color, to_amount](QVariant const& value) {
        auto [color, amount] = blend_tints(from_color, from_amount, to_color, to_amount, value.toReal());
        apply_tint(color, amount, true);
    });
    connect(m_tint_fade, &QAbstractAnimation::finished, this, [this] {
        apply_tint(space_color(), space_tint_amount());
    });
    m_tint_fade->start();
}

// `visible_only` restyles just what's on screen (the window, its tab bar and current tab), for the steps of a fade:
// restyling every tab takes too long for that. A full apply ends the fade.
// Tabs not shown when the tint changed restyle as they're shown (see restyle_if_tint_changed()): restyling every tab of
// every window took long enough to hitch the end of each switch between spaces.
static int s_tint_generation = 0;
static constexpr char const* TINT_GENERATION_PROPERTY = "arc_tint_generation";

static void notify_palette_change(QWidget* widget)
{
    QEvent palette_change(QEvent::PaletteChange);
    QApplication::sendEvent(widget, &palette_change);
    widget->update();
}

static void restyle_tab(Tab& tab)
{
    notify_palette_change(&tab);
    for (auto* edit : tab.findChildren<LocationEdit*>())
        notify_palette_change(edit);
    tab.setProperty(TINT_GENERATION_PROPERTY, s_tint_generation);
}

void ArcSidebar::restyle_if_tint_changed(Tab& tab)
{
    if (tab.property(TINT_GENERATION_PROPERTY).toInt() != s_tint_generation)
        restyle_tab(tab);
}

void ArcSidebar::apply_tint(QColor const& color, double amount, bool visible_only)
{
    if (s_tint_initialized && color == s_applied_tint && amount == s_applied_tint_amount)
        return;
    s_tint_initialized = true;
    s_applied_tint = color;
    s_applied_tint_amount = amount;
    (void)visible_only; // Each step of a fade and its end restyle the same: what's shown.

    ChromeStyle::set_chrome_background_tint(color, amount);
    ++s_tint_generation;
    // Upstream's chrome widgets restyle themselves on PaletteChange (the path a system theme change takes).
    // Notify only those; Qt's own widgets would reset their style sheets on a synthetic event. Only what's shown: each
    // window, its tab strip and its current tab; other tabs restyle as they're shown.
    for (auto* top_level : QApplication::topLevelWidgets()) {
        auto* browser_window = qobject_cast<BrowserWindow*>(top_level);
        if (!browser_window)
            continue;
        notify_palette_change(browser_window);
        for (auto* tabs : browser_window->findChildren<TabWidget*>())
            notify_palette_change(tabs);
        if (auto* tab = browser_window->current_tab())
            restyle_tab(*tab);
        browser_window->update();
    }

    // Arc has no line between the sidebar and the page; upstream's column style draws one.
    static auto const column_style = QStringLiteral("QWidget#LadybirdVerticalTabBar { border: none; background: transparent; }");
    if (auto* column = parentWidget(); column && column->styleSheet() != column_style)
        column->setStyleSheet(column_style);
}

// --- Interaction --------------------------------------------------------------------------------

void ArcSidebar::on_row_clicked(QModelIndex const& index)
{
    switch (row_kind(index)) {
    case RowKind::PinnedTab:
        open_or_activate_node(index.data(IdRole).toString());
        break;
    case RowKind::Folder:
        m_tree->toggle_folder(index);
        break;
    case RowKind::NewTab:
        if (m_window)
            CommandBar::toggle_for(*m_window);
        break;
    case RowKind::TodayTab:
        if (auto* tab = tab_in_row(index); tab && m_tabs.index_of(tab) >= 0)
            activate(*tab);
        break;
    case RowKind::BottomSpace:
        break;
    }
}

void ArcSidebar::on_close_clicked(QModelIndex const& index)
{
    if (auto* tab = tab_in_row(index); tab && m_tabs.index_of(tab) >= 0)
        close_tab(*tab);
}

ArcSidebar::DropTarget ArcSidebar::resolve_drop(QModelIndex const& target, SidebarTree::DropPlace place) const
{
    if (place == SidebarTree::DropPlace::End || !target.isValid())
        return { false, {}, -1 };

    switch (row_kind(target)) {
    case RowKind::BottomSpace:
        return { false, {}, -1 }; // The end of Today.
    case RowKind::NewTab:
        if (place == SidebarTree::DropPlace::Above)
            return { true, {}, -1 };
        return { false, {}, 0 };
    case RowKind::TodayTab: {
        auto today = today_tabs(m_space_id);
        auto i = static_cast<int>(today.indexOf(tab_in_row(target)));
        return { false, {}, place == SidebarTree::DropPlace::Above ? i : i + 1 };
    }
    case RowKind::Folder:
        if (place == SidebarTree::DropPlace::Into)
            return { true, target.data(IdRole).toString(), -1 };
        [[fallthrough]];
    case RowKind::PinnedTab: {
        auto location = SpaceStore::the().location_of(target.data(IdRole).toString());
        return { true, location.parent_id, place == SidebarTree::DropPlace::Above ? location.index : location.index + 1 };
    }
    }
    return { false, {}, -1 };
}

// Reorders a Today tab by moving it within upstream's tab order.
static void move_today_tab(BrowserWindow& window, TabWidget& tabs, Tab& tab, QList<Tab*> today, int today_index)
{
    auto from = today.indexOf(&tab);
    today.removeAll(&tab);
    if (today.isEmpty())
        return;
    if (today_index < 0)
        today_index = today.size();
    if (from >= 0 && from < today_index)
        --today_index;
    today_index = qBound(0, today_index, static_cast<int>(today.size()));

    auto old_index = tabs.index_of(&tab);
    int new_index = 0;
    if (today_index < today.size()) {
        auto before = tabs.index_of(today[today_index]);
        new_index = old_index < before ? before - 1 : before;
    } else {
        auto last = tabs.index_of(today.last());
        new_index = old_index < last ? last : last + 1;
    }
    if (old_index >= 0 && new_index != old_index)
        window.move_tab(old_index, new_index);
}

void ArcSidebar::on_row_dropped(QModelIndex const& source, QModelIndex const& target, SidebarTree::DropPlace place)
{
    auto destination = resolve_drop(target, place);
    auto kind = row_kind(source);
    auto& store = SpaceStore::the();

    if (kind == RowKind::PinnedTab || kind == RowKind::Folder) {
        auto id = source.data(IdRole).toString();
        if (destination.pinned) {
            store.move_node(id, m_space_id, destination.parent_id, destination.index);
            return;
        }
        if (kind == RowKind::Folder)
            return; // Folders only live in the pinned section.

        // Dragged below the divider: unpin into Today.
        auto* tab = tab_for_node(id);
        if (!tab) {
            auto const* node = store.node(id);
            if (!node)
                return;
            tab = open_tab(node->url);
            if (!tab)
                return;
        }
        unpin_node(id);
        move_today_tab(*m_window, m_tabs, *tab, today_tabs(m_space_id), destination.index);
        schedule_rebuild();
        return;
    }

    if (kind == RowKind::TodayTab) {
        auto* tab = tab_in_row(source);
        if (!tab || m_tabs.index_of(tab) < 0)
            return;
        if (destination.pinned) {
            pin_tab(*tab, destination.parent_id, destination.index);
            return;
        }
        move_today_tab(*m_window, m_tabs, *tab, today_tabs(m_space_id), destination.index);
        schedule_rebuild();
        save_session();
    }
}

void ArcSidebar::on_urls_dropped(QList<QUrl> const& urls, QModelIndex const& target, SidebarTree::DropPlace place)
{
    auto destination = resolve_drop(target, place);
    for (auto const& url : urls) {
        if (destination.pinned) {
            SpaceStore::the().add_pinned_tab(m_space_id, destination.parent_id, destination.index, url.toString(), url.host());
            if (destination.index >= 0)
                ++destination.index;
        } else {
            open_tab(url.toString());
        }
    }
}

bool ArcSidebar::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel && m_tree && (watched == m_tree->viewport() || watched == m_panel))
        return handle_wheel(*static_cast<QWheelEvent*>(event));
    if (m_tree && watched == m_tree->verticalScrollBar()) {
        if (event->type() == QEvent::Enter)
            show_scrollbar_briefly();
        else if (event->type() == QEvent::Leave && m_scrollbar_linger)
            m_scrollbar_linger->start();
        return false;
    }
    if (watched == m_rename_editor && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        finish_inline_rename(false);
        return true;
    }
    if ((event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show) && watched->property("arc_inset_watched").toBool()) {
        inset_toolbar(*static_cast<QWidget*>(watched));
        if (event->type() == QEvent::Show)
            install_address_bar(*static_cast<QWidget*>(watched));
        return false;
    }
    if (watched == m_watched_column && m_mode == Mode::Hidden) {
        if (event->type() == QEvent::Enter)
            m_reveal_timer->start();
        else if (event->type() == QEvent::Leave)
            m_reveal_timer->stop();
        return false;
    }
    if (watched == m_host) {
        if (event->type() == QEvent::Enter)
            m_retract_timer->stop();
        return false;
    }
    if (watched == m_window) {
        if (event->type() == QEvent::Paint) {
            // The window's background, under everything: the (tinted) chrome color, painted by Qt rather than left to
            // the native window, so no part of the chrome shows an untinted color.
            QPainter painter(m_window);
            painter.fillRect(static_cast<QPaintEvent*>(event)->rect(), ChromeStyle::chrome_background(m_window->palette()));
            return false;
        }
        if (event->type() == QEvent::Resize && m_mode == Mode::Hidden && m_host && !m_host_lifting_off && !m_host_docking)
            size_host();
        else if (event->type() == QEvent::WindowActivate && m_mode_set)
            set_window_controls_hidden(*window(), m_mode == Mode::Hidden || m_host_docking); // AppKit can restore them on activation.
        if (event->type() == QEvent::WindowActivate) {
            apply_profile_for_new_views();
            apply_space_tint(); // The window frame shows this window's space's color, not the last-rebuilt one's.
        }
        return false;
    }

    if (watched == m_downloads_button || watched == m_shelf) {
        if (event->type() == QEvent::Enter) {
            m_shelf_hide_timer->stop();
            if (watched == m_downloads_button && (!m_shelf->isVisible() || m_shelf_hiding))
                m_shelf_show_timer->start();
        } else if (event->type() == QEvent::Leave) {
            m_shelf_show_timer->stop();
            if (m_shelf->isVisible())
                m_shelf_hide_timer->start();
        }
        return false;
    }

    if (watched == m_header && (event->type() == QEvent::Enter || event->type() == QEvent::Leave)) {
        m_header_hovered = event->type() == QEvent::Enter;
        m_header->setProperty("hovered", m_header_hovered);
        m_header->style()->unpolish(m_header);
        m_header->style()->polish(m_header);
        m_space_menu_button->setVisible(m_header_hovered);
        update_space_header_icon();
        return false;
    }

    if (watched == m_top_strip) {
        if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            if (auto* handle = m_window ? m_window->windowHandle() : nullptr)
                handle->startSystemMove();
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            if (m_window)
                m_window->isMaximized() ? m_window->showNormal() : m_window->showMaximized();
            return true;
        }
        return false;
    }

    if (watched != m_favorites)
        return QWidget::eventFilter(watched, event);

    // Drop a tab (or a link) on the favorites grid to make it a favorite.
    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto* drag = static_cast<QDragMoveEvent*>(event);
        if (drag->source() == m_tree || drag->mimeData()->hasUrls()) {
            drag->setDropAction(Qt::CopyAction);
            drag->accept();
            return true;
        }
        return false;
    }
    case QEvent::Drop: {
        auto* drop = static_cast<QDropEvent*>(event);
        if (drop->source() == m_tree) {
            auto index = m_tree->currentIndex();
            auto kind = row_kind(index);
            if (kind == RowKind::TodayTab) {
                // The row can outlive its tab while a drag runs (rebuilds wait for the drop).
                if (auto* tab = tab_in_row(index); tab && m_tabs.index_of(tab) >= 0)
                    add_favorite(*tab);
            } else if (kind == RowKind::PinnedTab) {
                // Arc moves a pinned tab into favorites.
                auto id = index.data(IdRole).toString();
                if (auto const* node = SpaceStore::the().node(id)) {
                    auto title = node->title.isEmpty() ? node->saved_title : node->title;
                    auto favorite_id = SpaceStore::the().add_favorite(node->url, title, -1, current_profile());
                    if (auto* tab = tab_for_node(id)) {
                        set_tab_node(*tab, favorite_node(favorite_id));
                        set_tab_space(*tab, {});
                    }
                    SpaceStore::the().remove_node(id);
                    save_session();
                }
            }
        } else if (drop->mimeData()->hasUrls()) {
            for (auto const& url : drop->mimeData()->urls())
                SpaceStore::the().add_favorite(url.toString(), url.host(), -1, current_profile());
        }
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        m_tree->reset_drag_state();
        return true;
    }
    default:
        return false;
    }
}

// --- Menus --------------------------------------------------------------------------------------

static void add_move_to_space_menu(QMenu& menu, QString const& current_space, std::function<void(QString const&)> move)
{
    auto const& spaces = SpaceStore::the().spaces();
    if (spaces.size() <= 1)
        return;
    auto* submenu = menu.addMenu("Move to Space");
    for (auto const& space : spaces) {
        if (space.id == current_space)
            continue;
        auto id = space.id;
        submenu->addAction(space.icon.isEmpty() ? space.title : space.icon + "  " + space.title, [move, id] { move(id); });
    }
}

void ArcSidebar::show_tree_menu(QPoint const& position)
{
    auto index = m_tree->indexAt(position);
    QMenu menu(this);
    QPersistentModelIndex persistent(index);

    auto kind = index.isValid() ? row_kind(index) : RowKind::NewTab;
    auto node_id = index.data(IdRole).toString();
    QPointer<Tab> tab = index.isValid() ? tab_in_row(index) : nullptr;
    if (tab && m_tabs.index_of(tab) < 0)
        tab = nullptr;

    auto move = [this, persistent](QString const& space_id) {
        if (persistent.isValid())
            move_to_space(persistent, space_id);
    };

    switch (kind) {
    case RowKind::BottomSpace:
    case RowKind::NewTab:
        menu.addAction("New Tab", [this] {
            if (m_window)
                CommandBar::toggle_for(*m_window);
        });
        menu.addAction("New Folder", [this] { new_folder({}, -1); });
        break;

    case RowKind::TodayTab:
        if (!tab)
            return;
        menu.addAction("Pin Tab", [this, tab] {
            if (tab)
                pin_tab(*tab, {}, -1);
        });
        menu.addAction("Add to Favorites", [this, tab] {
            if (tab)
                add_favorite(*tab);
        });
        add_move_to_space_menu(menu, m_space_id, move);
        menu.addSeparator();
        menu.addAction("Copy Link", [this, tab] {
            if (tab && m_window) {
                QGuiApplication::clipboard()->setText(url_of(*tab));
                Toast::show_in(*m_window, "Copied URL", "link");
            }
        });
        menu.addAction("Duplicate Tab", [this, tab] {
            if (tab)
                duplicate_tab(*tab);
        });
        menu.addSeparator();
        menu.addAction("Close Tab", [this, tab] {
            if (tab)
                close_tab(*tab);
        });
        if (auto today = today_tabs(m_space_id); !today.isEmpty() && today.last() != tab) {
            menu.addAction("Close Tabs Below", [this, tab] {
                if (tab)
                    close_tabs_below(*tab);
            });
        }
        break;

    case RowKind::PinnedTab: {
        menu.addAction("Rename…", [this, node_id] { begin_inline_rename(node_id); });
        if (tab && persistent.data(NavigatedAwayRole).toBool()) {
            menu.addAction("Back to Pinned URL", [this, node_id] { back_to_pinned_url(node_id); });
            menu.addAction("Separate From Pinned Tab", [this, node_id] { separate_from_pinned_tab(node_id); });
            menu.addAction("Set Pinned URL to Current Page", [this, tab, node_id] {
                if (!tab)
                    return;
                SpaceStore::the().set_node_url(node_id, url_of(*tab));
                tab->setProperty(PINNED_HOME_PROPERTY, QString());
                update_tab_rows(*tab);
            });
        }
        menu.addAction("Add to Favorites", [this, node_id] {
            if (auto const* node = SpaceStore::the().node(node_id))
                SpaceStore::the().add_favorite(node->url, node->title.isEmpty() ? node->saved_title : node->title, -1, current_profile());
        });
        add_move_to_space_menu(menu, m_space_id, move);
        menu.addSeparator();
        menu.addAction("Copy Link", [this, tab, node_id] {
            auto const* node = SpaceStore::the().node(node_id);
            auto url = tab ? url_of(*tab) : node ? node->url : QString();
            if (url.isEmpty() || !m_window)
                return;
            QGuiApplication::clipboard()->setText(url);
            Toast::show_in(*m_window, "Copied URL", "link");
        });
        if (tab) {
            menu.addAction("Duplicate Tab", [this, tab] {
                if (tab)
                    duplicate_tab(*tab);
            });
        }
        menu.addSeparator();
        if (tab) {
            menu.addAction("Close Tab", [this, tab] {
                if (tab)
                    close_tab(*tab);
            });
        }
        menu.addAction(tab ? "Unpin (Move to Today)" : "Remove from Sidebar", [this, node_id] { unpin_node(node_id); });
        break;
    }

    case RowKind::Folder:
        menu.addAction("New Folder Inside", [this, node_id] { new_folder(node_id, -1); });
        menu.addAction("Rename…", [this, node_id] { begin_inline_rename(node_id); });
        add_move_to_space_menu(menu, m_space_id, move);
        menu.addSeparator();
        menu.addAction("Delete Folder…", [this, node_id] { delete_folder(node_id); });
        break;
    }

    KeepSidebarOpen keep_open(this);
    menu.exec(m_tree->viewport()->mapToGlobal(position));
}

void ArcSidebar::show_space_menu(QString const& space_id, QPoint const& global_position)
{
    auto& store = SpaceStore::the();
    auto const* space = store.space(space_id);
    if (!space)
        return;

    QMenu menu(this);
    menu.addAction("Change Space Icon\u2026", [this, space_id] { edit_space(space_id); });
    menu.addAction("Rename Space\u2026", [this, space_id] {
        auto const* space = SpaceStore::the().space(space_id);
        if (!space)
            return;
        bool ok = false;
        auto title = QInputDialog::getText(m_window, "Rename Space", "Space name:", QLineEdit::Normal, space->title, &ok).trimmed();
        if (ok && !title.isEmpty())
            SpaceStore::the().update_space(space_id, title, space->icon, space->color);
    });

    auto* colors = menu.addMenu("Edit Theme Color");
    for (auto const& preset : COLOR_PRESETS) {
        QPixmap swatch(14, 14);
        swatch.fill(QColor(preset.hex));
        auto hex = QString(preset.hex);
        auto* action = colors->addAction(QIcon(swatch), preset.name, [space_id, hex] {
            auto const* space = SpaceStore::the().space(space_id);
            if (space)
                SpaceStore::the().update_space(space_id, space->title, space->icon, QColor(hex));
        });
        action->setCheckable(true);
        action->setChecked(space->color == QColor(preset.hex));
    }

    menu.addSeparator();
    menu.addAction("New Folder", [this] { new_folder({}, -1); });

    menu.addSeparator();
    int index = 0;
    for (int i = 0; i < store.spaces().size(); ++i) {
        if (store.spaces()[i].id == space_id)
            index = i;
    }
    if (index > 0)
        menu.addAction("Move Space Left", [space_id, index] { SpaceStore::the().move_space(space_id, index - 1); });
    if (index + 1 < store.spaces().size())
        menu.addAction("Move Space Right", [space_id, index] { SpaceStore::the().move_space(space_id, index + 1); });
    menu.addAction("New Space\u2026", [this] { new_space(); });

    menu.addSeparator();
    auto* remove = menu.addAction("Delete Space", [this, space_id] { delete_space(space_id); });
    remove->setEnabled(store.spaces().size() > 1);

    KeepSidebarOpen keep_open(this);
    menu.exec(global_position);
}

void ArcSidebar::show_favorite_menu(QString const& favorite_id, QPoint const& global_position)
{
    auto favorites = SpaceStore::the().favorites_for_profile(current_profile());
    int index = 0;
    for (int i = 0; i < favorites.size(); ++i) {
        if (favorites[i].id == favorite_id)
            index = i;
    }

    QMenu menu(this);
    if (index > 0)
        menu.addAction("Move Left", [favorite_id] { SpaceStore::the().move_favorite_within_profile(favorite_id, -1); });
    if (index + 1 < favorites.size())
        menu.addAction("Move Right", [favorite_id] { SpaceStore::the().move_favorite_within_profile(favorite_id, +1); });
    menu.addSeparator();
    menu.addAction("Remove from Favorites", [this, favorite_id] {
        // Its open tab, if any, becomes a Today tab in the current space.
        if (auto* tab = tab_for_node(favorite_node(favorite_id))) {
            set_tab_node(*tab, {});
            set_tab_space(*tab, m_space_id);
        }
        SpaceStore::the().remove_favorite(favorite_id);
        save_session();
    });
    KeepSidebarOpen keep_open(this);
    menu.exec(global_position);
}

}
