/*
 * Arc-style sidebar: favorites, the current space's pinned folders and tabs, Today's tabs,
 * and a space switcher.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <UI/Qt/Arc/ArcIcons.h>
#include <UI/Qt/Arc/ArcSidebar.h>
#include <UI/Qt/Arc/ProfileSessions.h>
#include <LibWebView/BrowsingSession.h>
#include <UI/Qt/Arc/CommandBar.h>
#include <UI/Qt/Arc/FloatingPanel.h>
#include <UI/Qt/Arc/SidebarWidgets.h>
#include <UI/Qt/Arc/SymbolIcon.h>
#include <UI/Qt/Arc/WindowControls.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/WebContentView.h>

#include <QButtonGroup>
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
#include <QGraphicsOpacityEffect>
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
static constexpr int SPARE_TAB_DELAY_MS = 1200;
static constexpr char const* NODE_PROPERTY = "arc_node";
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
    auto* tabs = window.findChild<TabWidget*>();
    if (!tabs)
        return;
    auto* sidebar = new ArcSidebar(window, *tabs);
    tabs->set_vertical_sidebar_widget(sidebar);
}

ArcSidebar* ArcSidebar::for_window(BrowserWindow& window)
{
    return window.findChild<ArcSidebar*>();
}

ArcSidebar::ArcSidebar(BrowserWindow& window, TabWidget& tabs)
    : QWidget(&tabs)
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
    connect(m_sidebar_toggle, &QAbstractButton::clicked, this, [] {
        auto& settings = WebView::Application::settings();
        auto tab_settings = settings.tab_settings();
        tab_settings.vertical_tabs_expanded = !tab_settings.vertical_tabs_expanded;
        settings.set_tab_settings(tab_settings);
    });
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
    m_shelf_opacity = new QGraphicsOpacityEffect(m_shelf);
    m_shelf_opacity->setOpacity(0);
    m_shelf->setGraphicsEffect(m_shelf_opacity);
    m_shelf_position_animation = new QPropertyAnimation(m_shelf, "pos", this);
    m_shelf_position_animation->setDuration(SHELF_ANIMATION_MS);
    m_shelf_position_animation->setEasingCurve(QEasingCurve::InOutCubic);
    m_shelf_opacity_animation = new QPropertyAnimation(m_shelf_opacity, "opacity", this);
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
        QTimer::singleShot(0, this, [this] { apply_toolbar_inset(); });
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

    rebuild();
}

bool ArcSidebar::handle_key_press(QKeyEvent const& event)
{
    // Control+1..9 (Alt elsewhere) switches spaces, as in Arc. Called from an app-wide event filter so
    // it works even while the page has focus (the web view gets first claim on most shortcuts).
    auto modifiers = event.modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    auto expected = SPACE_SHORTCUT_MODIFIER == Qt::META ? Qt::MetaModifier : Qt::AltModifier;
    if (modifiers != expected || event.key() < Qt::Key_1 || event.key() > Qt::Key_9)
        return false;
    switch_to_space_at(event.key() - Qt::Key_1);
    return true;
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
    for (auto* tab : tabs()) {
        if (node_of(*tab) == node_id)
            return tab;
    }
    return nullptr;
}

QString ArcSidebar::space_of(Tab const& tab) { return tab.property(SPACE_PROPERTY).toString(); }
QString ArcSidebar::node_of(Tab const& tab) { return tab.property(NODE_PROPERTY).toString(); }
void ArcSidebar::set_tab_space(Tab& tab, QString const& space) { tab.setProperty(SPACE_PROPERTY, space); }
void ArcSidebar::set_tab_node(Tab& tab, QString const& node) { tab.setProperty(NODE_PROPERTY, node); }

QString ArcSidebar::url_of(Tab const& tab)
{
    auto const& url = tab.view().url();
    return qstring_from_ak_string(url.serialize());
}

QString ArcSidebar::title_of(Tab const& tab)
{
    auto title = tab.title();
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
    auto key = QString("arc_watched_%1").arg(reinterpret_cast<quintptr>(this));
    if (tab.property(key.toUtf8().constData()).toBool())
        return;
    tab.setProperty(key.toUtf8().constData(), true);

    QPointer<Tab> guarded = &tab;
    auto refresh = [this, guarded] {
        if (guarded)
            update_tab_rows(*guarded);
    };
    connect(&tab, &Tab::title_changed, this, refresh);
    connect(&tab, &Tab::favicon_changed, this, refresh);
    connect(&tab, &Tab::loading_changed, this, refresh); // Navigations: a pinned tab may have left its URL.
    connect(&tab, &Tab::loading_finished, this, [this, refresh] {
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
    timer->disconnect();
    QPointer<ArcSidebar> guarded = this;
    QObject::connect(timer, &QTimer::timeout, [guarded] {
        if (!guarded)
            return;
        QList<SessionEntry> entries;
        for (auto* tab : guarded->tabs())
            entries.append({ url_of(*tab), space_of(*tab), node_of(*tab) });
        SpaceStore::the().set_session(std::move(entries));
    });
    timer->start();
}

// --- Actions ------------------------------------------------------------------------------------

Tab& ArcSidebar::open_tab(QString const& url)
{
    // A spare tab is ready for this: navigating it is much quicker than building a new tab's widgets.
    if (auto* spare = take_spare_tab(current_profile())) {
        spare->setProperty(BLANK_PROPERTY, false);
        set_tab_space(*spare, m_space_id);
        spare->navigate(ak_url_from_qurl(QUrl(url)));
        activate(*spare);
        return *spare;
    }
    ScopedProfileForNewViews profile(current_profile());
    auto& tab = m_window->new_tab_from_url(ak_url_from_qurl(QUrl(url)), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
    set_tab_space(tab, m_space_id);
    return tab;
}

// A blank tab built ahead of time, while the app is idle, in the current space's profile. Not listed anywhere; used
// by the next tab the sidebar opens, or as the empty page when a space's last tab closes.
void ArcSidebar::prepare_spare_tab()
{
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
    tab->navigate(ak_url_from_qurl(QUrl(node->url)));
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
    tab->navigate(ak_url_from_qurl(QUrl(node->url)));
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
        ScopedProfileForNewViews profile(profile_of_space(entry.space_id));
        auto& tab = m_window->new_tab_from_url(ak_url_from_qurl(QUrl(entry.url)), Web::HTML::ActivateTab::No, BrowserWindow::TabLocation::end());
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
bool ArcSidebar::is_blank(Tab const& tab)
{
    if (tab.property(BLANK_PROPERTY).toBool())
        return true;
    auto url = url_of(tab);
    return url == "about:newtab" || url == "about:blank" || url == qstring_from_ak_string(WebView::Application::settings().new_tab_page_url().serialize());
}

// Every close (sidebar, Cmd+W, Clear) comes through here. Like Arc: the next tab comes from the same space,
// and closing a space's last tab leaves an empty page rather than another space's tab or a closed window.
void ArcSidebar::before_tab_closes(Tab& tab)
{
    if (m_window->current_tab() != &tab)
        return;
    auto space = space_of(tab);
    Tab* next = nullptr;
    auto same_space = today_tabs(space);
    if (auto i = same_space.indexOf(&tab); i >= 0) {
        if (i + 1 < same_space.size())
            next = same_space[i + 1];
        else if (i > 0)
            next = same_space[i - 1];
    }
    if (!next) {
        for (auto* other : tabs()) {
            if (other != &tab && space_of(*other) == space && !is_blank(*other)) {
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
        if (tab != current && tab != m_spare_tab && is_blank(*tab)) {
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
    auto& tab = open_tab(node->url);
    set_tab_node(tab, node_id);
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
    auto& tab = open_tab(favorite->url);
    set_tab_node(tab, node);
    set_tab_space(tab, {});
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
    if (!ok)
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
    if (dialog->exec() == QDialog::Accepted) {
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
    if (answer != QMessageBox::Yes)
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

void ArcSidebar::switch_to_space(QString const& id)
{
    auto& store = SpaceStore::the();
    if (!store.space(id))
        return;

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
        if (&tab->view().session() == expected_session || tab->view().is_private() == WebView::IsPrivate::Yes || is_blank(*tab))
            continue;

        auto url = tab->view().url();
        auto index = m_tabs.index_of(tab);
        auto was_current = m_window->current_tab() == tab;
        auto space = space_of(*tab);
        auto node = node_of(*tab);
        set_tab_node(*tab, {}); // So the reopened tab is the one bound to the node.

        Tab* reopened = nullptr;
        {
            ScopedProfileForNewViews scope(profile);
            reopened = &m_window->new_tab_from_url(url, was_current ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No, BrowserWindow::TabLocation::at_index(index + 1));
        }
        set_tab_space(*reopened, space);
        set_tab_node(*reopened, node);
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

    // Tabs whose space went away (deleted, or the starter space an import replaced) join the current one.
    for (auto* tab : tabs()) {
        auto tab_space = space_of(*tab);
        if (!tab_space.isEmpty() && !SpaceStore::the().space(tab_space)) {
            set_tab_space(*tab, m_space_id);
            if (!is_favorite_node(node_of(*tab)))
                set_tab_node(*tab, {});
        }
    }

    auto muted = ChromeStyle::chrome_muted_text(palette());
    m_space_title->setText(space ? space->title : QString());
    m_space_title->setStyleSheet(QString("QLabel { color: %1; font-weight: 600; font-size: 13px; }").arg(muted.name()));
    auto hover = ChromeStyle::is_dark(palette()) ? "rgba(255,255,255,0.07)" : "rgba(0,0,0,0.05)";
    m_header->setStyleSheet(QString("QWidget#ArcSpaceHeader { border-radius: 9px; background: transparent; }"
                                    "QWidget#ArcSpaceHeader[hovered=\"true\"] { background: %1; }")
                                .arg(hover));
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
    while (auto* item = m_favorites_layout->takeAt(0)) {
        if (auto* widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    auto favorites = SpaceStore::the().favorites_for_profile(current_profile());
    m_favorites->setVisible(!favorites.isEmpty());
    if (favorites.isEmpty())
        return;

    auto dark = ChromeStyle::is_dark(palette());
    auto tile = dark ? "rgba(255,255,255,0.09)" : "rgba(255,255,255,0.45)";
    auto tile_hover = dark ? "rgba(255,255,255,0.14)" : "rgba(255,255,255,0.65)";
    auto tile_checked = dark ? "rgba(255,255,255,0.24)" : "rgba(255,255,255,0.95)";
    auto style = QString("QToolButton { background: %1; border: none; border-radius: 12px; }"
                         "QToolButton:hover { background: %2; }"
                         "QToolButton:checked { background: %3; }")
                     .arg(tile, tile_hover, tile_checked);

    // Always four equal columns, like Arc, so one favorite is a tile rather than a full-width bar.
    constexpr int columns = 4;
    for (int column = 0; column < columns; ++column)
        m_favorites_layout->setColumnStretch(column, 1);
    auto* current = m_window ? m_window->current_tab() : nullptr;

    for (int i = 0; i < favorites.size(); ++i) {
        auto const& favorite = favorites[i];
        auto* tab = tab_for_node(favorite_node(favorite.id));

        auto* button = new QToolButton(m_favorites);
        button->setIcon(icon_for(favorite.url, tab));
        button->setIconSize(QSize(16, 16));
        button->setToolTip(favorite.title.isEmpty() ? favorite.url : favorite.title);
        button->setCheckable(true);
        button->setChecked(tab && tab == current);
        button->setFixedHeight(FAVORITE_TILE_HEIGHT);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setFocusPolicy(Qt::NoFocus);
        button->setStyleSheet(style);
        button->setContextMenuPolicy(Qt::CustomContextMenu);

        auto id = favorite.id;
        connect(button, &QToolButton::clicked, this, [this, id] { open_or_activate_favorite(id); });
        connect(button, &QToolButton::customContextMenuRequested, this, [this, id, button](QPoint const& position) {
            show_favorite_menu(id, button->mapToGlobal(position));
        });

        m_favorites_layout->addWidget(button, i / columns, i % columns);
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
            QString title = node.title;
            if (title.isEmpty())
                title = tab ? title_of(*tab) : (node.saved_title.isEmpty() ? node.url : node.saved_title);
            item->setText(title);
            item->setIcon(icon_for(tab ? url_of(*tab) : node.url, tab));
            item->setData(static_cast<int>(RowKind::PinnedTab), KindRole);
            item->setData(tab != nullptr, IsLoadedRole);
            item->setData(tab && !pinned_urls_match(node.url, url_of(*tab)), NavigatedAwayRole);
            item->setData(QVariant::fromValue(reinterpret_cast<quintptr>(tab)), TabRole);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        }
        parent.appendRow(item);
    }
}

void ArcSidebar::rebuild_tree()
{
    // Restoring folders' expansion isn't the user opening them: no expand animation, which would replay on every
    // tab change. Nor intermediate repaints: the list changes once, in place.
    m_tree->setAnimated(false);
    m_tree->setUpdatesEnabled(false);
    auto restore = qScopeGuard([this] {
        m_tree->setUpdatesEnabled(true);
        m_tree->setAnimated(true);
    });
    auto scroll = m_tree->verticalScrollBar()->value();
    m_model->clear();

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
    m_tree->verticalScrollBar()->setValue(scroll);
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
    m_shelf_opacity_animation->setStartValue(m_shelf->isVisible() ? m_shelf_opacity->opacity() : 0.0);
    m_shelf_opacity_animation->setEndValue(1.0);
    m_shelf_animation->setDirection(QAbstractAnimation::Forward);
    m_shelf_hiding = false;
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
    m_shelf_opacity_animation->setStartValue(m_shelf_opacity->opacity());
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
    // Everything but the downloads button is rebuilt.
    while (auto* item = m_switcher_layout->takeAt(0)) {
        if (auto* widget = item->widget(); widget && widget != m_downloads_button)
            widget->deleteLater();
        delete item;
    }

    auto const& spaces = SpaceStore::the().spaces();

    // [downloads]  [space] [space] ...  [+], spread across the bar like Arc's.
    m_switcher_layout->addWidget(m_downloads_button);
    m_switcher_layout->addStretch(1);

    for (int i = 0; i < spaces.size(); ++i) {
        auto const& space = spaces[i];
        auto active = space.id == m_space_id;

        auto* button = new SymbolButton(m_switcher);
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
    }

    auto* add = new SymbolButton(m_switcher);
    add->set_symbol("plus");
    add->set_dimmed(true);
    add->setToolTip("New Space");
    connect(add, &QAbstractButton::clicked, this, &ArcSidebar::new_space);
    m_switcher_layout->addWidget(add);
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
    for_each_item(*m_model->invisibleRootItem(), [&](QStandardItem& item) {
        if (tab_in_row(item.index()) != &tab)
            return;
        if (row_kind(item.index()) == RowKind::PinnedTab) {
            auto const* pinned = SpaceStore::the().node(node);
            if (pinned && pinned->title.isEmpty())
                item.setText(title);
            auto away = pinned && !pinned_urls_match(pinned->url, url_of(tab));
            if (item.data(NavigatedAwayRole).toBool() != away)
                item.setData(away, NavigatedAwayRole);
        } else {
            item.setText(title);
        }
        item.setIcon(icon);
    });

    if (!node.isEmpty() && !tab.title().isEmpty())
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
void ArcSidebar::apply_toolbar_inset()
{
    if (!m_window)
        return;
    for (auto* toolbar : m_window->findChildren<QWidget*>("LadybirdNavigationToolbar")) {
        if (auto* layout = toolbar->layout()) {
            auto margins = layout->contentsMargins();
            if (margins.left() != HIDDEN_TOOLBAR_LEFT_MARGIN) {
                margins.setLeft(HIDDEN_TOOLBAR_LEFT_MARGIN);
                layout->setContentsMargins(margins);
            }
        }
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
void ArcSidebar::apply_space_tint()
{
    static QColor applied;
    static bool initialized = false;
    auto color = space_color();
    if (initialized && color == applied)
        return;
    initialized = true;
    applied = color;

    ChromeStyle::set_chrome_background_tint(color, ChromeStyle::is_dark(palette()) ? 0.26 : 0.38);
    // Upstream's chrome widgets restyle themselves on PaletteChange (the path a system theme change takes).
    // Notify only those; Qt's own widgets would reset their style sheets on a synthetic event.
    auto notify = [](QWidget* widget) {
        QEvent palette_change(QEvent::PaletteChange);
        QApplication::sendEvent(widget, &palette_change);
        widget->update();
    };
    for (auto* top_level : QApplication::topLevelWidgets()) {
        auto* browser_window = qobject_cast<BrowserWindow*>(top_level);
        if (!browser_window)
            continue;
        notify(browser_window);
        for (auto* tabs : browser_window->findChildren<TabWidget*>())
            notify(tabs);
        for (auto* tab : browser_window->findChildren<Tab*>())
            notify(tab);
        for (auto* edit : browser_window->findChildren<LocationEdit*>())
            notify(edit);
        for (auto* widget : browser_window->findChildren<QWidget*>())
            widget->update();
    }

    // Arc has no line between the sidebar and the page; upstream's column style draws one.
    if (auto* column = parentWidget())
        column->setStyleSheet("QWidget#LadybirdVerticalTabBar { border: none; background: transparent; }");
}

// --- Interaction --------------------------------------------------------------------------------

void ArcSidebar::on_row_clicked(QModelIndex const& index)
{
    switch (row_kind(index)) {
    case RowKind::PinnedTab:
        open_or_activate_node(index.data(IdRole).toString());
        break;
    case RowKind::Folder:
        m_tree->setExpanded(index, !m_tree->isExpanded(index));
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
            tab = &open_tab(node->url);
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
        if (event->type() == QEvent::Resize && m_mode == Mode::Hidden && m_host && !m_host_lifting_off && !m_host_docking)
            size_host();
        else if (event->type() == QEvent::WindowActivate && m_mode_set)
            set_window_controls_hidden(*window(), m_mode == Mode::Hidden || m_host_docking); // AppKit can restore them on activation.
        if (event->type() == QEvent::WindowActivate)
            apply_profile_for_new_views();
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
                if (auto* tab = tab_in_row(index))
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
        menu.addAction("Close Tab", [this, tab] {
            if (tab)
                close_tab(*tab);
        });
        break;

    case RowKind::PinnedTab: {
        menu.addAction("Rename…", [this, node_id] { rename_node(node_id); });
        if (tab && persistent.data(NavigatedAwayRole).toBool()) {
            menu.addAction("Back to Pinned URL", [this, node_id] { back_to_pinned_url(node_id); });
            menu.addAction("Separate From Pinned Tab", [this, node_id] { separate_from_pinned_tab(node_id); });
            menu.addAction("Set Pinned URL to Current Page", [this, tab, node_id] {
                if (!tab)
                    return;
                SpaceStore::the().set_node_url(node_id, url_of(*tab));
                update_tab_rows(*tab);
            });
        }
        menu.addAction("Add to Favorites", [this, node_id] {
            if (auto const* node = SpaceStore::the().node(node_id))
                SpaceStore::the().add_favorite(node->url, node->title.isEmpty() ? node->saved_title : node->title, -1, current_profile());
        });
        add_move_to_space_menu(menu, m_space_id, move);
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
        menu.addAction("Rename…", [this, node_id] { rename_node(node_id); });
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
