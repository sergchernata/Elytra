/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/Rect.h>
#include <LibURL/URL.h>

namespace WebView {

class ViewImplementation;

// What the browser's UI tells extensions of its windows and tabs (chrome.tabs and chrome.windows), and does for them.
// The UI implements it and installs it with ExtensionRuntime::set_tabs_delegate(); it calls
// ExtensionRuntime::tabs_changed() whenever anything it describes here changes (a tab opened, closed, moved,
// activated, pinned, retitled or given a new icon; a window opened, closed or focused), and the runtime works out
// which events to send. Loads and navigations the runtime sees for itself.
//
// IDs are Chrome's: positive integers, never reused while the browser runs, a tab's the same for its whole life.
class ExtensionTabsDelegate {
public:
    virtual ~ExtensionTabsDelegate() = default;

    struct Window {
        i64 id { 0 };
        bool focused { false };
        bool incognito { false };
        bool always_on_top { false };
        StringView type { "normal"sv };  // "normal", "popup", "panel", "app" or "devtools"
        StringView state { "normal"sv }; // "normal", "minimized", "maximized" or "fullscreen"
        Optional<Gfx::IntRect> bounds;
    };

    struct Tab {
        i64 id { 0 };
        i64 window_id { 0 };
        i64 index { 0 };
        // The view showing the tab, for the runtime to find its frames, load it and run scripts in it.
        ViewImplementation* view { nullptr };
        bool active { false };
        bool highlighted { false };
        bool pinned { false };
        bool audible { false };
        bool muted { false };
        bool discarded { false };
        bool incognito { false };
        Optional<i64> opener_tab_id;
        // What the tab shows. Without these, the runtime asks the view.
        Optional<String> title;
        Optional<String> fav_icon_url;
    };

    // Every window, and every tab, in the UI's order (a window's tabs by index).
    virtual Vector<Window> windows() const = 0;
    virtual Vector<Tab> tabs() const = 0;

    // The window the user last focused, even when none is focused now.
    virtual Optional<i64> last_focused_window_id() const = 0;

    struct CreateTab {
        Optional<i64> window_id;
        Optional<i64> index;
        Optional<URL::URL> url; // Without one, the new tab page.
        bool active { true };
        bool pinned { false };
        Optional<i64> opener_tab_id;
    };
    // Opens a tab; the runtime loads the URL in its view. Returns its ID.
    virtual ErrorOr<i64, String> create_tab(CreateTab) = 0;

    struct UpdateTab {
        Optional<bool> active;
        Optional<bool> highlighted;
        Optional<bool> pinned;
        Optional<bool> muted;
        Optional<i64> opener_tab_id;
    };
    // Changes the tab (the runtime loads a new URL itself).
    virtual ErrorOr<void, String> update_tab(i64 tab_id, UpdateTab) = 0;
    // Moves a tab to an index of a window (-1 for the end).
    virtual ErrorOr<void, String> move_tab(i64 tab_id, i64 window_id, i64 index) = 0;
    virtual void close_tab(i64 tab_id) = 0;

    struct CreateWindow {
        Vector<URL::URL> urls;   // Without any, the new tab page.
        Optional<i64> tab_id;    // A tab to move into the new window.
        StringView type { "normal"sv };
        Optional<StringView> state;
        bool focused { true };
        bool incognito { false };
        Optional<Gfx::IntRect> bounds;
    };
    // Opens a window, with tabs for its URLs (the runtime loads them). Returns its ID.
    virtual ErrorOr<i64, String> create_window(CreateWindow) = 0;

    struct UpdateWindow {
        Optional<bool> focused;
        Optional<bool> draw_attention;
        Optional<StringView> state;
        Optional<i32> left;
        Optional<i32> top;
        Optional<i32> width;
        Optional<i32> height;
    };
    virtual ErrorOr<void, String> update_window(i64 window_id, UpdateWindow) = 0;
    virtual void close_window(i64 window_id) = 0;

    // A data: URL of what the window's active tab shows, as "png" or "jpeg" (with a quality of 0-100).
    virtual void capture_visible_tab(i64 /* window_id */, StringView /* format */, u8 /* quality */, Function<void(ErrorOr<String, String>)> on_complete)
    {
        on_complete("Capturing tabs isn't supported"_string);
    }
};

}
