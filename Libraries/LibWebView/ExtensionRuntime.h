/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/OwnPtr.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <LibURL/URL.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class ActionAPI;
class Extension;
class ExtensionAPI;
class ExtensionRegistry;
class ExtensionTabsDelegate;
class ManagementAPI;
class ViewImplementation;
class WebContentPage;

// The browser's side of extensions' chrome.* APIs: it knows each extension context in the page processes, answers
// their calls, and sends them events and messages (Base/res/ladybird/extensions/chrome-api.js has the protocol).
class WEBVIEW_API ExtensionRuntime {
public:
    struct Context {
        AK_ALLOC_WITH_KMALLOC;

        u64 id { 0 };
        RefPtr<WebContentPage> page;
        u64 page_context_id { 0 };
        String extension_id;
        String type; // "page", "background", "popup" or "content_script"
        URL::URL url;
        bool is_top_level { false };
        // The frame the context is in, as its page reports it ("frame" message).
        Optional<Web::HTML::CrossProcessId> navigable_id;
        // Event name -> listener IDs (with what was passed after the callback).
        HashMap<String, HashMap<i64, JsonArray>> listeners;

        bool has_listener(StringView event) const;
    };

    using Reply = Function<void(ErrorOr<JsonValue, String>)>;
    using Handler = Function<void(Context const&, JsonArray const& arguments, Reply)>;

    explicit ExtensionRuntime(ExtensionRegistry&);
    ~ExtensionRuntime();

    // From the page processes.
    void context_created(WebContentPage&, u64 page_context_id, String extension_id, String type, URL::URL url, bool is_top_level);
    void context_destroyed(WebContentPage&, u64 page_context_id);
    void page_closed(WebContentPage&);
    void message_received(WebContentPage&, u64 page_context_id, StringView message);

    // An API function, named as the extension calls it ("storage.local.get").
    void register_function(StringView name, Handler);
    // Calls one, as if the context had.
    void call_function(StringView name, Context const&, JsonArray const& arguments, Reply);

    // Sends an event to the extension's contexts listening to it.
    void dispatch_event(StringView extension_id, StringView event, JsonArray arguments);

    // Where chrome.storage.local and sync are kept; until it's set, nothing is saved.
    void set_storage_directory(ByteString);

    // Makes the hidden view an extension's background runs in.
    Function<OwnPtr<ViewImplementation>()> create_background_view;

    // Starts (or restarts) the backgrounds of installed extensions whose background isn't running.
    void extensions_changed();
    // Closes the backgrounds, while the application they need is still whole.
    void shut_down();
    ViewImplementation* background_view(StringView extension_id) const;
    // Closes an extension's background (as it's disabled), or restarts it (runtime.reload, or as it's enabled).
    void stop_background(StringView extension_id);
    void restart_background(StringView extension_id);
    // runtime.reload: restarts the background and reloads the extension's pages.
    void reload_extension(StringView extension_id);

    // chrome.permissions, chrome.management, chrome.privacy: what extensions are granted, enabled, and set.
    ManagementAPI& management() const { return *m_management; }

    // What the browser's UI provides. permissions.request asks the user for what isn't granted yet; without this,
    // requests are denied.
    Function<void(Extension const&, Vector<String> permissions, Vector<String> origins, Function<void(bool granted)>)> request_permissions_prompt;
    // runtime.openOptionsPage; without this, the options page opens in a new tab.
    Function<void(Extension const&, URL::URL const&)> open_options_page;
    // management.uninstallSelf and uninstall, when they ask the user first; without this, those fail.
    Function<void(Extension const&, Function<void(bool confirmed)>)> confirm_uninstall;

    Context const* find_context(u64 id) const;

    template<typename Callback>
    void for_each_context(Callback callback) const
    {
        for (auto const& [id, context] : m_contexts)
            callback(*context);
    }

    ExtensionRegistry& registry() const { return m_registry; }
    // How big an extension page's document is, as it reports it: the UI fits a popup to it.
    Function<void(Context const&, i32 width, i32 height)> on_document_size;

    // chrome.action and chrome.commands, which the UI shows and works.
    ActionAPI& action() const { return *m_action; }
    Optional<ByteString> const& storage_directory() const { return m_storage_directory; }

    // A message to a context, in chrome-api.js's protocol.
    void send(Context const&, JsonObject const& message);

    // runtime.MessageSender, for a context.
    JsonObject sender_of(Context const&) const;
    bool is_background_page(WebContentPage const&) const;

