/*
 * Extensions in the Arc UI: each extension's button in the sidebar's top strip, with its badge and popup; the
 * Extensions window, where extensions are installed from the Chrome Web Store or a folder, turned on and off, and
 * removed; their keyboard commands; and the windows and tabs they see (chrome.tabs and chrome.windows).
 *
 * The extensions themselves run in LibWebView (ExtensionRegistry, ExtensionRuntime): this is what the user sees of them.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QWidget>

class QKeyEvent;
class QMenu;

namespace Ladybird {
class BrowserWindow;
}

namespace Ladybird::Arc {

// Once, as the browser starts.
void install_extensions_ui();

// The row of extension buttons in a window's sidebar; it places itself at the right of its parent.
QWidget* create_extension_buttons(BrowserWindow&, QWidget* parent);

// An extension's keyboard command, if the keys are one: it runs, and the keys go no further.
bool handle_extension_shortcut(QKeyEvent const&, BrowserWindow*);

void show_extensions_window();

// Whether the browser itself uses the keys (an extension's command can't take them): its menus' shortcuts, its other
// shortcuts (Find Next and Previous), and those every Mac app has.
bool is_browser_shortcut(QKeyCombination);

// "Extensions…", for a menu.
void add_extensions_action_to(QMenu&);

}
