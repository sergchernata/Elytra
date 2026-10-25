/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibURL/URL.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>

namespace WebView {

// What the browser's UI does for chrome.action: it shows each extension's button, and opens its popup.
class ExtensionActionDelegate {
public:
    virtual ~ExtensionActionDelegate() = default;

    // The extension's button looks different now, or the extension came or went: ask ActionAPI::state_for() again.
    virtual void action_changed(StringView extension_id) = 0;

    enum class ByUser : u8 {
        No,
        Yes,
    };
    // Opens the extension's popup, for the window (the focused one without): as the user clicked the button or pressed
    // its keys, or as the extension asked (action.openPopup), which only an active window allows.
    virtual ErrorOr<void, String> open_popup(StringView extension_id, URL::URL const& popup, Optional<i64> window_id, ByUser) = 0;
};

// chrome.action, and chrome.commands, whose _execute_action command works the button.
class WEBVIEW_API ActionAPI final : public ExtensionAPI {
public:
    explicit ActionAPI(ExtensionRuntime&);

    void set_delegate(ExtensionActionDelegate* delegate) { m_delegate = delegate; }

    // How the button looks in a tab (or everywhere, without one).
    struct State {
        String title;
        Optional<URL::URL> popup;
        String badge_text;
        // Unset, the browser's own.
        Optional<Color> badge_background_color;
        Optional<Color> badge_text_color;
        bool enabled { true };
        // Icon size in pixels -> path in the extension.
        HashMap<u32, String> icon_paths;
    };
    // Nothing for an extension without a button.
    Optional<State> state_for(StringView extension_id, Optional<i64> tab_id = {}) const;

    // The user clicked the button, in a tab: its popup opens, or the extension hears of the click.
    void clicked(StringView extension_id, Optional<i64> tab_id, Optional<i64> window_id = {});

    struct Command {
        String name;
        String shortcut; // As Chrome shows it ("Command+Shift+Y"), empty when unbound.
        String description;
    };
    Vector<Command> commands_for(StringView extension_id) const;
    // The user pressed a command's keys, in a tab.
    void command(StringView extension_id, StringView name, Optional<i64> tab_id, Optional<i64> window_id = {});

    virtual void extensions_changed() override;
    virtual void extension_uninstalled(StringView extension_id) override { m_states.remove(extension_id); }

private:
    struct Values {
        Optional<String> title;
        Optional<String> popup; // A path in the extension; empty for none.
        Optional<String> badge_text;
        Optional<Color> badge_background_color;
        Optional<Color> badge_text_color;
        Optional<bool> enabled;
        Optional<HashMap<u32, String>> icon_paths;
    };
    struct ExtensionState {
        Values global;
        HashMap<i64, Values> tabs;
    };

    ExtensionState& state(StringView extension_id);
    void changed(StringView extension_id);

    HashMap<String, ExtensionState> m_states;
    ExtensionActionDelegate* m_delegate { nullptr };
};

}
