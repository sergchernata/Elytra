/*
 * Entry point for the Arc-style chrome layered on top of the Qt UI.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Settings.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Arc/ArcChrome.h>
#include <UI/Qt/Arc/CommandBar.h>
#include <UI/Qt/BrowserWindow.h>

#include <QAction>
#include <QSettings>

namespace Ladybird::Arc {

// Arc keeps tabs in a sidebar. Turn upstream's vertical tabs on once; after
// that the user's own choice in Settings wins.
static void enable_sidebar_on_first_run()
{
    QSettings arc_settings("Ladybird", "ArcChrome");
    if (arc_settings.value("sidebar_enabled_once").toBool())
        return;

    auto& settings = WebView::Application::settings();
    auto tab_settings = settings.tab_settings();
    tab_settings.vertical_tabs_enabled = true;
    tab_settings.vertical_tabs_expanded = true;
    settings.set_tab_settings(tab_settings);

    arc_settings.setValue("sidebar_enabled_once", true);
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

void install()
{
    enable_sidebar_on_first_run();
    route_new_tab_to_command_bar();
}

}
