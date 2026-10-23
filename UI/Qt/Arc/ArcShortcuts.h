/*
 * Arc's keyboard shortcuts, beyond what upstream binds. A key press maps to one of these commands; ArcChrome.cpp
 * catches the keys app-wide (ahead of upstream's own shortcuts, where they clash) and the window's sidebar runs them.
 *
 * Qt's Ctrl is Cmd on macOS, and its Meta is the Control key there.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <QString>
#include <Qt>

namespace Ladybird::Arc {

enum class ArcCommand {
    ToggleSidebar,       // Cmd+S
    CopyUrl,             // Cmd+Shift+C (upstream: dev tools, still on Cmd+Shift+I and F12)
    CopyUrlAsMarkdown,   // Cmd+Option+Shift+C
    NewTabFromClipboard, // Cmd+Option+V
    TogglePin,           // Cmd+D (upstream: bookmark the page)
    ClearToday,          // Cmd+Shift+K
    NextTab,             // Cmd+Option+Down, Cmd+Shift+], Control+Tab, Cmd+Page Down
    PreviousTab,         // Cmd+Option+Up, Cmd+Shift+[, Control+Shift+Tab, Cmd+Page Up
    NextSpace,           // Cmd+Option+Right
    PreviousSpace,       // Cmd+Option+Left
    SidebarItem,         // Cmd+1 to Cmd+8: that item (favorites first, then the list); Cmd+9: the last
};

struct ArcShortcut {
    ArcCommand command;
    int item { 0 }; // SidebarItem: 0-based, or -1 for the last.

    bool operator==(ArcShortcut const&) const = default;
};

Optional<ArcShortcut> arc_shortcut_for(int key, Qt::KeyboardModifiers);

// "[title](url)", for Copy URL as Markdown. An empty title uses the URL.
QString url_as_markdown(QString title, QString const& url);

}
