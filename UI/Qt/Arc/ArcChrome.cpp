/*
 * Entry point for the Arc-style chrome layered on top of the Qt UI.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Settings.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Arc/ArcChrome.h>
#include <UI/Qt/Arc/ArcImporter.h>
#include <UI/Qt/Arc/ArcSidebar.h>
#include <UI/Qt/Arc/CommandBar.h>
#include <UI/Qt/Arc/SpaceStore.h>
#include <UI/Qt/BrowserWindow.h>

#include <QAction>
#include <QActionEvent>
#include <QApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QPointer>
#include <QTimer>

namespace Ladybird::Arc {

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
        if (event->type() == QEvent::KeyPress) {
            if (auto* widget = qobject_cast<QWidget*>(watched)) {
                if (auto* window = qobject_cast<BrowserWindow*>(widget->window())) {
                    if (auto* sidebar = ArcSidebar::for_window(*window); sidebar && sidebar->handle_key_press(*static_cast<QKeyEvent*>(event)))
                        return true;
                }
            }
            return false;
        }
        if (event->type() == QEvent::Show) {
            if (auto* window = qobject_cast<BrowserWindow*>(watched))
                ArcSidebar::attach(*window);
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

void install()
{
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

    enable_sidebar_on_first_run();
    route_new_tab_to_command_bar();
    add_import_menu_items();
}

}
