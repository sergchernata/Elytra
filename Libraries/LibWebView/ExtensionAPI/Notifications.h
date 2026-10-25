/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionRuntime.h>

namespace WebView {

// What a URL an extension gave (an icon, a file to download) holds: a file of the extension's own (a chrome-extension://
// URL of its), a data: URL, or a blob: URL one of its pages made. Anything else, other extensions' files included, is
// refused.
struct ExtensionResource {
    ByteBuffer bytes;
    String mime_type; // Empty when unknown.
};
WEBVIEW_API ErrorOr<ExtensionResource, String> read_extension_resource(ExtensionRuntime const&, ExtensionRuntime::Context const&, URL::URL const&, size_t maximum_size);

// A notification an extension shows (chrome.notifications' NotificationOptions, with its images read).
struct ExtensionNotification {
    String extension_id;
    String id;
    String type; // "basic", "image", "list" or "progress"
    String title;
    String message;
    String context_message;
    // The images' bytes (PNG, JPEG, ...), as the extension gave them; the UI decodes them.
    ByteBuffer icon;
    Optional<ByteBuffer> image;
    i32 priority { 0 }; // -2 to 2
    Optional<double> event_time; // Milliseconds since the epoch.
    Vector<String> buttons;      // At most two titles.
    struct Item {
        String title;
        String message;
    };
    Vector<Item> items;
    Optional<i32> progress; // 0 to 100
    bool require_interaction { false };
    bool silent { false };
};

// What the browser's UI does for chrome.notifications: it shows the notifications, and tells NotificationsAPI what the
// user did with them.
class ExtensionNotificationsDelegate {
public:
    virtual ~ExtensionNotificationsDelegate() = default;

    // A new notification, or one the extension changed (the same extension and ID): show it, or update it.
    virtual void show_notification(ExtensionNotification const&) = 0;
    // The extension cleared it, or the extension is gone.
    virtual void close_notification(StringView extension_id, StringView id) = 0;
};

// chrome.notifications. Notifications last as long as the browser runs, or until cleared or closed.
class WEBVIEW_API NotificationsAPI final : public ExtensionAPI {
public:
    explicit NotificationsAPI(ExtensionRuntime&);

    void set_delegate(ExtensionNotificationsDelegate* delegate) { m_delegate = delegate; }

    // What the user did, from the UI. Nothing happens for a notification that isn't showing.
    void clicked(StringView extension_id, StringView id);
    void button_clicked(StringView extension_id, StringView id, size_t button_index);
    // The notification went away (by_user: the user dismissed it, rather than it timing out).
    void closed(StringView extension_id, StringView id, bool by_user);

    Optional<ExtensionNotification const&> find(StringView extension_id, StringView id) const;
    Vector<ExtensionNotification const*> notifications_of(StringView extension_id) const;

    virtual void extensions_changed() override;

private:
    ExtensionNotification const* find_showing(StringView extension_id, StringView id) const;
    void dispatch(StringView extension_id, StringView event, JsonArray arguments);

    // Extension ID -> notification ID -> notification.
    HashMap<String, HashMap<String, ExtensionNotification>> m_notifications;
    ExtensionNotificationsDelegate* m_delegate { nullptr };
};

}
