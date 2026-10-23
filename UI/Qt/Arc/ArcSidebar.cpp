/*
 * Arc-style sidebar: favorites, the current space's pinned folders and tabs, Today's tabs,
 * and a space switcher.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <UI/Qt/Arc/ArcSidebar.h>
#include <UI/Qt/Arc/CommandBar.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/WebContentView.h>

#include <QComboBox>
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
#include <QMouseEvent>
#include <QWindow>
#include <QGraphicsOpacityEffect>
#include <QApplication>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>
#include <iterator>

namespace Ladybird::Arc {

static constexpr char const* SPACE_PROPERTY = "arc_space";
static constexpr char const* NODE_PROPERTY = "arc_node";
static constexpr auto FAVORITE_PREFIX = "fav:";
static constexpr int ExpandedRole = Qt::UserRole + 20;
static constexpr int FAVORITE_TILE_HEIGHT = 54;
static constexpr int TOP_STRIP_HEIGHT = 34;

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

static QString rgba(QColor const& color)
{
    return QString("rgba(%1, %2, %3, %4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());
}

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

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // The window buttons (traffic lights) sit over this strip; dragging it moves the window.
    m_top_strip = new QWidget(this);
    m_top_strip->setFixedHeight(TOP_STRIP_HEIGHT);
    m_top_strip->installEventFilter(this);
    layout->addWidget(m_top_strip);

    m_favorites = new QWidget(this);
    m_favorites_layout = new QGridLayout(m_favorites);
    m_favorites_layout->setContentsMargins(0, 0, 0, 6);
    m_favorites_layout->setSpacing(8);
    m_favorites->setAcceptDrops(true);
    m_favorites->installEventFilter(this);
    layout->addWidget(m_favorites);

    m_space_title = new QLabel(this);
    m_space_title->setContentsMargins(8, 2, 8, 2);
    m_space_title->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_space_title, &QLabel::customContextMenuRequested, this, [this](QPoint const& position) {
        show_space_menu(m_space_id, m_space_title->mapToGlobal(position));
    });
    layout->addWidget(m_space_title);

    m_model = new SidebarModel(this);
    m_tree = new SidebarTree(this);
    m_tree->setModel(m_model);
    layout->addWidget(m_tree, 1);

    m_switcher = new QWidget(this);
    m_switcher_layout = new QHBoxLayout(m_switcher);
    m_switcher_layout->setContentsMargins(2, 4, 2, 2);
    m_switcher_layout->setSpacing(2);
    layout->addWidget(m_switcher);

    connect(m_tree, &SidebarTree::row_clicked, this, &ArcSidebar::on_row_clicked);
    connect(m_tree, &SidebarTree::close_clicked, this, &ArcSidebar::on_close_clicked);
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
        track_tabs();
        schedule_rebuild();
        save_session();
    });
    connect(&m_tabs, &TabWidget::current_tab_changed, this, &ArcSidebar::on_current_tab_changed);
    connect(&store, &SpaceStore::changed, this, &ArcSidebar::schedule_rebuild);

    m_space_id = store.space(store.last_active_space_id()) ? store.last_active_space_id() : store.first_space_id();
    restore_session_assignments();
    track_tabs();

    if (auto* current = m_window->current_tab()) {
        auto space = space_of(*current);
        if (!space.isEmpty() && store.space(space))
            m_space_id = space;
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
        if (space_of(*tab) == space_id && node_of(*tab).isEmpty())
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
    auto& tab = m_window->new_tab_from_url(ak_url_from_qurl(QUrl(url)), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
    set_tab_space(tab, m_space_id);
    return tab;
}

void ArcSidebar::activate(Tab& tab)
{
    if (auto index = m_tabs.index_of(&tab); index >= 0)
        m_window->activate_tab(index);
}

void ArcSidebar::close_tab(Tab& tab)
{
    // Stay in this space: pick a neighbor from the same space before upstream picks any neighbor.
    if (m_window->current_tab() == &tab) {
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
                if (other != &tab && space_of(*other) == space) {
                    next = other;
                    break;
                }
            }
        }
        if (next)
            activate(*next);
    }
    if (auto index = m_tabs.index_of(&tab); index >= 0)
        m_window->request_to_close_tab(index);
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
    auto id = SpaceStore::the().add_favorite(url_of(tab), title_of(tab), index);
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

static QDialog* make_space_dialog(QWidget* parent, QString const& title, QString const& icon, QColor const& color, QLineEdit*& name_edit, QLineEdit*& icon_edit, QComboBox*& color_box)
{
    auto* dialog = new QDialog(parent);
    dialog->setWindowTitle("Space");
    auto* form = new QFormLayout(dialog);

    name_edit = new QLineEdit(title, dialog);
    icon_edit = new QLineEdit(icon, dialog);
    icon_edit->setPlaceholderText("An emoji, or leave empty");
    icon_edit->setMaxLength(8);
    color_box = new QComboBox(dialog);
    for (auto const& preset : COLOR_PRESETS) {
        QPixmap swatch(14, 14);
        swatch.fill(QColor(preset.hex));
        color_box->addItem(QIcon(swatch), preset.name, QString(preset.hex));
        if (QColor(preset.hex) == color)
            color_box->setCurrentIndex(color_box->count() - 1);
    }

    form->addRow("Name", name_edit);
    form->addRow("Icon", icon_edit);
    form->addRow("Color", color_box);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    form->addRow(buttons);
    return dialog;
}

void ArcSidebar::new_space()
{
    auto& store = SpaceStore::the();
    auto preset = COLOR_PRESETS[store.spaces().size() % std::size(COLOR_PRESETS)];

    QLineEdit* name_edit = nullptr;
    QLineEdit* icon_edit = nullptr;
    QComboBox* color_box = nullptr;
    auto* dialog = make_space_dialog(m_window, "New Space", {}, QColor(preset.hex), name_edit, icon_edit, color_box);
    dialog->setWindowTitle("New Space");
    name_edit->selectAll();
    if (dialog->exec() == QDialog::Accepted) {
        auto name = name_edit->text().trimmed();
        auto id = store.add_space(name.isEmpty() ? "New Space" : name, icon_edit->text().trimmed(), QColor(color_box->currentData().toString()));
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
    QLineEdit* icon_edit = nullptr;
    QComboBox* color_box = nullptr;
    auto* dialog = make_space_dialog(m_window, space->title, space->icon, space->color, name_edit, icon_edit, color_box);
    if (dialog->exec() == QDialog::Accepted) {
        auto name = name_edit->text().trimmed();
        store.update_space(space_id, name.isEmpty() ? space->title : name, icon_edit->text().trimmed(), QColor(color_box->currentData().toString()));
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

    update_current_highlight();
    rebuild_favorites();
}

// --- Views --------------------------------------------------------------------------------------

QColor ArcSidebar::space_color() const
{
    auto const* space = SpaceStore::the().space(m_space_id);
    return space ? space->color : QColor();
}

static QHash<QString, QIcon>& favicon_cache()
{
    static QHash<QString, QIcon> cache;
    return cache;
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
    auto host = QUrl(url).host();
    if (tab && !tab->favicon().isNull()) {
        if (!host.isEmpty())
            favicon_cache().insert(host, tab->favicon());
        return tab->favicon();
    }
    if (auto it = favicon_cache().find(host); it != favicon_cache().end())
        return *it;
    return letter_icon(host);
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

    auto const* space = SpaceStore::the().space(m_space_id);
    if (!space) {
        m_space_id = SpaceStore::the().first_space_id();
        space = SpaceStore::the().space(m_space_id);
    }

    auto muted = ChromeStyle::chrome_muted_text(palette());
    m_space_title->setText(space ? (space->icon.isEmpty() ? space->title : space->icon + "  " + space->title) : QString());
    m_space_title->setStyleSheet(QString("QLabel { color: %1; font-weight: 600; font-size: 13px; }").arg(muted.name()));
    m_tree->delegate().accent = space_color();

    rebuild_favorites();
    rebuild_tree();
    rebuild_switcher();
    update();
    if (window()->isActiveWindow() || !qApp->activeWindow())
        apply_space_tint();
}

void ArcSidebar::rebuild_favorites()
{
    while (auto* item = m_favorites_layout->takeAt(0)) {
        if (auto* widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    auto const& favorites = SpaceStore::the().favorites();
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
        button->setIconSize(QSize(22, 22));
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
            item->setData(QVariant::fromValue(reinterpret_cast<quintptr>(tab)), TabRole);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        }
        parent.appendRow(item);
    }
}

void ArcSidebar::rebuild_tree()
{
    auto scroll = m_tree->verticalScrollBar()->value();
    m_model->clear();

    auto const* space = SpaceStore::the().space(m_space_id);
    if (!space)
        return;

    auto& root = *m_model->invisibleRootItem();
    append_nodes(root, space->pinned);

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

    for_each_item(root, [&](QStandardItem& item) {
        if (row_kind(item.index()) == RowKind::Folder)
            m_tree->setExpanded(item.index(), item.data(ExpandedRole).toBool());
    });

    update_current_highlight();
    m_tree->verticalScrollBar()->setValue(scroll);
}

void ArcSidebar::rebuild_switcher()
{
    while (auto* item = m_switcher_layout->takeAt(0)) {
        if (auto* widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    auto const& spaces = SpaceStore::the().spaces();
    auto muted = ChromeStyle::chrome_muted_text(palette());

    for (int i = 0; i < spaces.size(); ++i) {
        auto const& space = spaces[i];
        auto active = space.id == m_space_id;

        auto* button = new QToolButton(m_switcher);
        button->setText(space.icon.isEmpty() ? space.title.left(1).toUpper() : space.icon);
        button->setToolTip(i < 9 ? QString("%1  (%2%3)").arg(space.title, SPACE_SHORTCUT_MODIFIER == Qt::META ? "⌃" : "Alt+").arg(i + 1) : space.title);
        button->setFixedSize(28, 28);
        button->setFocusPolicy(Qt::NoFocus);
        button->setContextMenuPolicy(Qt::CustomContextMenu);

        auto hover = ChromeStyle::chrome_surface_hover(palette());
        hover.setAlpha(120);
        button->setStyleSheet(QString("QToolButton { background: transparent; border: none; border-radius: 8px; color: %1; font-weight: 700; font-size: 14px; }"
                                      "QToolButton:hover { background: %2; }")
                                  .arg(ChromeStyle::chrome_text(palette()).name(), rgba(hover)));
        if (!active) {
            // Emoji can't be recolored, so dim inactive spaces with opacity, as Arc does.
            auto* dim = new QGraphicsOpacityEffect(button);
            dim->setOpacity(0.4);
            button->setGraphicsEffect(dim);
        }

        auto id = space.id;
        connect(button, &QToolButton::clicked, this, [this, id] { switch_to_space(id); });
        connect(button, &QToolButton::customContextMenuRequested, this, [this, id, button](QPoint const& position) {
            show_space_menu(id, button->mapToGlobal(position));
        });
        m_switcher_layout->addWidget(button);
        m_switcher_layout->addStretch(1);
    }

    auto* add = new QToolButton(m_switcher);
    add->setText("+");
    add->setToolTip("New Space");
    add->setFixedSize(28, 28);
    add->setFocusPolicy(Qt::NoFocus);
    add->setStyleSheet(QString("QToolButton { background: transparent; border: none; border-radius: 8px; color: %1; font-size: 16px; }"
                               "QToolButton:hover { background: %2; }")
                           .arg(muted.name(), rgba(ChromeStyle::chrome_surface_hover(palette()))));
    connect(add, &QToolButton::clicked, this, &ArcSidebar::new_space);
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

void ArcSidebar::paintEvent(QPaintEvent*)
{
    // Same (tinted) chrome background as the rest of the window; see apply_space_tint().
    QPainter painter(this);
    painter.fillRect(rect(), ChromeStyle::chrome_background(palette()));
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
        column->setStyleSheet("QWidget#LadybirdVerticalTabBar { border: none; }");
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
    if (watched == m_top_strip) {
        if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            if (auto* handle = window()->windowHandle())
                handle->startSystemMove();
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
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
                    auto favorite_id = SpaceStore::the().add_favorite(node->url, title);
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
                SpaceStore::the().add_favorite(url.toString(), url.host());
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
        if (tab) {
            menu.addAction("Save Current Page as Pinned URL", [tab, node_id] {
                if (tab)
                    SpaceStore::the().set_node_url(node_id, url_of(*tab));
            });
            menu.addAction("Go Back to Pinned URL", [tab, node_id] {
                auto const* node = SpaceStore::the().node(node_id);
                if (tab && node)
                    tab->navigate(ak_url_from_qurl(QUrl(node->url)));
            });
        }
        menu.addAction("Add to Favorites", [node_id] {
            if (auto const* node = SpaceStore::the().node(node_id))
                SpaceStore::the().add_favorite(node->url, node->title.isEmpty() ? node->saved_title : node->title);
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

    menu.exec(m_tree->viewport()->mapToGlobal(position));
}

void ArcSidebar::show_space_menu(QString const& space_id, QPoint const& global_position)
{
    auto& store = SpaceStore::the();
    auto const* space = store.space(space_id);
    if (!space)
        return;

    QMenu menu(this);
    menu.addAction("Edit Space…", [this, space_id] { edit_space(space_id); });

    auto* colors = menu.addMenu("Color");
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

    int index = 0;
    for (int i = 0; i < store.spaces().size(); ++i) {
        if (store.spaces()[i].id == space_id)
            index = i;
    }
    if (index > 0)
        menu.addAction("Move Left", [space_id, index] { SpaceStore::the().move_space(space_id, index - 1); });
    if (index + 1 < store.spaces().size())
        menu.addAction("Move Right", [space_id, index] { SpaceStore::the().move_space(space_id, index + 1); });

    menu.addSeparator();
    menu.addAction("New Space…", [this] { new_space(); });
    menu.addAction("New Folder", [this] { new_folder({}, -1); });
    auto* remove = menu.addAction("Delete Space…", [this, space_id] { delete_space(space_id); });
    remove->setEnabled(store.spaces().size() > 1);

    menu.exec(global_position);
}

void ArcSidebar::show_favorite_menu(QString const& favorite_id, QPoint const& global_position)
{
    auto& store = SpaceStore::the();
    int index = 0;
    for (int i = 0; i < store.favorites().size(); ++i) {
        if (store.favorites()[i].id == favorite_id)
            index = i;
    }

    QMenu menu(this);
    if (index > 0)
        menu.addAction("Move Left", [favorite_id, index] { SpaceStore::the().move_favorite(favorite_id, index - 1); });
    if (index + 1 < store.favorites().size())
        menu.addAction("Move Right", [favorite_id, index] { SpaceStore::the().move_favorite(favorite_id, index + 1); });
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
    menu.exec(global_position);
}

}
