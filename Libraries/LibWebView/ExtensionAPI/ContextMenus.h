/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebExtensions/MatchPattern.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>

namespace WebView {

// chrome.contextMenus: the items extensions add to the page's context menu, and to their button's.
//
// Items last while the browser runs. Chrome keeps them across restarts only for event pages; MV3 extensions create them
// in runtime.onInstalled, which isn't sent again on restart, so here they are created anew as backgrounds start (as
// 1Password does, from its own startup code).
class WEBVIEW_API ContextMenusAPI final : public ExtensionAPI {
public:
    explicit ContextMenusAPI(ExtensionRuntime&);

    // What the user right-clicked.
    struct Target {
        URL::URL page_url;
        // A frame's URL, when the click was in one.
        Optional<URL::URL> frame_url;
        Optional<URL::URL> link_url;
        // An image's, video's or audio's.
        Optional<URL::URL> src_url;
        Optional<String> media_type; // "image", "video" or "audio"
        Optional<String> selection_text;
        bool editable { false };
        // The tab, for onClicked's tab argument.
        Optional<i64> tab_id;
        // An extension's button (the "action" context): only that extension's items.
        Optional<String> action_of_extension;
    };

    // An item to show, in order. An extension with more than one top-level item for the page gets a parent named after
    // it, as in Chrome.
    struct MenuItem {
        String extension_id;
        // What clicked() takes; empty for the extension's own parent, which isn't an item of the extension.
        String key;
        String title; // With %s replaced by the selection.
        String type;  // "normal", "checkbox", "radio" or "separator"
        bool checked { false };
        bool enabled { true };
        Vector<MenuItem> children;
    };
    Vector<MenuItem> items_for(Target const&) const;

    // The user chose an item that items_for() gave for the target.
    void clicked(StringView extension_id, StringView key, Target const&);

    // How many items the extension has (for tests and the UI).
    size_t item_count(StringView extension_id) const;

    virtual void extensions_changed() override;

private:
    struct Item {
        JsonValue id;    // As the extension gave it: a string or an integer.
        String key;      // id, serialized.
        Optional<String> parent_key;
        String type { "normal"_string };
        String title;
        Vector<String> contexts;
        Optional<WebExtensions::MatchPatternSet> document_url_patterns;
        Optional<WebExtensions::MatchPatternSet> target_url_patterns;
        bool checked { false };
        bool enabled { true };
        bool visible { true };
    };
    struct ExtensionItems {
        Vector<Item> items; // In the order they were created.
        i64 next_generated_id { 1 };
        String version; // The extension's, when the items were made.
    };

    Item* find_item(StringView extension_id, StringView key);
    Item const* find_item(StringView extension_id, StringView key) const;
    void remove_with_descendants(ExtensionItems&, StringView key);
    void fix_radio_group(ExtensionItems&, StringView key, bool checked_now);
    ExtensionItems& items_of(StringView extension_id);
    bool matches(Item const&, Target const&) const;
    MenuItem menu_item_for(ExtensionItems const&, Item const&, Target const&) const;

    HashMap<String, ExtensionItems> m_items;
};

}
