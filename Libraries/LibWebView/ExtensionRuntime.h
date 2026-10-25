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
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class Extension;
class ExtensionAPI;
class ExtensionRegistry;
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

    Context const* find_context(u64 id) const;

    template<typename Callback>
    void for_each_context(Callback callback) const
    {
        for (auto const& [id, context] : m_contexts)
            callback(*context);
    }

    ExtensionRegistry& registry() const { return m_registry; }
    Optional<ByteString> const& storage_directory() const { return m_storage_directory; }

    // A message to a context, in chrome-api.js's protocol.
    void send(Context const&, JsonObject const& message);

    // runtime.MessageSender, for a context.
    JsonObject sender_of(Context const&) const;
    bool is_background_page(WebContentPage const&) const;

    // While an extension's background is loading, what it should hear waits.
    bool background_is_starting(StringView extension_id) const;
    void wait_for_background(StringView extension_id, Function<void()>);

    // Ports (runtime.connect and its kind): a channel from an opener's end to each receiver's.
    void open_channel(Context const& opener, String opener_port_id, Vector<Context const*> receivers, StringView name);

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

    Vector<NonnullOwnPtr<ExtensionAPI>> m_apis;
};

}
