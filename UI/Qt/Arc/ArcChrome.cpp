/*
 * Entry point for the Arc-style chrome layered on top of the Qt UI.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/PictureInPictureControls.h>
#include <LibURL/Parser.h>
#include <LibWebView/PictureInPictureManager.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Arc/ArcChrome.h>
#include <UI/Qt/Arc/ArcImporter.h>
#include <UI/Qt/Arc/ArcShortcuts.h>
#include <UI/Qt/Arc/ArcSidebar.h>
#include <UI/Qt/Arc/ProfileSessions.h>
#include <UI/Qt/Arc/ArcSelfTest.h>
#include <UI/Qt/Arc/UpdateChecker.h>
#include <UI/Qt/Arc/SpaceStore.h>
#include <UI/Qt/Arc/CommandBar.h>
#include <UI/Qt/Arc/LittleArc.h>
#include <UI/Qt/Arc/MiniPlayer.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/MacWindow.h>
#include <LibGfx/Color.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/WebContentView.h>

#include <QAction>
#include <QActionEvent>
#include <QApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QTimer>

namespace Ladybird::Arc {

// As in Arc, every window's buttons (close, minimize, zoom) are grey, in colour only under the pointer.
static void grey_window_buttons([[maybe_unused]] BrowserWindow& window)
{
#if defined(AK_OS_MACOS)
    // AppKit's look for an inactive window's buttons: a faint dot over the title bar.
    auto tint = ChromeStyle::is_dark(window.palette()) ? Gfx::Color(255, 255, 255, 28) : Gfx::Color(0, 0, 0, 36);
    set_appkit_window_buttons_grey(window, tint);
#endif
}


// Arc keeps tabs in a sidebar. Turn upstream's vertical tabs on the first time a profile runs
// with the Arc chrome; after that the user's own choice in Settings wins.
static void enable_sidebar_on_first_run()
{
    if (!SpaceStore::the().is_new_profile())
        return;

    auto& settings = WebView::Application::settings();
    auto tab_settings = settings.tab_settings();
    tab_settings.vertical_tabs_enabled = true;
    tab_settings.vertical_tabs_expanded = true;
    settings.set_tab_settings(tab_settings);
}

// Cmd+T, the File menu item and the sidebar's "+" button all trigger the
// shared New Tab action. Point it at the command bar instead.
static void route_new_tab_to_command_bar()
{
    auto* action = Application::the().new_tab_action();
    if (!action)
        return;

    QObject::disconnect(action, &QAction::triggered, nullptr, nullptr);
    QObject::connect(action, &QAction::triggered, action, [] {
        auto& application = Application::the();
        if (auto* window = application.active_window_if_any())
            CommandBar::toggle_for(*window);
        else
            application.open_new_tab();
    });
}

// Cmd+L, and upstream's other ways into the location field (a new space's first tab, a new window's new tab page),
// edit the address in the command bar, as in Arc. Windows without the sidebar (popups) keep upstream's field.
static void route_location_editing_to_command_bar()
{
    Tab::on_focus_location_editor = [](Tab& tab) {
        auto* window = qobject_cast<BrowserWindow*>(tab.window());
        if (!window || !ArcSidebar::for_window(*window))
            return false;
        auto* bar = window->findChild<CommandBar*>(QString(), Qt::FindDirectChildrenOnly);
        if (bar && bar->isVisible() && bar->mode() == CommandBar::Mode::EditCurrentTab)
            return true; // Already editing: don't close it (focus requests repeat).
        CommandBar::toggle_for(*window, CommandBar::Mode::EditCurrentTab, &tab);
        return true;
    };
}

// As in Arc, links opened from other apps (Slack, Mail, ...) open in a Little Arc, to look at and close, or keep with
// Cmd+O. Files keep upstream's handling (a tab in the browser window).
static void route_links_from_other_apps_to_little_arc()
{
    auto& application = Application::the();
    application.on_open_file = [upstream = AK::move(application.on_open_file)](URL::URL url) mutable {
        if (url.scheme().is_one_of("http"sv, "https"sv)) {
            open_little_arc(url);
            return;
        }
        if (upstream)
            upstream(AK::move(url));
    };
}

// "Import from Arc..." goes right above "Open File..." in every File menu. Menus are
// rebuilt per window, so watch for that action being added instead of editing upstream.
static QAction* import_action()
{
    static QAction* action = [] {
        auto* action = new QAction("Import from Arc\u2026", qApp);
        QObject::connect(action, &QAction::triggered, action, [] {
            run_import_dialog(Application::the().active_window_if_any());
        });
        return action;
    }();
    return action;
}

static void add_import_action_to(QMenu& menu)
{
    auto* anchor = Application::the().open_file_action();
    if (!anchor || !menu.actions().contains(anchor) || menu.actions().contains(import_action()))
        return;
    menu.insertAction(anchor, import_action());
}

// Watches app-wide events: File menus being built, and browser windows being shown.
class MenuWatcher final : public QObject {
public:
    using QObject::QObject;

    virtual bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
            // Arc's shortcuts. Where upstream binds the same keys (Cmd+D, Cmd+Shift+C, next/previous tab), claiming
            // the shortcut override makes Qt deliver a plain key press, below, instead of firing upstream's action.
            auto& key_event = *static_cast<QKeyEvent*>(event);
            if (auto shortcut = arc_shortcut_for(key_event.key(), key_event.modifiers()); shortcut.has_value()) {
                auto* widget = qobject_cast<QWidget*>(watched);
                auto* window = widget ? qobject_cast<BrowserWindow*>(widget->window()) : nullptr;
                // Little Arc: a new one from anywhere; its own two only in one (elsewhere Cmd+O is Open File).
                auto in_little_arc = window && is_little_arc(*window);
                auto little_arc_command = shortcut->command == ArcCommand::NewLittleArc
                    || (in_little_arc && (shortcut->command == ArcCommand::OpenInSpace || shortcut->command == ArcCommand::ChooseSpace));
                if (little_arc_command) {
                    event->accept();
                    if (event->type() == QEvent::KeyPress) {
                        if (shortcut->command == ArcCommand::NewLittleArc)
                            open_little_arc();
                        else if (shortcut->command == ArcCommand::OpenInSpace)
                            move_little_arc_to_space(*window);
                        else
                            choose_space_for_little_arc(*window);
                    }
                    return true;
                }
                if (shortcut->command == ArcCommand::OpenInSpace || shortcut->command == ArcCommand::ChooseSpace)
                    return false;
                auto* sidebar = window ? ArcSidebar::for_window(*window) : nullptr;
                if (!sidebar)
                    return false;
                if (event->type() == QEvent::ShortcutOverride) {
                    event->accept();
                    return true;
                }
                return sidebar->run_shortcut(*shortcut);
            }
            if (event->type() != QEvent::KeyPress)
                return false;
        }
        if (event->type() == QEvent::KeyPress) {
            // Cheap check first: this sees every key press, often several times as it propagates.
            if (!ArcSidebar::is_space_shortcut(*static_cast<QKeyEvent*>(event)))
                return false;
            if (auto* widget = qobject_cast<QWidget*>(watched)) {
                if (auto* window = qobject_cast<BrowserWindow*>(widget->window())) {
                    if (auto* sidebar = ArcSidebar::for_window(*window); sidebar && sidebar->handle_key_press(*static_cast<QKeyEvent*>(event)))
                        return true;
                }
            }
            return false;
        }
        if (event->type() == QEvent::Show) {
            if (auto* window = qobject_cast<BrowserWindow*>(watched)) {
                ArcSidebar::attach(*window);
                grey_window_buttons(*window);
            }
            return false;
        }
        if (event->type() == QEvent::PaletteChange || event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate) {
            if (auto* window = qobject_cast<BrowserWindow*>(watched); window && window->isVisible())
                grey_window_buttons(*window);
            return false;
        }
        if (event->type() != QEvent::ActionAdded)
            return false;
        if (static_cast<QActionEvent*>(event)->action() != Application::the().open_file_action())
            return false;
        if (auto* menu = qobject_cast<QMenu*>(watched)) {
            QPointer<QMenu> guarded = menu;
            QTimer::singleShot(0, menu, [guarded] {
                if (guarded)
                    add_import_action_to(*guarded);
            });
        }
        return false;
    }
};

static void add_import_menu_items()
{
    if (auto* anchor = Application::the().open_file_action()) {
        for (auto* object : anchor->associatedObjects()) {
            if (auto* menu = qobject_cast<QMenu*>(object))
                add_import_action_to(*menu);
        }
    }
    qApp->installEventFilter(new MenuWatcher(qApp));
}

static QPointF dev_point(char const* name)
{
    auto parts = qEnvironmentVariable(name).split(',');
    return parts.size() == 2 ? QPointF(parts[0].toDouble(), parts[1].toDouble()) : QPointF(-1, -1);
}

static void send_dev_mouse(QWidget& view, QEvent::Type type, QPointF point)
{
    auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    auto buttons = type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton;
    QMouseEvent event(type, point, view.mapToGlobal(point), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&view, &event);
}

static WebContentView* dev_picture_in_picture_view()
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (widget->windowTitle() == "Picture-in-Picture" && widget->isVisible())
            return widget->findChild<WebContentView*>();
    }
    return nullptr;
}

static void run_picture_in_picture_dev_aid()
{
    auto text = qEnvironmentVariable("ARC_DEV_PIP").toUtf8();
    auto url = URL::Parser::basic_parse(StringView(text.constData(), static_cast<size_t>(text.size())));
    auto* window = Application::the().active_window_if_any();
    auto* tab = window ? window->current_tab() : nullptr;
    if (!url.has_value() || !tab)
        return;
    tab->navigate(*url);
    QPointer<BrowserWindow> guarded_window = window;
    QPointer<Tab> guarded_tab = tab;
    QTimer::singleShot(4000, [guarded_window, guarded_tab] {
        if (!guarded_window || !guarded_tab)
            return;
        auto& view = guarded_tab->view();
        auto center = QPointF(view.width() / 2.0, view.height() / 2.0);
        send_dev_mouse(view, QEvent::MouseMove, center);
        send_dev_mouse(view, QEvent::MouseButtonPress, center);
        send_dev_mouse(view, QEvent::MouseButtonRelease, center);
        QTimer::singleShot(1500, [guarded_window] {
            if (!guarded_window)
                return;
            // ARC_DEV_PIP_LEAVE=space leaves for another space (adding one if there's only one), not a new tab.
            if (qEnvironmentVariable("ARC_DEV_PIP_LEAVE") == "space") {
                if (auto* sidebar = ArcSidebar::for_window(*guarded_window)) {
                    if (SpaceStore::the().spaces().size() < 2)
                        SpaceStore::the().add_space("Dev Space", "sf:star", QColor("#9b7fd4"));
                    for (auto const& space : SpaceStore::the().spaces()) {
                        if (space.id != sidebar->current_space_id()) {
                            sidebar->switch_to_space(space.id);
                            break;
                        }
                    }
                }
                return;
            }
            guarded_window->new_tab_from_url(URL::about_blank(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
        });
    });

    if (auto hover = dev_point("ARC_DEV_PIP_HOVER"); hover.x() >= 0) {
        auto* timer = new QTimer(qApp);
        QObject::connect(timer, &QTimer::timeout, [hover] {
            if (auto* view = dev_picture_in_picture_view())
                send_dev_mouse(*view, QEvent::MouseMove, hover);
        });
        timer->start(400);
    }
    if (auto click = dev_point("ARC_DEV_PIP_CLICK"); click.x() >= 0) {
        QTimer::singleShot(9000, [click] {
            if (auto* view = dev_picture_in_picture_view()) {
                send_dev_mouse(*view, QEvent::MouseMove, click);
                send_dev_mouse(*view, QEvent::MouseButtonPress, click);
                send_dev_mouse(*view, QEvent::MouseButtonRelease, click);
            }
        });
    }
    // ARC_DEV_PIP_MINIMIZE=<ms>: clicks the window's Minimize then.
    if (auto delay = qEnvironmentVariableIntValue("ARC_DEV_PIP_MINIMIZE"); delay > 0) {
        QTimer::singleShot(delay, [] {
            auto* view = dev_picture_in_picture_view();
            if (!view)
                return;
            auto dpr = view->devicePixelRatioF();
            auto geometry = Compositing::picture_in_picture_controls_geometry({ qRound(view->width() * dpr), qRound(view->height() * dpr) }, dpr);
            auto center = QPointF(geometry.minimize_button.center().x() / dpr, geometry.minimize_button.center().y() / dpr);
            send_dev_mouse(*view, QEvent::MouseMove, center);
            QTimer::singleShot(300, [center] {
                if (auto* view = dev_picture_in_picture_view()) {
                    send_dev_mouse(*view, QEvent::MouseButtonPress, center);
                    send_dev_mouse(*view, QEvent::MouseButtonRelease, center);
                }
            });
        });
    }
    if (qEnvironmentVariableIsSet("ARC_DEV_MINI_PLAYER_REVEAL")) {
        QTimer::singleShot(11000, [guarded_window] {
            if (!guarded_window)
                return;
            for (auto* player : guarded_window->findChildren<MiniPlayer*>())
                player->set_revealed(true);
        });
    }
}

void install()
{
    UpdateChecker::the().start();
    open_all_profile_sessions();
    schedule_self_test_if_requested();

    // The first window's tab opens before its sidebar exists: give it the profile of the space it will show.
    if (auto const* space = SpaceStore::the().space(SpaceStore::the().last_active_space_id()))
        set_profile_for_new_views(space->profile);

    // Development aid: ARC_DEV_IMPORT=1 imports from Arc at startup without dialogs and logs the summary.
    if (qEnvironmentVariableIsSet("ARC_DEV_IMPORT")) {
        QTimer::singleShot(1500, [] {
            auto result = import_from_arc(default_arc_data_directory());
            if (result.is_error())
                qWarning("Arc import failed: %s", result.error().string_literal().characters_without_null_termination());
            else
                qWarning("Arc import:\n%s", qPrintable(describe(result.value())));
        });
    }

    // Development aid: ARC_DEV_LITTLE_ARC=<url> (or empty) opens a Little Arc at startup.
    if (qEnvironmentVariableIsSet("ARC_DEV_LITTLE_ARC")) {
        QTimer::singleShot(3000, [] {
            auto text = qEnvironmentVariable("ARC_DEV_LITTLE_ARC");
            Optional<URL::URL> url;
            if (!text.isEmpty())
                url = URL::Parser::basic_parse(StringView(text.toUtf8().constData(), static_cast<size_t>(text.toUtf8().size())));
            open_little_arc(url);
        });
    }

    // Development aid: ARC_DEV_PIP=<url> loads a page with a video in the current tab, clicks it (pages that play with
    // sound on a click start), then leaves it for a new tab, so its video goes into Picture-in-Picture. Options:
    // ARC_DEV_PIP_HOVER=<x>,<y> keeps the window's controls up as a resting pointer would, ARC_DEV_PIP_CLICK=<x>,<y>
    // clicks the window there (a control), and ARC_DEV_MINI_PLAYER_REVEAL=1 slides the sidebar's player open.
    if (qEnvironmentVariableIsSet("ARC_DEV_PIP"))
        QTimer::singleShot(3000, [] { run_picture_in_picture_dev_aid(); });

    enable_sidebar_on_first_run();
    route_new_tab_to_command_bar();
    route_location_editing_to_command_bar();
    route_links_from_other_apps_to_little_arc();
    Tab::animates_loading_spinner = false;
    Tab::hides_sidebar_toggle_while_expanded = true;
    // The sidebar's windows line the toolbar's buttons up with the page from the start, rather than correcting
    // upstream's margin once it's laid out (a frame could show at upstream's, making the buttons jump).
    Tab::toolbar_left_margin = [](BrowserWindow& window) -> Optional<int> {
        if (window.is_popup_window())
            return {};
        return ArcSidebar::TOOLBAR_LEFT_MARGIN;
    };
    WebView::PictureInPictureManager::on_windows_changed = &ArcSidebar::picture_in_picture_windows_changed;
    Tab::create_returning_view = &ArcSidebar::create_returning_view;
    // Not for the self-test, which runs beside the real browser.
    if (!qEnvironmentVariableIsSet("ARC_SELF_TEST"))
        register_little_arc_hot_key();
    add_import_menu_items();
}

}
