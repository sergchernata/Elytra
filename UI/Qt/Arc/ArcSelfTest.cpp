/*
 * Integration tests of the Arc sidebar against real browser windows. See ArcSelfTest.h.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Arc/AddressBar.h>
#include <UI/Qt/Arc/ArcSelfTest.h>
#include <UI/Qt/Arc/ArcSidebar.h>
#include <UI/Qt/Arc/CommandBar.h>
#include <UI/Qt/Arc/ProfileSessions.h>
#include <UI/Qt/Arc/SidebarTree.h>
#include <UI/Qt/Arc/SpaceStore.h>
#include <UI/Qt/Arc/Toast.h>
#include <UI/Qt/Arc/UpdateChecker.h>
#include <UI/Qt/BrowserWindow.h>
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
#include <QScrollBar>
#include <QTimer>
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
    check(!ever_away, "nor a \"|\" while it loads its own URL");
    auto row = row_for_node(node);
    check(row.isValid() && row.data(Qt::DisplayRole).toString() == "P1", "then shows the page's title", row.data(Qt::DisplayRole).toString());
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
