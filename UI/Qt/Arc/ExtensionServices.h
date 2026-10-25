/*
 * What extensions show and do through the browser, beyond their buttons (Extensions.h): their notifications
 * (chrome.notifications), shown as cards over the page; their items in the page's context menus and their button's
 * (chrome.contextMenus). Their downloads (chrome.downloads) are the browser's own, in its downloads UI.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Vector.h>
#include <QList>
#include <QPointer>

class QMenu;
class QString;
class QWidget;

namespace WebView {
class Menu;
struct ExtensionNotification;
}

namespace Ladybird {
class BrowserWindow;
class WebContentView;
}

namespace Ladybird::Arc {

// Once, as the browser starts (from install_extensions_ui()).
void install_extension_services();

// The page's context menus get the extensions' items as they open: each WebView menu, with the QMenu that shows it.
struct ExtensionContextMenu {
    WebView::Menu* menu { nullptr };
    QPointer<QMenu> qmenu;
};
void add_extension_items_to_context_menus(WebContentView&, Vector<ExtensionContextMenu>);

// The extension's items for its button's menu ("action" and "all"), in the window's current tab.
void add_extension_action_items(QMenu&, StringView extension_id, BrowserWindow*);

// For the self-test: the page's context menu items for the window's current tab, as add_extension_items_to_context_menus()
// adds them, in a menu that isn't shown.
void add_extension_page_items_for_testing(QMenu&, BrowserWindow&);

// A context menu item's title as the menu shows it: one line, no control characters, elided when long.
QString extension_menu_title(StringView title);

// For the self-test: notifications shown and closed as NotificationsAPI would, the cards (shown or not), and a window's
// "+N more" label, if it has one.
void show_extension_notification_for_testing(WebView::ExtensionNotification const&);
void close_extension_notification_for_testing(StringView extension_id, StringView id);
QList<QWidget*> extension_notification_cards_for_testing();
QWidget* extension_notification_more_label_for_testing(BrowserWindow&);

}