    // While an extension's background is loading, what it should hear waits.
    bool background_is_starting(StringView extension_id) const;
    void wait_for_background(StringView extension_id, Function<void()>);

    // chrome.tabs and chrome.windows: the UI's tabs and windows (ExtensionAPI/TabsDelegate.h). The UI calls
    // tabs_changed() whenever any of them changed; without a delegate, extensions see no tabs.
    void set_tabs_delegate(ExtensionTabsDelegate*);
    ExtensionTabsDelegate* tabs_delegate() const { return m_tabs_delegate; }
    void tabs_changed();

    // What ExtensionAPI/Tabs.cpp hooks in.
    struct TabsHooks {
        Function<void()> tabs_changed;
        // Fills in a sender's tab, frameId and documentId; false when the context isn't in a tab.
        Function<bool(Context const&, JsonObject& sender)> describe_sender;
        // A message from a page process for no context (page_context_id 0): its frames' navigations, and the results
        // of scripts the browser ran.
        Function<void(WebContentPage&, JsonObject const&)> page_message;
        Function<void(WebContentPage&)> page_closed;
    };
    TabsHooks tabs_hooks;

    // Ports (runtime.connect and its kind): a channel from an opener's end to each receiver's.
    void open_channel(Context const& opener, String opener_port_id, Vector<Context const*> receivers, StringView name);
    // A message (runtime.sendMessage and its kind) to each receiver's runtime.onMessage: the first answer is the reply.
    void deliver_message(Context const& sender, Vector<Context const*> receivers, JsonValue const& message, Reply);

    // A port whose other end is in the browser (a native messaging host) rather than in a context.
    class BrowserPortEnd {
    public:
        virtual ~BrowserPortEnd() = default;
        virtual void browser_port_message(u64 port, JsonValue const& message) = 0;
        // The context's end closed, or the context went away.
        virtual void browser_port_disconnected(u64 port) = 0;
    };
    Optional<u64> open_browser_port(Context const& opener, String opener_port_id, BrowserPortEnd&);
    void post_from_browser_port(u64 port, JsonValue const& message);
    void close_browser_port(u64 port, Optional<String> error = {});

    // Where native messaging hosts' manifests are looked for, in order; until it's set, the default directories.
    void set_native_host_directories(Vector<ByteString>);
    Vector<ByteString> native_host_directories() const;

private:
    Context* find_context(WebContentPage&, u64 page_context_id);
    void handle_call(Context&, JsonObject const& message);

    void register_runtime_functions();
    void register_messaging_functions();

    void start_background(Extension const&);

    void message_response_received(Context&, JsonObject const&);
    void context_stopped_answering(u64 context_id);

    struct PortEnd {
        u64 context_id { 0 };
        String port_id;
        bool operator==(PortEnd const&) const = default;
    };
    struct Channel {
        PortEnd opener;
        Vector<PortEnd> receivers;
        // Instead of receivers, the browser.
        BrowserPortEnd* browser_end { nullptr };
    };
    Optional<u64> channel_of(PortEnd const&) const;
    void post_to_port(Context const&, JsonObject const&);
    void disconnect_port(PortEnd const&, Optional<String> error = {});
    void send_port_disconnected(PortEnd const&, Optional<String> error);


    ExtensionRegistry& m_registry;
    HashMap<u64, NonnullOwnPtr<Context>> m_contexts;
    u64 m_next_context_id { 1 };
    HashMap<String, Handler> m_functions;

    struct PendingMessage {
        u64 caller { 0 };
        Reply reply;
        HashTable<u64> awaiting;
    };
    HashMap<u64, PendingMessage> m_pending_messages;
    u64 m_next_message_id { 1 };

    HashMap<u64, Channel> m_channels;
    u64 m_next_channel_id { 1 };
    u64 m_next_port_id { 1 };

    struct Background {
        String version;
        OwnPtr<ViewImplementation> view;
        bool installed_event_sent { false };
        // What waits for the background to have started: messages sent before anything could hear them.
        Vector<Function<void()>> waiting;
    };
    HashMap<String, Background> m_backgrounds;

    Optional<ByteString> m_storage_directory;
    Optional<Vector<ByteString>> m_native_host_directories;

    Vector<NonnullOwnPtr<ExtensionAPI>> m_apis;
    ActionAPI* m_action { nullptr };
    ExtensionTabsDelegate* m_tabs_delegate { nullptr };
    ManagementAPI* m_management { nullptr };

    // onInstalled and onStartup: the version each extension's background last started with in this run.
    Vector<JsonObject> lifecycle_events(Extension const&);
    HashMap<String, String> m_started_versions;
};

}
