/*
 * Integration tests of the Arc sidebar against real browser windows. See ArcSelfTest.h.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Arc/ArcSelfTest.h>
#include <UI/Qt/Arc/ArcSidebar.h>
#include <UI/Qt/Arc/CommandBar.h>
#include <UI/Qt/Arc/ProfileSessions.h>
#include <UI/Qt/Arc/SidebarTree.h>
#include <UI/Qt/Arc/SpaceStore.h>
#include <UI/Qt/Arc/UpdateChecker.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/WebContentView.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLayout>
#include <QScrollBar>
#include <QTimer>

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
