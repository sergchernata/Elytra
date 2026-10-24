/*
 * Integration tests of the Arc sidebar against real browser windows. See ArcSelfTest.h.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/PictureInPictureControls.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Arc/AddressBar.h>
#include <UI/Qt/Arc/ArcSelfTest.h>
#include <UI/Qt/Arc/ArcSidebar.h>
#include <UI/Qt/Arc/CommandBar.h>
#include <UI/Qt/Arc/FloatingPanel.h>
#include <UI/Qt/Arc/LittleArc.h>
#include <UI/Qt/Arc/MiniPlayer.h>
#include <UI/Qt/Arc/OnePassword.h>
#include <UI/Qt/Arc/ProfileSessions.h>
#include <UI/Qt/Arc/SidebarTree.h>
#include <UI/Qt/Arc/SidebarWidgets.h>
#include <UI/Qt/Arc/SpaceSlide.h>
#include <UI/Qt/Arc/SpaceStore.h>
#include <UI/Qt/Arc/SymbolButton.h>
#include <UI/Qt/Arc/Toast.h>
#include <UI/Qt/Arc/UpdateChecker.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/WebContentView.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <cstdio>
#include <functional>
#include <unistd.h>

namespace Ladybird::Arc {

static constexpr int TOOLBAR_LEFT_MARGIN = 2; // ArcSidebar's HIDDEN_TOOLBAR_LEFT_MARGIN.

class SelfTest final : public QObject {
public:
    explicit SelfTest(ArcSidebar& sidebar)
        : m_sidebar(sidebar)
    {
    }

    int run();

protected:
    // Records every paint of a navigation toolbar with the wrong left margin (see toolbar_stays_aligned()).
    virtual bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (m_watch_toolbars && event->type() == QEvent::Paint && watched->objectName() == "LadybirdNavigationToolbar") {
            auto* layout = static_cast<QWidget*>(watched)->layout();
            if (layout && layout->contentsMargins().left() != TOOLBAR_LEFT_MARGIN)
                ++m_misaligned_toolbar_paints;
        }
        return false;
    }

private:
    // --- Harness ---------------------------------------------------------------------------------

    void check(bool condition, char const* name, QString const& detail = {})
    {
        if (condition) {
            std::printf("PASS: %s\n", name);
        } else {
            std::printf("FAIL: %s%s%s\n", name, detail.isEmpty() ? "" : ": ", qPrintable(detail));
            ++m_failures;
        }
        std::fflush(stdout);
    }

    static bool wait_until(std::function<bool()> const& condition, int timeout_ms = 8000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!condition()) {
            if (timer.elapsed() > timeout_ms)
                return false;
            QEventLoop loop;
            QTimer::singleShot(10, &loop, &QEventLoop::quit);
            loop.exec();
        }
        return true;
    }

    static void settle(int ms)
    {
        QElapsedTimer timer;
        timer.start();
        wait_until([&] { return timer.elapsed() >= ms; }, ms + 1000);
    }

    static QList<BrowserWindow*> browser_windows()
    {
        QList<BrowserWindow*> windows;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* window = qobject_cast<BrowserWindow*>(widget); window && window->isVisible())
                windows.append(window);
        }
        return windows;
    }

    static QString page(QString const& title, QString const& body = {})
    {
        return QString("data:text/html,<title>%1</title>%2").arg(title, body.isEmpty() ? title : body);
    }

    // --- Access to the sidebar -------------------------------------------------------------------

    ArcSidebar& s() { return m_sidebar; }
    BrowserWindow& window() { return *m_sidebar.m_window; }
    int tab_count() { return m_sidebar.m_tabs.count(); }
    QScrollBar& list_scroll() { return *m_sidebar.m_tree->verticalScrollBar(); }

    bool loaded(Tab& tab) { return !tab.is_loading() && ArcSidebar::url_of(tab).startsWith("data:"); }
    // Whether a tab is at a page (URLs come back percent-encoded).
    static bool at(Tab& tab, QString const& url) { return QUrl(ArcSidebar::url_of(tab)) == QUrl(url, QUrl::TolerantMode) || pinned_urls_match(url, ArcSidebar::url_of(tab)); }

    QModelIndex row_for_node(QString const& node_id)
    {
        QModelIndex found;
        std::function<void(QStandardItem&)> walk = [&](QStandardItem& parent) {
            for (int row = 0; row < parent.rowCount(); ++row) {
                auto* item = parent.child(row);
                if (item->data(IdRole).toString() == node_id)
                    found = item->index();
                walk(*item);
            }
        };
        walk(*m_sidebar.m_model->invisibleRootItem());
        return found;
    }

    void rebuild_now()
    {
        m_sidebar.rebuild();
        settle(50);
    }

    // --- Scenarios -------------------------------------------------------------------------------

    void seed();
    void toolbar_stays_aligned();
    void unloaded_pinned_tab_opens_in_the_spare();
    void loading_pinned_row_keeps_its_saved_title();
    void closing_a_spaces_last_tab_keeps_the_window();
    void clear_closes_only_this_spaces_today_tabs();
    void list_keeps_its_scroll_position();
    void each_space_keeps_its_scroll_position();
    void favorites_update_in_place();
    void pinned_tab_away_back_and_separate();
    void a_pinned_tab_that_redirects_isnt_away();
    void opening_a_folder_leaves_the_list_alone_while_it_slides();
    void favorites_never_overlap_when_they_change();
    void closing_a_tab_returns_to_the_one_before();
    void downloads_shelf_rows_rise_in();
    void leaving_a_playing_video_shows_it_in_picture_in_picture();
    void minimizing_picture_in_picture_moves_the_video_to_the_sidebar_player();
    void the_docked_sidebar_has_its_toggle_beside_the_window_buttons();
    void little_arc_opens_a_page_and_moves_it_into_a_space();
    void profiles_get_their_own_session();
    void list_ends_with_room_to_double_click();
    void update_pill_hidden_without_a_release();
    void background_tabs_show_while_loading_and_stay_open();
    void the_empty_page_becomes_a_tab_once_navigated();
    void tabs_of_removed_pinned_tabs_and_favorites_move_to_today();
    void a_malformed_pinned_url_doesnt_crash();
    void popups_close_themselves();
    void private_windows_write_nothing();
    void address_bar_edits_the_current_tab();
    void arc_shortcuts_step_through_the_sidebar();
    void swiping_the_sidebar_switches_spaces();
    void pinned_tabs_rename_in_place();
    void duplicate_and_close_tabs_below();
    void switching_spaces_slides();
    void the_slide_and_color_fade_run_smoothly();
    void switching_spaces_keeps_the_switcher_in_place();
    struct ChromeTiles {
        QMap<QString, int> unpainted;
        QMap<QString, int> untinted;
    };
    ChromeTiles chrome_tiles_ignoring_the_tint(bool fade_step, Function<void()> after_tint = {});
    void every_chrome_surface_takes_the_space_color();
    void fills_from_one_password();

    // Sends a key the way Qt does for a shortcut: the override first (claimed if Arc handles the key), then the press.
    // Returns whether the override was claimed, i.e. upstream's action for the same key didn't fire.
    bool press_shortcut(int key, Qt::KeyboardModifiers modifiers)
    {
        QWidget* target = QApplication::focusWidget();
        if (!target || target->window() != &window())
            target = window().current_tab() ? static_cast<QWidget*>(&window().current_tab()->view()) : &window();
        QKeyEvent override(QEvent::ShortcutOverride, key, modifiers);
        override.ignore();
        QApplication::sendEvent(target, &override);
        auto claimed = override.isAccepted();
        QKeyEvent down(QEvent::KeyPress, key, modifiers);
        QApplication::sendEvent(target, &down);
        return claimed;
    }

    ArcSidebar& m_sidebar;
    int m_failures { 0 };
    bool m_watch_toolbars { false };
    int m_misaligned_toolbar_paints { 0 };

    QString m_alpha;
    QString m_beta;
    QStringList m_alpha_pinned; // Top-level pinned tabs, in order.
    QString m_folder;
};

void SelfTest::seed()
{
    auto& store = SpaceStore::the();
    m_alpha = store.add_space("Alpha", "sf:house", QColor("#5b8def"));

    // A long list, so it scrolls.
    m_folder = store.add_folder(m_alpha, {}, -1, "Folder");
    for (int i = 0; i < 40; ++i)
        store.add_pinned_tab(m_alpha, m_folder, -1, page(QString("F%1").arg(i)), QString("Folder tab %1").arg(i));
    store.set_folder_expanded(m_folder, true);
    for (int i = 0; i < 6; ++i)
        m_alpha_pinned.append(store.add_pinned_tab(m_alpha, {}, -1, page(QString("P%1").arg(i)), QString("Saved P%1").arg(i)));
    store.add_favorite(page("Fav A"), "Fav A");
    store.add_favorite(page("Fav B"), "Fav B");

    // A space in another profile (only an import can set one).
    Space beta;
    beta.source_id = "self-test-beta";
    beta.title = "Beta";
    beta.icon = "sf:briefcase";
    beta.color = QColor("#9b7fd4");
    beta.profile = "SelfTest";
    store.merge_imported({ beta }, {});
    for (auto const& space : store.spaces()) {
        if (space.source_id == "self-test-beta")
            m_beta = space.id;
    }

    s().switch_to_space(m_alpha);
    rebuild_now();
}

void SelfTest::toolbar_stays_aligned()
{
    // (Launch first: a window opening on the empty page edits its address in the command bar, where upstream
    // would focus its location field.)
    auto* bar = window().findChild<CommandBar*>(QString(), Qt::FindDirectChildrenOnly);
    check(bar && bar->isVisible() && bar->mode() == CommandBar::Mode::EditCurrentTab && bar->text().isEmpty(),
        "a window opening on the empty page opens the command bar, empty");
    if (bar)
        bar->dismiss();

    // Regression: a new tab's toolbar showed at upstream's margin for a frame, then jumped left.
    m_misaligned_toolbar_paints = 0;
    m_watch_toolbars = true;
    qApp->installEventFilter(this);

    auto& first = *s().open_tab(page("Toolbar 1"));
    wait_until([&] { return loaded(first); });
    auto& second = *s().open_tab(page("Toolbar 2"));
    wait_until([&] { return loaded(second); });
    s().activate(first);
    settle(150);
    s().activate(second);
    settle(150);
    s().close_tab(second);
    settle(150);
    s().close_tab(first);
    settle(300);

    m_watch_toolbars = false;
    qApp->removeEventFilter(this);
    check(m_misaligned_toolbar_paints == 0, "toolbars never paint at upstream's margin while tabs open, switch and close",
        QString("%1 misaligned paints").arg(m_misaligned_toolbar_paints));
}

void SelfTest::unloaded_pinned_tab_opens_in_the_spare()
{
    check(wait_until([&] { return !s().m_spare_tab.isNull(); }), "a spare tab is ready after launch");
    QPointer<Tab> spare = s().m_spare_tab;
    auto tabs_before = tab_count();
    auto node = m_alpha_pinned[0];
    check(!s().tab_for_node(node), "the pinned tab starts unloaded");

    s().open_or_activate_node(node);
    check(spare && s().tab_for_node(node) == spare.data(), "opening an unloaded pinned tab uses the spare tab");
    check(tab_count() == tabs_before, "and builds no new tab while opening it");
    check(spare && s().m_window->current_tab() == spare.data(), "and shows it");
    check(wait_until([&] { return s().m_spare_tab && s().m_spare_tab != spare; }), "a new spare is made afterwards");
    check(s().m_spare_tab && ArcSidebar::is_blank(*s().m_spare_tab) && s().today_tabs(m_alpha).indexOf(s().m_spare_tab.data()) < 0,
        "the spare is blank and not listed");
    if (spare)
        wait_until([&] { return loaded(*spare); });
}

void SelfTest::loading_pinned_row_keeps_its_saved_title()
{
    // Regression: rows showed "New Tab" and a "|" while their tab loaded.
    auto node = m_alpha_pinned[1];
    QStringList titles_seen;
    bool ever_away = false;
    auto connection = connect(s().m_model, &QAbstractItemModel::dataChanged, this, [&] {
        auto row = row_for_node(node);
        if (!row.isValid())
            return;
        titles_seen.append(row.data(Qt::DisplayRole).toString());
        ever_away |= row.data(NavigatedAwayRole).toBool();
    });
    s().open_or_activate_node(node);
    auto* tab = s().tab_for_node(node);
    wait_until([&] { return tab && loaded(*tab); });
    settle(200);
    disconnect(connection);

    auto bad = std::find_if(titles_seen.begin(), titles_seen.end(), [](auto const& title) {
        return title == "New Tab" || title.startsWith("about:") || title.isEmpty();
    });
    check(bad == titles_seen.end(), "a loading pinned row never shows a blank page's title", bad == titles_seen.end() ? QString() : *bad);
    // While a page loads, its tab's title is its address until the page's own arrives.
    auto address = std::find_if(titles_seen.begin(), titles_seen.end(), [](auto const& title) { return title.startsWith("data:"); });
    check(address == titles_seen.end(), "nor its address while the page's title is on its way", address == titles_seen.end() ? QString() : *address);
    check(!ever_away, "nor a \"|\" while it loads its own URL");
    auto row = row_for_node(node);
    check(row.isValid() && row.data(Qt::DisplayRole).toString() == "P1", "then shows the page's title", row.data(Qt::DisplayRole).toString());
    check(SpaceStore::the().node(node) && !SpaceStore::the().node(node)->saved_title.startsWith("data:"), "and never saves the address as its title");

    // A Today tab going to another page shows its old title until the new one's arrives.
    QPointer<Tab> today = s().open_tab(page("Before"));
    wait_until([&] { return today && loaded(*today) && today->title() == "Before"; });
    QStringList today_titles;
    connection = connect(s().m_model, &QAbstractItemModel::dataChanged, this, [&] {
        for (auto const& index : s().m_model->match(s().m_model->index(0, 0), TabRole, QVariant::fromValue(reinterpret_cast<quintptr>(today.data())), 1, Qt::MatchRecursive))
            today_titles.append(index.data(Qt::DisplayRole).toString());
    });
    if (today)
        today->navigate(ak_url_from_qurl(QUrl(page("After"))));
    wait_until([&] { return today && loaded(*today) && today->title() == "After"; });
    settle(200);
    disconnect(connection);
    auto today_address = std::find_if(today_titles.begin(), today_titles.end(), [](auto const& title) { return title.startsWith("data:"); });
    check(today_address == today_titles.end() && today_titles.contains("After"), "a Today row never flashes its page's address either",
        today_titles.join(" / "));
    if (today)
        s().close_tab(*today);
    settle(200);
}

void SelfTest::closing_a_spaces_last_tab_keeps_the_window()
{
    s().switch_to_space(m_beta);
    settle(200);
    for (auto* tab : s().tabs()) {
        if (ArcSidebar::space_of(*tab) == m_beta && !ArcSidebar::is_blank(*tab))
            s().close_tab(*tab);
    }
    settle(200);
    auto& only = *s().open_tab(page("Only"));
    wait_until([&] { return loaded(only); });
    QPointer<Tab> guarded = &only;
    s().close_tab(only);
    wait_until([&] { return !guarded; });

    check(window().isVisible(), "closing a space's last tab leaves the window open");
    auto* current = window().current_tab();
    check(current && ArcSidebar::is_blank(*current), "and shows an empty page");
    check(s().today_tabs(m_beta).isEmpty(), "which isn't listed");
    check(s().current_space_id() == m_beta, "and the space stays the same");
    s().switch_to_space(m_alpha);
    settle(200);
}

void SelfTest::clear_closes_only_this_spaces_today_tabs()
{
    s().open_background_tabs({ { page("Today A1"), m_alpha, {} }, { page("Today A2"), m_alpha, {} }, { page("Today B1"), m_beta, {} } });
    wait_until([&] { return s().today_tabs(m_alpha).size() >= 2 && s().today_tabs(m_beta).size() >= 1; });
    auto beta_before = s().today_tabs(m_beta).size();
    auto pinned_live = s().tab_for_node(m_alpha_pinned[0]);

    s().switch_to_space(m_alpha);
    s().clear_today();
    check(wait_until([&] { return s().today_tabs(m_alpha).isEmpty(); }), "Clear closes the space's Today tabs");
    check(s().today_tabs(m_beta).size() == beta_before, "but not another space's");
    check(!pinned_live || s().tab_for_node(m_alpha_pinned[0]) == pinned_live, "nor its pinned tabs");
    check(window().isVisible(), "and the window stays open");
}

void SelfTest::list_keeps_its_scroll_position()
{
    // Regression: clicking a tab at the bottom of a scrolled list jumped it (favorites rebuilt and resized the list).
    s().switch_to_space(m_alpha);
    rebuild_now();
    auto& bar = list_scroll();
    check(bar.maximum() > 0, "the seeded list scrolls", QString("max %1").arg(bar.maximum()));

    for (auto position : { bar.maximum(), bar.maximum() / 2 }) {
        bar.setValue(position);
        settle(50);
        auto before = bar.value();
        int moves = 0;
        auto connection = connect(&bar, &QScrollBar::valueChanged, this, [&] { ++moves; });

        // The last pinned tab (bottom) or one in the middle of the folder.
        auto node = position == bar.maximum() ? m_alpha_pinned.last() : SpaceStore::the().node(m_folder)->children[20].id;
        s().on_row_clicked(row_for_node(node));
        auto* tab = s().tab_for_node(node);
        wait_until([&] { return tab && loaded(*tab); });
        settle(1500); // Through the spare tab being replaced, too.
        disconnect(connection);
        auto at_bottom = node == m_alpha_pinned.last();
        check(bar.value() == before && moves == 0, at_bottom ? "clicking a tab at the bottom of the list doesn't move it" : "clicking a tab mid-list doesn't move it",
            QString("%1 -> %2, %3 moves").arg(before).arg(bar.value()).arg(moves));
    }
}

void SelfTest::each_space_keeps_its_scroll_position()
{
    s().switch_to_space(m_alpha);
    rebuild_now();
    auto& bar = list_scroll();
    bar.setValue(bar.maximum() / 3);
    auto alpha_position = bar.value();
    s().switch_to_space(m_beta);
    settle(200);
    check(list_scroll().value() == 0, "another space's list starts at its own position", QString::number(list_scroll().value()));
    s().switch_to_space(m_alpha);
    settle(200);
    check(list_scroll().value() == alpha_position, "and switching back restores this one's",
        QString("%1 vs %2").arg(list_scroll().value()).arg(alpha_position));
}

void SelfTest::favorites_update_in_place()
{
    s().switch_to_space(m_alpha);
    rebuild_now();
    auto buttons = s().m_favorite_buttons;
    check(buttons.size() == 2, "favorites show as tiles", QString::number(buttons.size()));
    rebuild_now();
    rebuild_now();
    if (auto* tab = s().tab_for_node(m_alpha_pinned[0]))
        s().activate(*tab);
    settle(200);
    check(s().m_favorite_buttons == buttons, "refreshing keeps the same tiles (no rebuild that resizes the list)");

    auto id = SpaceStore::the().add_favorite(page("Fav C"), "Fav C");
    settle(200);
    check(s().m_favorite_buttons.size() == 3, "adding a favorite adds a tile", QString::number(s().m_favorite_buttons.size()));
    SpaceStore::the().remove_favorite(id);
    settle(200);
    check(s().m_favorite_buttons.size() == 2, "removing it removes the tile");
}

// Sites send their front page somewhere else as it loads: the pinned tab that lands there isn't "away" (no "|" or
// Back to Pinned URL) until it leaves that page.
void SelfTest::a_pinned_tab_that_redirects_isnt_away()
{
    auto port = qEnvironmentVariable("ARC_SELF_TEST_HTTP_PORT");
    if (port.isEmpty()) {
        check(false, "the test pages are served (run-arc-self-test.sh sets ARC_SELF_TEST_HTTP_PORT)");
        return;
    }
    auto base = QString("http://localhost:%1/").arg(port);
    s().switch_to_space(m_alpha);
    settle(200);
    auto node = SpaceStore::the().add_pinned_tab(m_alpha, {}, -1, base + "redirects.html", "Redirects");
    rebuild_now();
    auto away = [&] { return row_for_node(node).data(NavigatedAwayRole).toBool(); };
    bool ever_away = false;
    auto connection = connect(s().m_model, &QAbstractItemModel::dataChanged, this, [&] { ever_away |= away(); });

    s().open_or_activate_node(node);
    QPointer<Tab> tab = s().tab_for_node(node);
    check(wait_until([&] { return tab && tab->title() == "Landed" && !tab->is_loading(); }, 5000), "the pinned page redirects as it loads");
    settle(2500); // Past the landing's grace period.
    disconnect(connection);
    check(!ever_away && !away(), "and the page it lands on isn't away from the pinned URL");

    if (tab)
        tab->navigate(ak_url_from_qurl(QUrl(base + "elsewhere.html")));
    check(wait_until([&] { return away(); }, 5000), "leaving it is");

    s().back_to_pinned_url(node);
    check(wait_until([&] { return tab && tab->title() == "Landed"; }, 5000), "Back to Pinned URL lands there again");
    settle(2500);
    check(!away(), "and isn't away");

    // Its home is saved with the session, so it survives a restart.
    s().save_session();
    check(wait_until([&] {
        for (auto const& entry : SpaceStore::the().session()) {
            if (entry.node_id == node)
                return entry.home.endsWith("landed.html?session=1");
        }
        return false;
    }, 4000), "where it landed is saved with the session");

    if (tab)
        s().close_tab(*tab);
    SpaceStore::the().remove_node(node);
    rebuild_now();
}

// Opening a folder slides its rows in; anything that rebuilds or relays the list meanwhile draws the rows in their new
// places under the sliding picture of the old ones, so both show.
void SelfTest::opening_a_folder_leaves_the_list_alone_while_it_slides()
{
    s().switch_to_space(m_alpha);
    settle(300);
    auto folder = SpaceStore::the().add_folder(m_alpha, {}, 0, "Slide Folder");
    for (int i = 0; i < 4; ++i)
        SpaceStore::the().add_pinned_tab(m_alpha, folder, -1, page(QString("In folder %1").arg(i)), QString("In folder %1").arg(i));
    rebuild_now();
    settle(300);
    auto row = row_for_node(folder);
    if (!row.isValid()) {
        check(false, "the folder is listed");
        return;
    }
    if (m_sidebar.m_tree->isExpanded(row)) {
        m_sidebar.m_tree->toggle_folder(row);
        settle(600);
        row = row_for_node(folder);
    }

    QStringList changes;
    auto* model = s().m_model;
    QList<QMetaObject::Connection> connections {
        connect(model, &QAbstractItemModel::modelReset, this, [&] { changes.append("reset"); }),
        connect(model, &QAbstractItemModel::rowsRemoved, this, [&] { changes.append("rows removed"); }),
        connect(model, &QAbstractItemModel::rowsInserted, this, [&] { changes.append("rows inserted"); }),
        connect(model, &QAbstractItemModel::layoutChanged, this, [&] { changes.append("layout changed"); }),
        connect(model, &QAbstractItemModel::dataChanged, this, [&](QModelIndex const& from, QModelIndex const&, QList<int> const& roles) {
            QStringList names;
            for (auto role : roles)
                names.append(QString::number(role));
            changes.append(QString("data %1 [%2]").arg(from.data().toString(), names.join(',')));
        }),
    };
    bool rebuilt = false;
    QElapsedTimer timer;
    s().on_row_clicked(row);
    timer.start();
    while (timer.elapsed() < 500) {
        rebuilt |= m_sidebar.m_tree->is_rebuilding();
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    for (auto const& connection : connections)
        disconnect(connection);
    check(m_sidebar.m_tree->isExpanded(row_for_node(folder)), "clicking a folder opens it");
    check(changes.isEmpty() && !rebuilt, "and nothing rebuilds or changes the list while it slides open", changes.join("; "));

    SpaceStore::the().remove_node(folder);
    rebuild_now();
}

// Favorites that change are rebuilt: the old tiles go at once and the new ones show in their places straight away,
// never on top of each other.
void SelfTest::favorites_never_overlap_when_they_change()
{
    s().switch_to_space(m_alpha);
    settle(200);
    auto tiles_overlap = [&] {
        QList<QRect> shown;
        for (auto* tile : m_sidebar.m_favorites->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (!tile->isVisible())
                continue;
            for (auto const& other : shown) {
                if (other.intersects(tile->geometry()))
                    return true;
            }
            shown.append(tile->geometry());
        }
        return false;
    };
    auto tiles_shown = [&] {
        int count = 0;
        for (auto* tile : m_sidebar.m_favorites->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly))
            count += tile->isVisible();
        return count;
    };

    auto before = SpaceStore::the().favorites_for_profile(s().current_profile()).size();
    auto added = SpaceStore::the().add_favorite(page("Overlap"), "Overlap", 0, s().current_profile());
    s().rebuild_favorites();
    check(!tiles_overlap() && tiles_shown() == before + 1, "a new favorite's tiles show at once, side by side",
        QString("%1 shown").arg(tiles_shown()));
    bool ever_overlapped = false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 300) {
        ever_overlapped |= tiles_overlap();
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    check(!ever_overlapped, "and never on top of each other");

    SpaceStore::the().remove_favorite(added);
    s().rebuild_favorites();
    check(!tiles_overlap() && tiles_shown() == before, "removing one leaves the rest side by side", QString("%1 shown").arg(tiles_shown()));
    rebuild_now();
}

// As in Arc: closing the current tab shows the tab shown before it (if it's still open), not its neighbor.
void SelfTest::closing_a_tab_returns_to_the_one_before()
{
    s().switch_to_space(m_alpha);
    settle(200);
    QPointer<Tab> a = s().open_tab(page("Recent A"));
    QPointer<Tab> b = s().open_tab(page("Recent B"));
    QPointer<Tab> c = s().open_tab(page("Recent C"));
    if (!wait_until([&] { return a && b && c && loaded(*a) && loaded(*b) && loaded(*c); })) {
        check(false, "three tabs open");
        return;
    }
    // Shown in the order B, A, C: closing C goes back to A (its neighbor is B).
    s().activate(*b);
    settle(100);
    s().activate(*a);
    settle(100);
    s().activate(*c);
    settle(100);
    auto gone = [&](QPointer<Tab> const& tab) { return !tab || s().m_tabs.index_of(tab.data()) < 0; };
    s().close_tab(*c);
    check(wait_until([&] { return gone(c) && window().current_tab() == a.data(); }), "closing a tab shows the one shown before it",
        window().current_tab() ? window().current_tab()->title() : QString());

    s().close_tab(*a);
    check(wait_until([&] { return gone(a) && window().current_tab() == b.data(); }), "and again, the one before that",
        window().current_tab() ? window().current_tab()->title() : QString());

    // Closing a tab that isn't shown leaves the current one.
    QPointer<Tab> d = s().open_tab(page("Recent D"));
    wait_until([&] { return d && loaded(*d); });
    s().activate(*b);
    settle(100);
    s().close_tab(*d);
    check(wait_until([&] { return gone(d); }) && window().current_tab() == b.data(), "closing another tab keeps the current one");
    check(s().current_space_id() == m_alpha, "and the space");
    if (b)
        s().close_tab(*b);
    settle(300);
}

// The downloads shelf rises out of the bottom bar, its rows one after another, painted (no graphics effect, which
// renders it offscreen and darkens it).
void SelfTest::downloads_shelf_rows_rise_in()
{
    auto* shelf = m_sidebar.m_shelf;
    s().show_shelf();
    if (shelf->is_empty()) {
        check(true, "the downloads shelf has nothing to show (empty Downloads folder)");
        return;
    }
    check(shelf->isVisible() && !shelf->graphicsEffect(), "the downloads shelf opens, painted directly");
    check(shelf->m_rows_elapsed_ms < 50, "its rows start below their places");
    check(wait_until([&] { return shelf->m_rows_elapsed_ms >= shelf_rows_duration_ms(static_cast<int>(shelf->m_items.size())) && shelf->opacity() == 1.0; }, 2000),
        "and rise into them");
    s().hide_shelf();
    check(wait_until([&] { return !shelf->isVisible(); }, 2000), "it closes");
}

// As in Arc: leaving a tab while its video plays (for another tab or space) shows the video in a floating player that
// keeps playing, and coming back puts it back in the page. Not for a muted tab.
void SelfTest::leaving_a_playing_video_shows_it_in_picture_in_picture()
{
    auto port = qEnvironmentVariable("ARC_SELF_TEST_HTTP_PORT");
    if (port.isEmpty()) {
        check(false, "the test pages are served (run-arc-self-test.sh sets ARC_SELF_TEST_HTTP_PORT)");
        return;
    }
    s().switch_to_space(m_alpha);
    settle(200);
    QPointer<Tab> video = s().open_tab(QString("http://localhost:%1/video.html").arg(port));
    QPointer<Tab> other = s().open_tab(page("Not the video"));
    if (!wait_until([&] { return video && other && !video->is_loading() && video->title() == "Video" && loaded(*other); }, 5000)) {
        check(false, "the video page loads");
        return;
    }
    s().activate(*video);
    settle(300);
    // A click on the page starts it (sound needs one).
    {
        auto& view = video->view();
        QPointF point(40, 40);
        QMouseEvent press(QEvent::MouseButtonPress, point, view.mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, view.mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&view, &press);
        QApplication::sendEvent(&view, &release);
    }
    if (!wait_until([&] { return video && video->title() == "Playing" && video->view().audio_play_state() == Web::HTML::AudioPlayState::Playing; }, 5000)) {
        check(false, "the video plays", video ? video->title() : QString());
        return;
    }

    s().activate(*other);
    check(wait_until([&] { return video && video->title() == "In PiP: video" && video->view().has_picture_in_picture_window(); }, 5000),
        "leaving its tab shows the playing video (the big one) in Picture-in-Picture", video ? video->title() : QString());

    s().activate(*video);
    check(wait_until([&] { return video && video->title() == "Playing" && !video->view().has_picture_in_picture_window(); }, 5000),
        "coming back puts it back in the page, still playing", video ? video->title() : QString());

    // Leaving for another space does it too.
    s().switch_to_space(m_beta);
    check(wait_until([&] { return video && video->title() == "In PiP: video"; }, 5000), "so does switching to another space",
        video ? video->title() : QString());
    s().switch_to_space(m_alpha);
    if (video)
        s().activate(*video);
    wait_until([&] { return video && video->title() == "Playing"; }, 5000);

    // A muted tab's video stays put.
    video->view().toggle_page_mute_state();
    settle(200);
    s().activate(*other);
    settle(1000);
    check(video && video->title() == "Playing" && !video->view().has_picture_in_picture_window(), "a muted tab's video doesn't",
        video ? video->title() : QString());

    if (video)
        s().close_tab(*video);
    if (other)
        s().close_tab(*other);
    settle(300);
}

static WebContentView* picture_in_picture_view()
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (widget->windowTitle() == "Picture-in-Picture" && widget->isVisible())
            return widget->findChild<WebContentView*>();
    }
    return nullptr;
}

static void click_view_at(QWidget& view, QPointF point)
{
    QMouseEvent move(QEvent::MouseMove, point, view.mapToGlobal(point), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QMouseEvent press(QEvent::MouseButtonPress, point, view.mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, point, view.mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &move);
    QApplication::sendEvent(&view, &press);
    QApplication::sendEvent(&view, &release);
}

// Clicks one of the Picture-in-Picture window's own controls (drawn by the compositor, where its geometry puts them).
static bool click_picture_in_picture_control(Compositing::PictureInPictureControl control)
{
    auto* view = picture_in_picture_view();
    if (!view)
        return false;
    auto dpr = view->devicePixelRatioF();
    auto geometry = Compositing::picture_in_picture_controls_geometry({ qRound(view->width() * dpr), qRound(view->height() * dpr) }, dpr);
    Gfx::IntRect bounds;
    for (auto const& button : geometry.buttons()) {
        if (button.action == control)
            bounds = button.bounds;
    }
    auto center = QPointF(bounds.center().x() / dpr, bounds.center().y() / dpr);
    // The pointer arriving shows the controls, which take presses only while they show.
    QMouseEvent move(QEvent::MouseMove, center, view->mapToGlobal(center), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view, &move);
    QEventLoop loop;
    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    loop.exec();
    click_view_at(*view, center);
    return true;
}

// As in Arc: Minimize closes the Picture-in-Picture window and leaves the video playing, with a player for it at the
// bottom of the sidebar (play or pause, mute, back to the tab, back into Picture-in-Picture, and close).
void SelfTest::minimizing_picture_in_picture_moves_the_video_to_the_sidebar_player()
{
    auto port = qEnvironmentVariable("ARC_SELF_TEST_HTTP_PORT");
    if (port.isEmpty())
        return; // Reported by the scenario before.
    s().switch_to_space(m_alpha);
    settle(200);
    QPointer<Tab> video = s().open_tab(QString("http://localhost:%1/video.html").arg(port));
    QPointer<Tab> other = s().open_tab(page("Not the video either"));
    if (!wait_until([&] { return video && other && !video->is_loading() && video->title() == "Video" && loaded(*other); }, 5000)) {
        check(false, "the video page loads (player)");
        return;
    }
    s().activate(*video);
    settle(300);
    click_view_at(video->view(), QPointF(40, 40));
    if (!wait_until([&] { return video && video->title() == "Playing"; }, 5000)) {
        check(false, "the video plays (player)", video ? video->title() : QString());
        return;
    }
    auto& player = *s().m_mini_player;
    check(!player.isVisible(), "no player while the playing tab is shown");

    auto leave_into_picture_in_picture = [&] {
        s().activate(*other);
        return wait_until([&] { return video && video->view().has_picture_in_picture_window() && picture_in_picture_view(); }, 5000);
    };
    if (!leave_into_picture_in_picture()) {
        check(false, "leaving the tab opens Picture-in-Picture (player)", video ? video->title() : QString());
        return;
    }
    check(!player.isVisible(), "no player while the video shows in Picture-in-Picture");

    click_picture_in_picture_control(Compositing::PictureInPictureControl::Minimize);
    check(wait_until([&] { return video && !video->view().has_picture_in_picture_window() && player.isVisible(); }, 5000),
        "Minimize closes the window and shows the player in the sidebar");
    check(video && video->title() == "Playing" && player.is_playing(), "the video plays on", video ? video->title() : QString());
    check(player.title() == "Playing" && player.height() == MiniPlayer::CONTROLS_HEIGHT, "the player shows the tab's title, folded",
        QString("%1 %2").arg(player.title()).arg(player.height()));
    auto bottom = player.geometry().bottom();
    player.set_revealed(true);
    settle(MiniPlayer::REVEAL_MS + 150);
    check(player.height() == MiniPlayer::CONTROLS_HEIGHT + MiniPlayer::TITLE_HEIGHT && player.geometry().bottom() == bottom
            && player.close_button()->isVisible(),
        "hovering it slides it up from where it rests, showing its title row", QString::number(player.height()));
    player.set_revealed(false);
    settle(MiniPlayer::REVEAL_MS + 150);
    check(player.height() == MiniPlayer::CONTROLS_HEIGHT && !player.close_button()->isVisible(), "and back down");

    player.play_button()->click();
    check(wait_until([&] { return video && video->title() == "Video" && !player.is_playing(); }, 5000), "its pause button pauses the video",
        video ? video->title() : QString());
    player.play_button()->click();
    check(wait_until([&] { return video && video->title() == "Playing" && player.is_playing(); }, 5000), "and its play button plays it again",
        video ? video->title() : QString());

    player.mute_button()->click();
    check(wait_until([&] { return video && video->view().page_mute_state() == Web::HTML::MuteState::Muted && player.is_muted(); }, 3000),
        "its mute button mutes the tab");
    settle(300);
    check(player.isVisible() && player.is_playing(), "and the muted video still shows as playing");
    player.mute_button()->click();
    check(wait_until([&] { return video && video->view().page_mute_state() == Web::HTML::MuteState::Unmuted && !player.is_muted(); }, 3000),
        "and unmutes it");

    player.picture_in_picture_button()->click();
    check(wait_until([&] { return video && video->view().has_picture_in_picture_window() && !player.isVisible(); }, 5000),
        "its Picture-in-Picture button shows the video there again, in place of the player");
    click_picture_in_picture_control(Compositing::PictureInPictureControl::Close);
    check(wait_until([&] { return video && video->title() == "Video"; }, 5000), "Close in Picture-in-Picture stops the video",
        video ? video->title() : QString());
    settle(600);
    check(!player.isVisible(), "and leaves no player");

    // Back to the tab from the player's icon.
    if (video)
        s().activate(*video);
    settle(300);
    click_view_at(video->view(), QPointF(40, 40));
    wait_until([&] { return video && video->title() == "Playing"; }, 5000);
    if (!leave_into_picture_in_picture()) {
        check(false, "leaving the tab again opens Picture-in-Picture", video ? video->title() : QString());
        return;
    }
    click_picture_in_picture_control(Compositing::PictureInPictureControl::Minimize);
    wait_until([&] { return player.isVisible(); }, 5000);
    player.icon_button()->click();
    check(wait_until([&] { return video && window().current_tab() == video && !player.isVisible(); }, 3000),
        "the player's icon goes back to the tab, and the player goes");

    // Its close button stops the video.
    if (!leave_into_picture_in_picture()) {
        check(false, "leaving the tab a third time opens Picture-in-Picture", video ? video->title() : QString());
        return;
    }
    click_picture_in_picture_control(Compositing::PictureInPictureControl::Minimize);
    wait_until([&] { return player.isVisible(); }, 5000);
    player.close_button()->click();
    check(wait_until([&] { return video && video->title() == "Video" && !player.isVisible(); }, 5000),
        "the player's close button stops the video and closes the player", video ? video->title() : QString());

    if (video)
        s().close_tab(*video);
    if (other)
        s().close_tab(*other);
    settle(300);
}

// As in Arc: the docked sidebar has its own toggle beside the window buttons, in place of the toolbar's.
void SelfTest::the_docked_sidebar_has_its_toggle_beside_the_window_buttons()
{
    if (s().m_mode != ArcSidebar::Mode::Docked) {
        check(false, "the sidebar is docked for the toggle check");
        return;
    }
    auto* toggle = s().m_sidebar_toggle;
    auto lights = s().m_traffic_lights->geometry();
    check(toggle->isVisible() && toggle->geometry().left() > lights.right() && qAbs(toggle->geometry().center().y() - lights.center().y()) <= 2,
        "the docked sidebar's toggle sits right of the window buttons");
    auto toolbar_toggles = 0;
    if (auto* tab = window().current_tab(); tab && tab->toolbar_container()) {
        for (auto* button : tab->toolbar_container()->findChildren<QToolButton*>()) {
            if (button->isVisible() && button->defaultAction() && button->defaultAction()->text() == "Toggle Vertical Tabs Expanded")
                ++toolbar_toggles;
        }
    }
    check(toolbar_toggles == 0, "and the toolbar doesn't show one too", QString::number(toolbar_toggles));
}

static QList<BrowserWindow*> little_arcs()
{
    QList<BrowserWindow*> windows;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = qobject_cast<BrowserWindow*>(widget); window && window->isVisible() && is_little_arc(*window))
            windows.append(window);
    }
    return windows;
}

// Little Arc: a small window for one page, with no sidebar; "Open in" (Cmd+O) moves the page, as it is, into a space.
void SelfTest::little_arc_opens_a_page_and_moves_it_into_a_space()
{
    auto port = qEnvironmentVariable("ARC_SELF_TEST_HTTP_PORT");
    if (port.isEmpty()) {
        check(false, "the test pages are served (run-arc-self-test.sh sets ARC_SELF_TEST_HTTP_PORT)");
        return;
    }
    s().switch_to_space(m_alpha);
    settle(200);
    auto url = QString("http://localhost:%1/landed.html").arg(port);
    QPointer<BrowserWindow> little = &open_little_arc(URL::Parser::basic_parse(StringView(url.toUtf8().constData(), static_cast<size_t>(url.toUtf8().size()))));
    QPointer<Tab> tab = little ? little->current_tab() : nullptr;
    check(wait_until([&] { return tab && tab->title() == "Landed" && !tab->is_loading(); }, 5000), "a Little Arc opens the page");
    check(little && !ArcSidebar::for_window(*little) && little->width() < window().width(), "in a small window with no sidebar");

    // Only its bar and the page: the page fills the window's width, with nothing of the tab column over it.
    if (little && tab) {
        auto& view = tab->view();
        auto left = view.mapTo(little.data(), QPoint(0, 0)).x();
        check(left <= 4 && view.width() >= little->width() - 8, "its page fills the window (inside its frame)",
            QString("page at x %1, %2 wide in %3").arg(left).arg(view.width()).arg(little->width()));
        bool covered = false;
        for (auto* widget : little->findChildren<QWidget*>()) {
            if (!widget->isVisible() || widget == &view || view.isAncestorOf(widget) || widget->isAncestorOf(&view) || widget->objectName() == "ArcLittleArcBar"
                || widget->parentWidget() == nullptr || widget->window() != little.data())
                continue;
            auto bar = little->findChild<QWidget*>("ArcLittleArcBar");
            if (bar && bar->isAncestorOf(widget))
                continue;
            auto rect = QRect(widget->mapTo(little.data(), QPoint(0, 0)), widget->size());
            auto page = QRect(view.mapTo(little.data(), QPoint(0, 0)), view.size());
            if (rect.intersects(page) && rect.width() < page.width()) {
                covered = true;
                check(false, "nothing covers the page", QString("%1 %2").arg(widget->metaObject()->className(), widget->objectName()));
                break;
            }
        }
        if (!covered)
            check(true, "nothing covers the page");
    }

    // Open in (Cmd+O): the page moves into the most recent space as it is (not loaded again), and the Little Arc closes.
    if (little)
        move_little_arc_to_space(*little);
    check(wait_until([&] { return !little || !little->isVisible(); }, 3000), "Open in closes the Little Arc");
    check(tab && s().m_tabs.index_of(tab.data()) >= 0 && s().today_tabs(m_alpha).contains(tab.data()), "and moves its page into the most recent space's Today");
    check(tab && s().current_space_id() == m_alpha && window().current_tab() == tab.data() && tab->title() == "Landed", "showing it, as it was");
    if (tab)
        s().close_tab(*tab);
    settle(200);

    // Into another space (with another profile, where it opens again in that profile's session).
    little = &open_little_arc(URL::Parser::basic_parse(StringView(url.toUtf8().constData(), static_cast<size_t>(url.toUtf8().size()))));
    tab = little ? little->current_tab() : nullptr;
    wait_until([&] { return tab && tab->title() == "Landed" && !tab->is_loading(); }, 5000);
    if (little)
        move_little_arc_to_space(*little, m_beta);
    auto landed_in_beta = [&] {
        for (auto* today : s().today_tabs(m_beta)) {
            if (ArcSidebar::url_of(*today).endsWith("/landed.html"))
                return today;
        }
        return static_cast<Tab*>(nullptr);
    };
    check(wait_until([&] { return (!little || !little->isVisible()) && landed_in_beta() && s().current_space_id() == m_beta && window().current_tab() == landed_in_beta(); }, 5000),
        "or into a chosen space, shown there", [&] {
            QStringList parts;
            parts << QString("little visible %1").arg(little && little->isVisible());
            parts << QString("space %1 (beta %2)").arg(s().current_space_id() == m_beta).arg(m_beta);
            for (auto* today : s().today_tabs(m_beta))
                parts << ArcSidebar::url_of(*today);
            parts << QString("current %1").arg(window().current_tab() ? ArcSidebar::url_of(*window().current_tab()) : QString());
            for (auto* any : s().tabs())
                parts << QString("tab %1 space %2").arg(ArcSidebar::url_of(*any), ArcSidebar::space_of(*any));
            return parts.join(" | ");
        }());
    tab = landed_in_beta();

    // With nothing to open, it asks what to look up.
    QPointer<BrowserWindow> empty = &open_little_arc();
    check(wait_until([&] {
        auto* bar = empty ? empty->findChild<CommandBar*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
        return bar && bar->isVisible();
    }, 2000), "an empty Little Arc opens the command bar");
    if (empty)
        empty->close();

    // A link opened from another app opens in one too.
    auto before = little_arcs().size();
    Application::the().on_open_file(URL::Parser::basic_parse(StringView(url.toUtf8().constData(), static_cast<size_t>(url.toUtf8().size()))).value());
    check(wait_until([&] {
        for (auto* little_arc : little_arcs()) {
            if (little_arc->current_tab() && little_arc->current_tab()->title() == "Landed")
                return true;
        }
        return false;
    }, 5000), "a link from another app opens in a Little Arc", QString("%1 Little Arcs").arg(little_arcs().size()));
    for (auto* window : little_arcs())
        window->close();
    wait_until([&] { return little_arcs().size() == 0; }, 2000);
    before = little_arcs().size();

    // Cmd+Option+N opens one from the browser window.
    auto* target = window().current_tab() ? static_cast<QWidget*>(&window().current_tab()->view()) : &window();
    QKeyEvent override_event(QEvent::ShortcutOverride, Qt::Key_N, Qt::ControlModifier | Qt::AltModifier);
    QApplication::sendEvent(target, &override_event);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_N, Qt::ControlModifier | Qt::AltModifier);
    QApplication::sendEvent(target, &press);
    check(wait_until([&] { return little_arcs().size() == before + 1; }, 2000), "Cmd+Option+N opens a Little Arc");
    for (auto* window : little_arcs())
        window->close();

    if (tab)
        s().close_tab(*tab);
    s().switch_to_space(m_alpha);
    settle(300);
}

void SelfTest::pinned_tab_away_back_and_separate()
{
    auto node = m_alpha_pinned[2];
    s().open_or_activate_node(node);
    auto* tab = s().tab_for_node(node);
    wait_until([&] { return tab && loaded(*tab); });
    auto away = [&] { return row_for_node(node).data(NavigatedAwayRole).toBool(); };
    check(tab && !away(), "a pinned tab on its pinned URL isn't away");

    tab->navigate(ak_url_from_qurl(QUrl(page("Elsewhere"))));
    check(wait_until([&] { return away(); }), "navigating elsewhere marks it away (\"|\")");

    s().back_to_pinned_url(node);
    check(wait_until([&] { return !away() && at(*tab, page("P2")); }), "Back to Pinned URL returns it");

    tab->navigate(ak_url_from_qurl(QUrl(page("Elsewhere 2"))));
    wait_until([&] { return away(); });
    auto today_before = s().today_tabs(m_alpha).size();
    s().separate_from_pinned_tab(node);
    check(wait_until([&] { return s().today_tabs(m_alpha).size() == today_before + 1; }), "Separate keeps the page in a tab of its own");
    auto today = s().today_tabs(m_alpha);
    check(!today.isEmpty() && wait_until([&] { return at(*today.last(), page("Elsewhere 2")); }), "showing the page it was on");
    check(wait_until([&] { return !away() && at(*tab, page("P2")); }), "and returns the pinned tab to its URL");
    for (auto* today_tab : s().today_tabs(m_alpha))
        s().close_tab(*today_tab);
    settle(200);
}

void SelfTest::profiles_get_their_own_session()
{
    auto& default_session = WebView::Application::default_session();
    s().switch_to_space(m_beta);
    settle(200);
    auto& beta_tab = *s().open_tab(page("In Beta"));
    wait_until([&] { return loaded(beta_tab); });
    auto beta_session = session_for_profile("SelfTest");
    check(beta_session && &beta_tab.view().session() == beta_session.ptr(), "a tab opened in a profile's space uses that profile's session");
    check(&beta_tab.view().session() != &default_session, "not the default one");

    s().switch_to_space(m_alpha);
    settle(200);
    auto& alpha_tab = *s().open_tab(page("In Alpha"));
    wait_until([&] { return loaded(alpha_tab); });
    check(&alpha_tab.view().session() == &default_session, "a tab in a default-profile space uses the default session");

    // Moving a tab to a space of another profile reopens it in that profile's session.
    QPointer<Tab> moved = &alpha_tab;
    ArcSidebar::set_tab_space(alpha_tab, m_beta);
    s().schedule_rebuild();
    check(wait_until([&] { return !moved; }), "a tab moved to another profile's space is replaced");
    Tab* reopened = nullptr;
    wait_until([&] {
        for (auto* tab : s().today_tabs(m_beta)) {
            if (at(*tab, page("In Alpha")))
                reopened = tab;
        }
        return reopened != nullptr;
    });
    check(reopened && &reopened->view().session() == beta_session.ptr(), "by the same page in that profile's session",
        reopened ? QString() : QString("Beta has %1 Today tabs").arg(s().today_tabs(m_beta).size()));

    for (auto* tab : s().today_tabs(m_beta))
        s().close_tab(*tab);
    s().switch_to_space(m_alpha);
    settle(300);
}

void SelfTest::list_ends_with_room_to_double_click()
{
    rebuild_now();
    auto* root = s().m_model->invisibleRootItem();
    auto last = root->child(root->rowCount() - 1);
    check(last && row_kind(last->index()) == RowKind::BottomSpace, "the list ends with empty room");
    check(last && s().m_tree->visualRect(last->index()).height() >= 40, "of at least 40 points");

    auto* bar = window().findChild<CommandBar*>(QString(), Qt::FindDirectChildrenOnly);
    if (bar)
        bar->dismiss();
    check(!bar || !bar->isVisible(), "the command bar starts closed");
    emit s().m_tree->background_double_clicked();
    bar = window().findChild<CommandBar*>(QString(), Qt::FindDirectChildrenOnly);
    check(wait_until([&] { return bar && bar->isVisible(); }, 2000), "double-clicking empty sidebar space opens the command bar");
    if (bar)
        bar->dismiss();
}

void SelfTest::update_pill_hidden_without_a_release()
{
    check(UpdateChecker::the().state() == UpdateChecker::State::UpToDate, "a development build has no update");
    check(s().m_update_button && !s().m_update_button->isVisible(), "so the update pill is hidden");
}

void SelfTest::background_tabs_show_while_loading_and_stay_open()
{
    // Regression: tabs still loading counted as blank, so they were hidden from Today and closed as stale.
    s().switch_to_space(m_alpha);
    auto& current = *s().open_tab(page("Foreground"));
    wait_until([&] { return loaded(current); });
    // A slow page: its URL stays blank until it commits.
    auto slow = page("Slow", "<script>let t = Date.now(); while (Date.now() - t < 1500) {}</script>slow");
    s().open_background_tabs({ { slow, m_alpha, {} } });
    Tab* background = nullptr;
    for (auto* tab : s().tabs()) {
        if (tab != &current && ArcSidebar::space_of(*tab) == m_alpha && ArcSidebar::node_of(*tab).isEmpty() && !ArcSidebar::is_blank(*tab) && tab != s().m_spare_tab)
            background = tab;
    }
    QPointer<Tab> guarded = background;
    check(background && s().today_tabs(m_alpha).contains(background), "a background tab shows in Today while it loads");
    // Switching tabs used to close it as a stale blank page.
    s().activate(current);
    settle(400);
    check(guarded && s().today_tabs(m_alpha).contains(guarded.data()), "and switching tabs doesn't close it");
    if (guarded)
        s().close_tab(*guarded);
    s().close_tab(current);
    settle(300);
}

void SelfTest::the_empty_page_becomes_a_tab_once_navigated()
{
    // Close the space's last tab: an empty page shows. Navigating it makes it a tab like any other.
    s().switch_to_space(m_beta);
    settle(200);
    auto& only = *s().open_tab(page("Last"));
    wait_until([&] { return loaded(only); });
    QPointer<Tab> guarded = &only;
    s().close_tab(only);
    wait_until([&] { return !guarded; });
    auto* empty = window().current_tab();
    check(empty && ArcSidebar::is_blank(*empty), "closing the last tab shows the empty page");
    if (!empty)
        return;
    empty->navigate(ak_url_from_qurl(QUrl(page("Typed"))));
    check(wait_until([&] { return s().today_tabs(m_beta).contains(empty); }), "navigating it lists it in Today");
    check(wait_until([&] { return loaded(*empty) && &empty->view().session() == session_for_profile("SelfTest").ptr(); }), "in its space's profile");
    if (auto* tab = s().today_tabs(m_beta).value(0))
        s().close_tab(*tab);
    s().switch_to_space(m_alpha);
    settle(300);
}

void SelfTest::tabs_of_removed_pinned_tabs_and_favorites_move_to_today()
{
    // Regression: a tab whose pinned entry or favorite was removed elsewhere (another window, a re-import)
    // vanished from the sidebar while it kept running.
    s().switch_to_space(m_alpha);
    auto& store = SpaceStore::the();
    auto node = store.add_pinned_tab(m_alpha, {}, -1, page("Doomed"), "Doomed");
    settle(100);
    s().open_or_activate_node(node);
    auto* tab = s().tab_for_node(node);
    wait_until([&] { return tab && loaded(*tab); });
    store.remove_node(node); // As another window or an import would.
    check(wait_until([&] { return tab && s().today_tabs(m_alpha).contains(tab); }), "a removed pinned tab's tab moves to Today");

    auto favorite = store.add_favorite(page("Doomed favorite"), "Doomed favorite");
    settle(100);
    s().open_or_activate_favorite(favorite);
    auto* favorite_tab = s().tab_for_node(QString("fav:%1").arg(favorite));
    wait_until([&] { return favorite_tab && loaded(*favorite_tab); });
    store.remove_favorite(favorite);
    check(wait_until([&] { return favorite_tab && s().today_tabs(s().current_space_id()).contains(favorite_tab); }), "so does a removed favorite's");
    for (auto* today : s().today_tabs(m_alpha))
        s().close_tab(*today);
    settle(300);
}

void SelfTest::a_malformed_pinned_url_doesnt_crash()
{
    auto node = SpaceStore::the().add_pinned_tab(m_alpha, {}, -1, "http://[::1", "Broken");
    settle(100);
    auto tabs_before = tab_count();
    s().open_or_activate_node(node); // Used to abort the browser.
    settle(300);
    check(true, "opening a pinned tab with a malformed URL doesn't crash");
    check(!s().tab_for_node(node) && tab_count() <= tabs_before, "and opens nothing");
    SpaceStore::the().remove_node(node);
    s().on_row_clicked({}); // Nor does clicking nothing.
    check(true, "clicking no row doesn't crash");
}

void SelfTest::popups_close_themselves()
{
    // Regression: popups got a sidebar, and a popup's window.close() left an empty page instead of closing it.
    auto& opener = *s().open_tab(page("Opener"));
    wait_until([&] { return loaded(opener); });
    auto windows_before = browser_windows().size();
    opener.view().run_javascript("window.open('data:text/html,<title>Popup</title>popup', '_blank', 'popup,width=320,height=240');"_string);
    BrowserWindow* popup = nullptr;
    wait_until([&] {
        for (auto* candidate : browser_windows()) {
            if (candidate->is_popup_window())
                popup = candidate;
        }
        return popup != nullptr;
    });
    check(popup, "a page can open a popup window");
    if (!popup)
        return;
    check(!ArcSidebar::for_window(*popup), "popups get no sidebar");
    QPointer<BrowserWindow> guarded = popup;
    if (auto* tab = popup->current_tab()) {
        wait_until([&] { return loaded(*tab); });
        tab->view().run_javascript("window.close();"_string);
    }
    check(wait_until([&] { return !guarded || !guarded->isVisible(); }), "and closing itself closes the popup window");
    check(browser_windows().size() <= windows_before + 0 || !guarded, "leaving the windows as they were");
    s().close_tab(opener);
    settle(300);
}

void SelfTest::private_windows_write_nothing()
{
    // Regression: private windows' tabs, titles and icons were saved to the profile.
    auto secret = page("Private secret");
    auto& private_window = Application::the().new_window({ ak_url_from_qurl(QUrl(secret)) }, Application::the().configuration_for_new_window(),
        BrowserWindow::IsPopupWindow::No, WebView::IsPrivate::Yes);
    auto* private_tab = private_window.current_tab();
    wait_until([&] { return private_tab && loaded(*private_tab); });
    settle(2500); // Past the session save's debounce.
    auto saved = std::any_of(SpaceStore::the().session().begin(), SpaceStore::the().session().end(), [&](auto const& entry) {
        return pinned_urls_match(secret, entry.url);
    });
    check(!saved, "a private window's tabs aren't saved to the profile");
    check(!ArcSidebar::for_window(private_window) || !ArcSidebar::for_window(private_window)->m_spare_tab, "and it keeps no spare tab");
    private_window.close();
    settle(300);
    window().activateWindow();
    settle(300);
}

void SelfTest::address_bar_edits_the_current_tab()
{
    s().switch_to_space(m_alpha);
    auto* opened = s().open_tab(page("Addressed"));
    QPointer<Tab> tab = opened;
    wait_until([&] { return tab && loaded(*tab) && window().current_tab() == tab; });
    if (!tab) {
        check(false, "a tab opens for the address bar");
        return;
    }
    auto bar_of = [](Tab& tab) { return tab.toolbar_container() ? tab.toolbar_container()->findChild<AddressBar*>() : nullptr; };
    auto* bar = bar_of(*tab);
    auto* edit = tab->toolbar_container() ? tab->toolbar_container()->findChild<LocationEdit*>() : nullptr;
    check(bar && bar->isVisible(), "each tab's toolbar shows Arc's address bar");
    check(edit && !edit->isVisible(), "in place of upstream's location field");
    if (!bar)
        return;
    check(wait_until([&] { return QUrl(bar->url()) == QUrl(ArcSidebar::url_of(*tab)); }), "showing the page's URL", bar->url().toString());
    check(qAbs(bar->mapTo(tab->toolbar_container(), bar->pill_rect().center()).x() - tab->toolbar_container()->width() / 2.0) < 120,
        "centered in the toolbar");

    // Navigating updates it.
    tab->navigate(ak_url_from_qurl(QUrl(page("Addressed 2"))));
    check(wait_until([&] { return at(*tab, page("Addressed 2")) && bar->url() == QUrl(ArcSidebar::url_of(*tab)); }), "it follows navigation");

    // Clicking the URL edits it in the command bar: the full URL, selected, with the tab to stay on.
    auto tabs_before = tab_count();
    emit bar->edit_requested();
    auto* command_bar = window().findChild<CommandBar*>(QString(), Qt::FindDirectChildrenOnly);
    check(wait_until([&] { return command_bar = window().findChild<CommandBar*>(QString(), Qt::FindDirectChildrenOnly), command_bar && command_bar->isVisible(); }, 2000),
        "clicking the URL opens the command bar");
    if (!command_bar)
        return;
    check(command_bar->mode() == CommandBar::Mode::EditCurrentTab, "to edit the current tab");
    check(QUrl(command_bar->text()) == QUrl(ArcSidebar::url_of(*tab)) && command_bar->text_is_all_selected(), "with its whole URL, selected", command_bar->text());
    check(command_bar->rows().size() == 1 && command_bar->rows()[0].kind == CommandBarRow::Kind::CurrentTab && command_bar->selected_row() == 0,
        "and the tab itself as the only row, selected");

    // Return on it just closes the bar.
    auto* field = command_bar->findChild<QLineEdit*>();
    auto press = [&](int key) {
        QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(field, &down);
    };
    press(Qt::Key_Return);
    check(wait_until([&] { return !command_bar->isVisible(); }, 2000), "Return on the tab closes the bar");
    check(tab_count() == tabs_before && window().current_tab() == tab, "staying on the tab, with no new tab");

    // Cmd+L (and upstream's other routes into the location field) open the same.
    tab->focus_location_editor();
    check(wait_until([&] { return command_bar->isVisible() && command_bar->mode() == CommandBar::Mode::EditCurrentTab; }, 2000),
        "Cmd+L opens the bar to edit the current tab");
    tab->focus_location_editor(); // Repeated focus requests don't close it.
    settle(50);
    check(command_bar->isVisible(), "and asking again keeps it open");

    // Entering something else loads it in this tab, not a new one.
    auto target = page("Addressed 3");
    field->setText(target);
    emit field->textEdited(target);
    check(command_bar->rows().isEmpty() || command_bar->rows()[0].kind != CommandBarRow::Kind::CurrentTab, "typing replaces the tab row");
    press(Qt::Key_Return);
    check(wait_until([&] { return tab && at(*tab, target); }), "entering a URL loads it in the same tab");
    check(tab_count() == tabs_before, "without opening a new tab", QString("%1 -> %2").arg(tabs_before).arg(tab_count()));

    // Escape closes it without going anywhere.
    tab->focus_location_editor();
    wait_until([&] { return command_bar->isVisible(); }, 2000);
    press(Qt::Key_Escape);
    check(wait_until([&] { return !command_bar->isVisible(); }, 2000) && at(*tab, target), "Escape closes it and stays put");

    // Cmd+T is still a new tab: empty, with no tab row for the current one.
    CommandBar::toggle_for(window());
    check(command_bar->isVisible() && command_bar->mode() == CommandBar::Mode::NewTab && command_bar->text().isEmpty(), "Cmd+T still opens an empty bar for a new tab");
    command_bar->dismiss();

    if (tab)
        s().close_tab(*tab);
    settle(200);
}

void SelfTest::arc_shortcuts_step_through_the_sidebar()
{
    s().switch_to_space(m_alpha);
    s().clear_today();
    wait_until([&] { return s().today_tabs(m_alpha).isEmpty(); });
    QPointer<Tab> first = s().open_tab(page("Step 1"));
    QPointer<Tab> second = s().open_tab(page("Step 2"));
    if (!wait_until([&] { return first && second && loaded(*first) && loaded(*second); })) {
        check(false, "two Today tabs open for the shortcuts");
        return;
    }
    rebuild_now();

    // Cmd+Option+Up/Down and Cmd+Shift+[/] go up and down the sidebar, not upstream's tab order.
    s().activate(*first);
    auto items = s().sidebar_items();
    auto first_at = s().current_sidebar_item(items);
    check(first_at >= 0 && first_at + 1 < items.size() && items[first_at + 1].tab == second, "the sidebar lists Today's tabs in order");
    check(press_shortcut(Qt::Key_Down, Qt::ControlModifier | Qt::AltModifier | Qt::KeypadModifier), "Cmd+Option+Down is Arc's, not upstream's");
    check(wait_until([&] { return window().current_tab() == second; }, 2000), "Cmd+Option+Down goes to the next tab in the sidebar");
    press_shortcut(Qt::Key_BraceLeft, Qt::ControlModifier | Qt::ShiftModifier);
    check(wait_until([&] { return window().current_tab() == first; }, 2000), "Cmd+Shift+[ goes back up");
    check(press_shortcut(Qt::Key_BracketRight, Qt::ControlModifier | Qt::ShiftModifier), "Cmd+Shift+] takes over upstream's next tab");
    check(wait_until([&] { return window().current_tab() == second; }, 2000), "and goes down the sidebar too");

    // From the last item, down wraps to the first: the first favorite.
    press_shortcut(Qt::Key_Down, Qt::ControlModifier | Qt::AltModifier);
    check(wait_until([&] { auto* tab = window().current_tab(); return tab && ArcSidebar::node_of(*tab).startsWith("fav:"); }, 4000),
        "down from the last tab wraps to the first favorite");

    // Cmd+1 is the first item, Cmd+9 the last.
    press_shortcut(Qt::Key_9, Qt::ControlModifier);
    check(wait_until([&] { return window().current_tab() == second; }, 2000), "Cmd+9 goes to the last tab");
    press_shortcut(Qt::Key_2, Qt::ControlModifier);
    check(wait_until([&] { auto* tab = window().current_tab(); return tab && ArcSidebar::node_of(*tab) == "fav:" + m_sidebar.m_favorite_ids.value(1); }, 4000),
        "Cmd+2 goes to the second item (the second favorite)");

    // Cmd+D pins and unpins.
    s().activate(*first);
    check(press_shortcut(Qt::Key_D, Qt::ControlModifier), "Cmd+D is Arc's pin, not upstream's bookmark");
    check(wait_until([&] { return first && !ArcSidebar::node_of(*first).isEmpty(); }, 2000), "Cmd+D pins a Today tab");
    auto pinned_node = first ? ArcSidebar::node_of(*first) : QString();
    check(SpaceStore::the().node(pinned_node) != nullptr, "into the space's pinned tabs");
    press_shortcut(Qt::Key_D, Qt::ControlModifier);
    check(wait_until([&] { return first && ArcSidebar::node_of(*first).isEmpty(); }, 2000) && !SpaceStore::the().node(pinned_node),
        "and again unpins it, back to Today");

    // Cmd+Option+Left/Right step through the spaces, without wrapping.
    auto const& spaces = SpaceStore::the().spaces();
    auto alpha_index = -1;
    for (int i = 0; i < spaces.size(); ++i) {
        if (spaces[i].id == m_alpha)
            alpha_index = i;
    }
    press_shortcut(Qt::Key_Right, Qt::ControlModifier | Qt::AltModifier);
    auto expected_next = alpha_index + 1 < spaces.size() ? spaces[alpha_index + 1].id : m_alpha;
    check(wait_until([&] { return s().current_space_id() == expected_next; }, 2000), "Cmd+Option+Right goes to the next space");
    press_shortcut(Qt::Key_Left, Qt::ControlModifier | Qt::AltModifier);
    check(wait_until([&] { return s().current_space_id() == m_alpha; }, 2000), "Cmd+Option+Left comes back");
    s().switch_to_space(spaces.first().id);
    press_shortcut(Qt::Key_Left, Qt::ControlModifier | Qt::AltModifier);
    settle(100);
    check(s().current_space_id() == spaces.first().id, "and the first space has nothing to its left");
    s().switch_to_space(m_alpha);
    settle(100);

    // Cmd+S hides and shows the sidebar.
    auto expanded = WebView::Application::settings().tab_settings().vertical_tabs_expanded;
    press_shortcut(Qt::Key_S, Qt::ControlModifier);
    check(WebView::Application::settings().tab_settings().vertical_tabs_expanded != expanded, "Cmd+S toggles the sidebar");
    settle(600);
    press_shortcut(Qt::Key_S, Qt::ControlModifier);
    check(WebView::Application::settings().tab_settings().vertical_tabs_expanded == expanded, "and again brings it back");
    settle(600);

    // The toast that confirms copies (not run here: the self-test runs on a real desktop, and leaves the clipboard be).
    auto* toast = Toast::show_in(window(), "Copied URL", "link");
    check(toast && toast->isVisible() && toast->text() == "Copied URL", "a toast shows over the page");
    if (toast && window().current_tab()) {
        auto& view = window().current_tab()->view();
        auto page_top_right = view.mapToGlobal(QPoint(view.width(), 0));
        check(qAbs(toast->geometry().right() - page_top_right.x()) < 40 && qAbs(toast->geometry().top() - page_top_right.y()) < 30,
            "at the page's top right");
    }
    QPointer<Toast> guarded_toast = toast;
    check(wait_until([&] { return !guarded_toast; }, Toast::VISIBLE_MS + Toast::FADE_MS + 1500), "and fades away by itself");

    // Cmd+Shift+K clears Today.
    press_shortcut(Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier);
    check(wait_until([&] { return s().today_tabs(m_alpha).isEmpty(); }), "Cmd+Shift+K clears Today");
    settle(200);
}

void SelfTest::swiping_the_sidebar_switches_spaces()
{
    auto const& spaces = SpaceStore::the().spaces();
    s().switch_to_space(spaces.first().id);
    settle(100);
    auto start = s().current_space_id();
    auto next = spaces.size() > 1 ? spaces[1].id : start;
    auto* viewport = m_sidebar.m_tree->viewport();
    auto swipe = [&](int dx, int dy, bool natural) {
        auto send = [&](Qt::ScrollPhase phase, QPoint delta) {
            QPointF position(viewport->width() / 2.0, 20);
            QWheelEvent event(position, viewport->mapToGlobal(position), delta, delta * 4, Qt::NoButton, Qt::NoModifier, phase, natural);
            QApplication::sendEvent(viewport, &event);
            return event.isAccepted();
        };
        send(Qt::ScrollBegin, {});
        for (int i = 0; i < 4; ++i)
            send(Qt::ScrollUpdate, QPoint(dx, dy));
        send(Qt::ScrollEnd, {});
        settle(100);
    };

    // Fingers moving left (natural scrolling: the deltas follow the fingers) bring in the next space.
    swipe(-20, 0, true);
    check(s().current_space_id() == next, "swiping left across the sidebar goes to the next space");
    swipe(20, 0, true);
    check(s().current_space_id() == start, "swiping right comes back");
    swipe(-20, 0, false); // Without natural scrolling, the deltas run against the fingers.
    check(s().current_space_id() == start, "with natural scrolling off, the same deltas are a swipe the other way");
    swipe(0, -20, true);
    check(s().current_space_id() == start, "scrolling up and down doesn't switch spaces");
    s().switch_to_space(m_alpha);
    settle(100);
}

void SelfTest::pinned_tabs_rename_in_place()
{
    s().switch_to_space(m_alpha);
    rebuild_now();
    auto node_id = m_alpha_pinned[2];
    auto title_before = SpaceStore::the().node(node_id) ? SpaceStore::the().node(node_id)->title : QString();
    auto index = row_for_node(node_id);
    check(index.isValid(), "a pinned tab to rename");
    if (!index.isValid())
        return;
    emit m_sidebar.m_tree->pinned_row_double_clicked(index);
    auto* editor = m_sidebar.m_rename_editor.data();
    check(editor && editor->isVisible(), "double-clicking a pinned tab edits its name in the row");
    if (!editor)
        return;
    auto row = m_sidebar.m_tree->visualRect(row_for_node(node_id));
    check(row.contains(editor->geometry().center()), "over the row itself");
    check(editor->selectedText() == editor->text() && !editor->text().isEmpty(), "with its name selected", editor->text());

    editor->insert("Renamed in place");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &enter);
    check(wait_until([&] { auto const* node = SpaceStore::the().node(node_id); return node && node->title == "Renamed in place"; }, 2000),
        "Return saves the new name");
    check(!m_sidebar.m_rename_editor, "and closes the field");

    // Escape leaves the name as it was.
    s().begin_inline_rename(node_id);
    editor = m_sidebar.m_rename_editor.data();
    if (editor) {
        editor->insert("Not this");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(editor, &escape);
    }
    settle(50);
    auto const* node = SpaceStore::the().node(node_id);
    check(editor && node && node->title == "Renamed in place" && !m_sidebar.m_rename_editor, "Escape cancels");

    // Rebuilding the list (which happens all the time) keeps the field over its row.
    s().begin_inline_rename(node_id);
    rebuild_now();
    settle(50);
    editor = m_sidebar.m_rename_editor.data();
    check(editor && editor->isVisible() && m_sidebar.m_tree->visualRect(row_for_node(node_id)).contains(editor->geometry().center()),
        "the field stays on its row when the list is rebuilt");
    s().finish_inline_rename(false);
    SpaceStore::the().rename_node(node_id, title_before);
    rebuild_now();
}

void SelfTest::switching_spaces_slides()
{
    auto* panel = m_sidebar.m_panel;
    auto running = [&] { return SpaceSlide::running_in(*panel); };
    auto settled = [&] { return !running() && !m_sidebar.m_holding_tint && (!m_sidebar.m_tint_fade || m_sidebar.m_tint_fade->state() != QAbstractAnimation::Running); };
    auto index_of = [](QString const& id) {
        auto const& spaces = SpaceStore::the().spaces();
        for (int i = 0; i < spaces.size(); ++i) {
            if (spaces[i].id == id)
                return i;
        }
        return -1;
    };

    s().switch_to_space(m_alpha);
    wait_until(settled, 2000);
    auto alpha_background = ChromeStyle::chrome_background(window().palette());

    s().switch_to_space(m_beta);
    auto* slide = running();
    check(slide, "switching spaces slides the sidebar across");
    check(slide && slide->direction() == (index_of(m_beta) > index_of(m_alpha) ? 1 : -1), "in from the side the new space is on");
    check(ChromeStyle::chrome_background(window().palette()) == alpha_background, "the window keeps the old space's color while it slides");
    check(m_sidebar.m_scrollbar_opacity == 0, "and the list's scroll bar hides");
    auto area = s().space_slide_area();
    auto picture = SpaceSlide::picture(*panel, area).toImage();
    check(!picture.isNull() && picture.pixelColor(2, picture.height() - 2).alpha() == 255 && picture.pixelColor(picture.width() - 2, 2).alpha() == 255,
        "its pictures are opaque, with the window's background in them");

    check(wait_until([&] { return !running(); }, 2000), "the slide ends");
    check(wait_until(settled, 2000), "then the window's color fades");
    check(ChromeStyle::chrome_background(window().palette()) != alpha_background, "to the new space's");
    check(wait_until([&] { return m_sidebar.m_scrollbar_opacity > 0; }, 1000), "and the scroll bar flashes in");

    // Quick switches: one slide at a time, ending where the last one went.
    s().switch_to_space(m_alpha);
    s().switch_to_space(m_beta);
    int slides = 0;
    for (auto* child : panel->findChildren<SpaceSlide*>(QString(), Qt::FindDirectChildrenOnly))
        slides += child->is_running() ? 1 : 0;
    check(slides == 1, "switching again mid-slide ends the first slide", QString::number(slides));
    check(wait_until(settled, 3000) && s().current_space_id() == m_beta, "and lands in the last space");

    s().switch_to_space(m_alpha);
    wait_until(settled, 2000);
}

// The core of switching spaces, step by step: the slide covers the favorites and list and moves steadily one way;
// the window holds the old color until it ends, then fades through the colors between the two, never darker.
void SelfTest::the_slide_and_color_fade_run_smoothly()
{
    auto* panel = m_sidebar.m_panel;
    auto fading = [&] { return m_sidebar.m_tint_fade && m_sidebar.m_tint_fade->state() == QAbstractAnimation::Running; };
    auto settled = [&] { return !SpaceSlide::running_in(*panel) && !m_sidebar.m_holding_tint && !fading(); };

    s().switch_to_space(m_alpha);
    wait_until(settled, 2000);
    auto from = ChromeStyle::chrome_background(window().palette());

    s().switch_to_space(m_beta);
    QPointer<SpaceSlide> slide = SpaceSlide::running_in(*panel);
    if (!slide) {
        check(false, "a slide runs to watch");
        return;
    }
    check(slide->geometry() == s().space_slide_area(), "the slide covers the favorites and the list, edge to edge");
    auto direction = slide->direction();

    QList<qreal> progress;
    bool color_held = true;
    QElapsedTimer timer;
    timer.start();
    while (slide && slide->is_running() && timer.elapsed() < 2000) {
        progress.append(slide->progress());
        color_held = color_held && ChromeStyle::chrome_background(window().palette()) == from;
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(4);
    }
    auto steady = progress.size() > 3;
    for (int i = 1; i < progress.size(); ++i)
        steady = steady && progress[i] >= progress[i - 1];
    QSet<qreal> distinct(progress.begin(), progress.end());
    check(steady && distinct.size() >= 4, "the slide moves steadily one way, over several frames",
        QString("%1 samples, %2 distinct").arg(progress.size()).arg(distinct.size()));
    check(timer.elapsed() >= SpaceSlide::DURATION_MS / 2 && timer.elapsed() < SpaceSlide::DURATION_MS + 1000, "and takes about as long as it should",
        QString("%1 ms").arg(timer.elapsed()));
    check(color_held, "the old color holds while it slides");
    Q_UNUSED(direction);

    // The fade.
    wait_until([&] { return fading() || settled(); }, 500);
    QList<QColor> steps;
    timer.restart();
    while (fading() && timer.elapsed() < 2000) {
        auto color = ChromeStyle::chrome_background(window().palette());
        if (steps.isEmpty() || steps.last() != color)
            steps.append(color);
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(4);
    }
    auto to = ChromeStyle::chrome_background(window().palette());
    check(from != to, "the two spaces have different colors to fade between");
    check(steps.size() >= 4, "the window fades through the colors in between", QString("%1 steps").arg(steps.size()));

    auto distance = [](QColor const& a, QColor const& b) {
        return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
    };
    bool toward = true;
    bool between = true;
    for (int i = 0; i < steps.size(); ++i) {
        if (i > 0)
            toward = toward && distance(steps[i], to) <= distance(steps[i - 1], to) + 3;
        auto within = [](int value, int a, int b) { return value >= qMin(a, b) - 3 && value <= qMax(a, b) + 3; };
        between = between && within(steps[i].red(), from.red(), to.red()) && within(steps[i].green(), from.green(), to.green())
            && within(steps[i].blue(), from.blue(), to.blue());
    }
    check(toward, "each step is closer to the new color");
    check(between, "and every step lies between the two (nothing darker or off)");
    check(steps.isEmpty() || distance(steps.first(), from) < distance(steps.first(), to), "it starts from the old color");
    check(to == ChromeStyle::chrome_background(window().palette()) && !fading(), "and ends exactly on the new one");

    s().switch_to_space(m_alpha);
    wait_until(settled, 2000);
}

// Clicking a space's button only changes which one is lit: the buttons aren't rebuilt (rebuilt ones briefly showed
// at other places, lighting up whichever was under the pointer).
void SelfTest::switching_spaces_keeps_the_switcher_in_place()
{
    auto* panel = m_sidebar.m_panel;
    auto settled = [&] { return !SpaceSlide::running_in(*panel) && !m_sidebar.m_holding_tint && (!m_sidebar.m_tint_fade || m_sidebar.m_tint_fade->state() != QAbstractAnimation::Running); };
    s().switch_to_space(m_alpha);
    wait_until(settled, 2000);

    auto buttons = [&] {
        QList<QPair<SymbolButton*, QRect>> list;
        for (auto const& button : m_sidebar.m_space_buttons)
            list.append({ button.data(), button ? button->geometry() : QRect() });
        return list;
    };
    auto before = buttons();
    auto const& spaces = SpaceStore::the().spaces();
    int beta_at = -1;
    for (int i = 0; i < spaces.size(); ++i) {
        if (spaces[i].id == m_beta)
            beta_at = i;
    }
    if (beta_at < 0 || beta_at >= before.size() || !before[beta_at].first) {
        check(false, "the switcher has a button for each space");
        return;
    }

    before[beta_at].first->click();
    auto right_after = buttons();
    settle(50);
    check(s().current_space_id() == m_beta, "clicking a space's button switches to it");
    check(right_after == before && buttons() == before, "and every button stays where it was, the same button");
    bool lit = true;
    for (int i = 0; i < before.size(); ++i)
        lit = lit && before[i].first && before[i].first->is_dimmed() == (spaces[i].id != m_beta);
    check(lit, "only the new space's button is lit");
    check(wait_until(settled, 2000), "the switch settles");

    // Adding a space does rebuild them: each new one shows at once in its place, not later somewhere else.
    auto id = SpaceStore::the().add_space("Switcher Test", "S", QColor(40, 160, 90));
    s().rebuild();
    bool placed = !m_sidebar.m_space_buttons.isEmpty();
    QRect previous;
    for (auto const& button : m_sidebar.m_space_buttons) {
        placed = placed && button && button->isVisible() && m_sidebar.m_switcher->rect().contains(button->geometry())
            && (previous.isNull() || button->geometry().left() >= previous.right());
        if (button)
            previous = button->geometry();
    }
    check(placed, "rebuilt buttons show at once, in order, inside the bar");
    SpaceStore::the().remove_space(id);
    s().switch_to_space(m_alpha);
    wait_until(settled, 2000);
}

// The window's chrome (everything but the page) as Qt paints it, over a color nothing paints, so what's left of it
// shows where only the native window background would.
static QColor const UNPAINTED(255, 0, 255);

static QImage render_window(BrowserWindow& window)
{
    auto dpr = window.devicePixelRatioF();
    QPixmap pixmap(window.size() * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(UNPAINTED);
    window.render(&pixmap, QPoint(), QRegion(window.rect()), QWidget::DrawWindowBackground | QWidget::DrawChildren);
    return pixmap.toImage();
}

// A tile's color, if it's (nearly) all one color: that's background, not a glyph or an edge.
static Optional<QRgb> solid_color(QImage const& image, QRect tile)
{
    QHash<QRgb, int> counts;
    for (int y = tile.top(); y <= tile.bottom(); ++y) {
        for (int x = tile.left(); x <= tile.right(); ++x)
            ++counts[image.pixel(x, y)];
    }
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() * 100 >= tile.width() * tile.height() * 97)
            return it.key();
    }
    return {};
}

// Mechanically: render the window under two very different tints, and every solid patch of chrome must change
// between them (else it paints a color of its own, or nothing at all).
//
// `fade_step` tints the way each step of the fade between spaces does (restyling only what's on screen), so the
// whole window fades, not just part of it.
SelfTest::ChromeTiles SelfTest::chrome_tiles_ignoring_the_tint(bool fade_step, Function<void()> after_tint)
{
    auto& win = window();
    settle(300);
    s().apply_tint(QColor(255, 0, 0), 0.5, fade_step);
    if (after_tint)
        after_tint();
    settle(100);
    auto red = render_window(win);
    s().apply_tint(QColor(0, 0, 255), 0.5, fade_step);
    if (after_tint)
        after_tint();
    settle(100);
    auto blue = render_window(win);
    s().apply_space_tint();
    s().apply_tint(s().space_color(), s().space_tint_amount());
    settle(100);

    auto dpr = win.devicePixelRatioF();
    QRect page;
    if (auto* tab = win.current_tab())
        page = QRect(tab->view().mapTo(&win, QPoint(0, 0)) * dpr, tab->view().size() * dpr);

    // Background is what's solid over more than an icon's size: a tile counts when its neighbors are the same color.
    static constexpr int TILE = 8; // Device pixels.
    int columns = red.width() / TILE;
    int rows = red.height() / TILE;
    auto solid_map = [&](QImage const& image) {
        QVector<Optional<QRgb>> map(columns * rows);
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column) {
                QRect tile(column * TILE, row * TILE, TILE, TILE);
                if (!page.intersects(tile))
                    map[row * columns + column] = solid_color(image, tile);
            }
        }
        return map;
    };
    auto red_tiles = solid_map(red);
    auto blue_tiles = solid_map(blue);
    auto in_a_patch = [&](QVector<Optional<QRgb>> const& map, int row, int column) {
        auto color = map[row * columns + column];
        if (!color.has_value() || row == 0 || column == 0 || row == rows - 1 || column == columns - 1)
            return false;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (map[(row + dy) * columns + column + dx] != color)
                    return false;
            }
        }
        return true;
    };

    QMap<QString, int> unpainted;
    QMap<QString, int> untinted;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            if (!in_a_patch(red_tiles, row, column) || !in_a_patch(blue_tiles, row, column))
                continue;
            auto a = *red_tiles[row * columns + column];
            auto b = *blue_tiles[row * columns + column];
            auto* widget = win.childAt(QPoint(column * TILE + TILE / 2, row * TILE + TILE / 2) / dpr);
            auto name = widget ? QString("%1 %2").arg(widget->metaObject()->className(), widget->objectName()) : QString("(window)");
            if (QColor(a) == UNPAINTED || QColor(b) == UNPAINTED)
                ++unpainted[name];
            else if (a == b)
                ++untinted[name];
        }
    }
    return { unpainted, untinted };
}

void SelfTest::every_chrome_surface_takes_the_space_color()
{
    auto describe = [](QMap<QString, int> const& tiles) {
        QStringList parts;
        for (auto it = tiles.constBegin(); it != tiles.constEnd(); ++it)
            parts.append(QString("%1: %2").arg(it.key()).arg(it.value()));
        return parts.join("; ");
    };
    auto [unpainted, untinted] = chrome_tiles_ignoring_the_tint(false);
    check(unpainted.isEmpty(), "every part of the chrome is painted (none leaves the untinted window background showing)", describe(unpainted));
    check(untinted.isEmpty(), "and none paints a color that ignores the space's", describe(untinted));

    auto [unpainted_in_fade, untinted_in_fade] = chrome_tiles_ignoring_the_tint(true);
    check(unpainted_in_fade.isEmpty() && untinted_in_fade.isEmpty(), "each step of the fade between spaces recolors all of it too",
        describe(unpainted_in_fade) + " " + describe(untinted_in_fade));

    // Only the tab shown restyles when the color changes; another takes the color as it's shown.
    QPointer<Tab> first = s().open_tab(page("Tint first"));
    QPointer<Tab> second = s().open_tab(page("Tint second"));
    wait_until([&] { return first && second && loaded(*first) && loaded(*second); });
    auto [unpainted_after_switch, untinted_after_switch] = chrome_tiles_ignoring_the_tint(false, [&] {
        if (first && second) {
            s().activate(*first);
            s().activate(*second);
        }
    });
    check(unpainted_after_switch.isEmpty() && untinted_after_switch.isEmpty(), "a tab shown after the color changes takes it",
        describe(unpainted_after_switch) + " " + describe(untinted_after_switch));
    for (auto const& tab : { first, second }) {
        if (tab)
            s().close_tab(*tab);
    }
    settle(200);
}

void SelfTest::duplicate_and_close_tabs_below()
{
    s().switch_to_space(m_alpha);
    QPointer<Tab> a = s().open_tab(page("Dup A"));
    QPointer<Tab> b = s().open_tab(page("Dup B"));
    if (!wait_until([&] { return a && b && loaded(*a) && loaded(*b); })) {
        check(false, "two tabs open to duplicate");
        return;
    }
    s().duplicate_tab(*a);
    check(wait_until([&] { return s().today_tabs(m_alpha).size() == 3; }), "Duplicate Tab opens a copy");
    auto today = s().today_tabs(m_alpha);
    auto a_at = today.indexOf(a.data());
    QPointer<Tab> copy = a_at >= 0 && a_at + 1 < today.size() ? today[a_at + 1] : nullptr;
    check(copy && copy != b, "just below the original");
    check(copy && wait_until([&] { return copy && at(*copy, page("Dup A")); }), "at the same page");

    s().close_tabs_below(*a);
    check(wait_until([&] { return s().today_tabs(m_alpha).size() == 1; }), "Close Tabs Below closes the tabs under it");
    check(a && s().today_tabs(m_alpha).value(0) == a, "and keeps it");
    if (a)
        s().close_tab(*a);
    settle(200);
}

// Fill from 1Password, against a stand-in `op` (fake-op.sh) and login pages served over http on this computer.
void SelfTest::fills_from_one_password()
{
    auto port = qEnvironmentVariable("ARC_SELF_TEST_HTTP_PORT");
    if (port.isEmpty()) {
        check(false, "the login pages are served (run-arc-self-test.sh sets ARC_SELF_TEST_HTTP_PORT)");
        return;
    }
    auto toast_text = [&] {
        auto* toast = Toast::current_in(window());
        return toast ? toast->text() : QString();
    };
    auto open = [&](QString const& url, QString const& title) -> QPointer<Tab> {
        QPointer<Tab> tab = s().open_tab(url);
        wait_until([&] { return tab && !tab->is_loading() && tab->title() == title; }, 5000);
        if (tab)
            s().activate(*tab);
        settle(100);
        return tab;
    };
    auto title_becomes = [&](QPointer<Tab> const& tab, QString const& title) {
        return wait_until([&] { return tab && tab->title() == title; }, 5000);
    };

    // One login for the site: it fills straight away, the username into the sign-in form's field (not the search
    // field in the form before it) and the password into the current-password field, as typing would.
    auto solo = open(QString("http://localhost:%1/login.html").arg(port), "Login");
    s().fill_from_one_password();
    check(title_becomes(solo, "solo@example.com|pa\"ss'1"), "one login for the site fills the sign-in form", solo ? solo->title() : QString());
    check(wait_until([&] { return toast_text() == "Filled from 1Password"; }, 2000), "and says so", toast_text());

    // A sign-in page that asks for the email first gets just the username.
    auto first_step = open(QString("http://localhost:%1/username-first.html").arg(port), "Username first");
    s().fill_from_one_password();
    check(title_becomes(first_step, "solo@example.com|"), "a page asking for the email first gets the username", first_step ? first_step->title() : QString());

    // Several logins: a menu to choose from, best matches first.
    auto several = open(QString("http://127.0.0.1:%1/login.html").arg(port), "Login");
    s().fill_from_one_password();
    QPointer<QMenu> menu;
    wait_until([&] {
        menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        return menu && menu->objectName() == "ArcOnePasswordMenu";
    }, 5000);
    QStringList choices;
    QAction* second = nullptr;
    if (menu) {
        for (auto* action : menu->actions()) {
            if (action->isSeparator())
                continue;
            choices.append(action->text());
            if (action->text().startsWith("Second"))
                second = action;
        }
    }
    check(choices.size() == 2 && choices[0].startsWith("First") && second, "several logins for the site open a menu of them",
        choices.join(", "));
    if (second) {
        second->trigger();
        menu->close();
    }
    check(title_becomes(several, "second@example.com|two"), "and the chosen one fills", several ? several->title() : QString());

    // The page moved on (to another origin) before the login arrived: nothing is filled.
    auto moved = open(QString("http://localhost:%1/login.html").arg(port), "Login");
    if (moved)
        s().fill_one_password_login("solo", moved, "http://example.com");
    check(wait_until([&] { return toast_text() == "The page changed, so nothing was filled"; }, 3000), "a page that moved on isn't filled", toast_text());
    settle(200);
    check(moved && moved->title() == "Login", "and keeps its fields empty");

    // Not over http(s): nothing to ask 1Password.
    auto insecure = open(page("Plain", "<input type=password>"), "Plain");
    s().fill_from_one_password();
    check(wait_until([&] { return toast_text() == "1Password fills only secure (https) pages"; }, 2000), "insecure pages aren't filled", toast_text());

    // 1Password's errors come back as a toast.
    if (solo)
        s().activate(*solo);
    qputenv("FAKE_OP_MODE", "dismissed");
    s().fill_from_one_password();
    check(wait_until([&] { return toast_text() == "1Password: cancelled"; }, 3000), "a dismissed Touch ID prompt says it was cancelled", toast_text());
    qunsetenv("FAKE_OP_MODE");

    for (auto const& tab : { solo, first_step, several, moved, insecure }) {
        if (tab)
            s().close_tab(*tab);
    }
    settle(300);
}

int SelfTest::run()
{
    seed();
    toolbar_stays_aligned();
    unloaded_pinned_tab_opens_in_the_spare();
    loading_pinned_row_keeps_its_saved_title();
    closing_a_spaces_last_tab_keeps_the_window();
    clear_closes_only_this_spaces_today_tabs();
    list_keeps_its_scroll_position();
    each_space_keeps_its_scroll_position();
    favorites_update_in_place();
    pinned_tab_away_back_and_separate();
    a_pinned_tab_that_redirects_isnt_away();
    opening_a_folder_leaves_the_list_alone_while_it_slides();
    favorites_never_overlap_when_they_change();
    closing_a_tab_returns_to_the_one_before();
    downloads_shelf_rows_rise_in();
    leaving_a_playing_video_shows_it_in_picture_in_picture();
    minimizing_picture_in_picture_moves_the_video_to_the_sidebar_player();
    the_docked_sidebar_has_its_toggle_beside_the_window_buttons();
    little_arc_opens_a_page_and_moves_it_into_a_space();
    profiles_get_their_own_session();
    list_ends_with_room_to_double_click();
    update_pill_hidden_without_a_release();
    background_tabs_show_while_loading_and_stay_open();
    the_empty_page_becomes_a_tab_once_navigated();
    tabs_of_removed_pinned_tabs_and_favorites_move_to_today();
    a_malformed_pinned_url_doesnt_crash();
    popups_close_themselves();
    private_windows_write_nothing();
    address_bar_edits_the_current_tab();
    arc_shortcuts_step_through_the_sidebar();
    swiping_the_sidebar_switches_spaces();
    pinned_tabs_rename_in_place();
    duplicate_and_close_tabs_below();
    switching_spaces_slides();
    the_slide_and_color_fade_run_smoothly();
    switching_spaces_keeps_the_switcher_in_place();
    every_chrome_surface_takes_the_space_color();
    fills_from_one_password();

    std::printf("%s: %d failure(s)\n", m_failures == 0 ? "ALL PASSED" : "FAILED", m_failures);
    std::fflush(stdout);
    return m_failures == 0 ? 0 : 1;
}

void schedule_self_test_if_requested()
{
    if (!qEnvironmentVariableIsSet("ARC_SELF_TEST"))
        return;
    QTimer::singleShot(2500, qApp, [] {
        auto* window = Application::the().active_window_if_any();
        auto* sidebar = window ? ArcSidebar::for_window(*window) : nullptr;
        if (!sidebar) {
            std::printf("FAIL: no browser window with a sidebar\n");
            std::fflush(stdout);
            _exit(1);
        }
        SelfTest test(*sidebar);
        auto result = test.run();
        // The test profile is thrown away; skip teardown, which isn't what's under test.
        _exit(result);
    });
}

}
