/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

ExtensionRuntime::ExtensionRuntime(ExtensionRegistry& registry)
    : m_registry(registry)
{
    register_runtime_functions();
    register_messaging_functions();

    m_apis.append(create_storage_api(*this));
    m_apis.append(create_alarms_api(*this));
    m_apis.append(create_native_messaging_api(*this));
    m_apis.append(create_tabs_api(*this));
    m_apis.append(create_management_api(*this));
}

ExtensionRuntime::~ExtensionRuntime() = default;

void ExtensionRuntime::context_created(WebContentPage& page, u64 page_context_id, String extension_id, String type, URL::URL url, bool is_top_level)
{
    // A page process may only speak for an extension that's installed.
    if (!m_registry.find(extension_id))
        return;
    // FIXME: Check that a context claiming to be an extension page is in a process hosting that extension's pages.

    auto context = make<Context>();
    context->id = m_next_context_id++;
    context->page = page;
    context->page_context_id = page_context_id;
    context->extension_id = move(extension_id);
    context->type = move(type);
    context->url = move(url);
    context->is_top_level = is_top_level;
    m_contexts.set(context->id, move(context));
}

void ExtensionRuntime::context_destroyed(WebContentPage& page, u64 page_context_id)
{
    auto* context = find_context(page, page_context_id);
    if (!context)
        return;
    auto id = context->id;
    context_stopped_answering(id);
    for (auto& api : m_apis)
        api->context_destroyed(id);
    m_contexts.remove(id);
}

void ExtensionRuntime::page_closed(WebContentPage& page)
{
    Vector<u64> closed;
    for (auto const& [id, context] : m_contexts) {
        if (context->page.ptr() == &page)
            closed.append(id);
    }
    for (auto id : closed) {
        context_stopped_answering(id);
        for (auto& api : m_apis)
            api->context_destroyed(id);
        m_contexts.remove(id);
    }
}

ExtensionRuntime::Context* ExtensionRuntime::find_context(WebContentPage& page, u64 page_context_id)
{
    for (auto& [id, context] : m_contexts) {
        if (context->page.ptr() == &page && context->page_context_id == page_context_id)
            return context.ptr();
    }
    return nullptr;
}

ExtensionRuntime::Context const* ExtensionRuntime::find_context(u64 id) const
{
    auto it = m_contexts.find(id);
    return it == m_contexts.end() ? nullptr : it->value.ptr();
}

void ExtensionRuntime::send(Context const& context, JsonObject const& message)
{
    if (!context.page || !context.page->is_open())
        return;
    context.page->async_deliver_extension_message(context.page_context_id, message.serialized());
}

void ExtensionRuntime::message_received(WebContentPage& page, u64 page_context_id, StringView text)
{
    auto* context = find_context(page, page_context_id);
    if (!context)
        return;

    auto json = JsonParser::parse(text);
    if (json.is_error() || !json.value().is_object())
        return;
    auto const& message = json.value().as_object();
    auto type = message.get_string("type"sv);
    if (!type.has_value())
        return;

    if (type == "call"sv) {
        handle_call(*context, message);
    } else if (type == "message_response"sv) {
        message_response_received(*context, message);
    } else if (type == "port_post"sv) {
        post_to_port(*context, message);
    } else if (type == "port_disconnect"sv) {
        if (auto port_id = message.get_string("port_id"sv); port_id.has_value())
            disconnect_port({ context->id, *port_id });
    } else if (type == "listen"sv) {
        auto event = message.get_string("event"sv);
        auto listener = message.get_integer<i64>("listener"sv);
        if (event.has_value() && listener.has_value())
            context->listeners.ensure(*event).set(*listener, message.get_array("extra"sv).value_or({}));
    } else if (type == "unlisten"sv) {
        auto event = message.get_string("event"sv);
        auto listener = message.get_integer<i64>("listener"sv);
        if (event.has_value() && listener.has_value()) {
            if (auto listeners = context->listeners.find(*event); listeners != context->listeners.end())
                listeners->value.remove(*listener);
        }
    }
}

void ExtensionRuntime::handle_call(Context& context, JsonObject const& message)
{
    auto id = message.get("id"sv);
    auto api = message.get_string("api"sv);
    if (!id.has_value() || !api.has_value())
        return;
    auto arguments = message.get_array("args"sv).value_or({});

    auto reply = [this, context_id = context.id, id = *id](ErrorOr<JsonValue, String> result) {
        auto const* context = find_context(context_id);
        if (!context)
            return;
        JsonObject reply;
        reply.set("type"sv, "reply"sv);
        reply.set("id"sv, id);
        if (result.is_error())
            reply.set("error"sv, result.release_error());
        else if (!result.value().is_null())
            reply.set("result"sv, result.release_value());
        send(*context, reply);
    };

    auto handler = m_functions.find(*api);
    if (handler == m_functions.end()) {
        reply(MUST(String::formatted("{} isn't supported", *api)));
        return;
    }
    handler->value(context, arguments, move(reply));
}

void ExtensionRuntime::register_function(StringView name, Handler handler)
{
    m_functions.set(MUST(String::from_utf8(name)), move(handler));
}

void ExtensionRuntime::dispatch_event(StringView extension_id, StringView event, JsonArray arguments)
{
    for (auto const& [id, context] : m_contexts) {
        if (context->extension_id != extension_id)
            continue;
        auto listeners = context->listeners.find(event);
        if (listeners == context->listeners.end() || listeners->value.is_empty())
            continue;
        JsonObject message;
        message.set("type"sv, "event"sv);
        message.set("event"sv, event);
        message.set("args"sv, arguments);
        send(*context, message);
    }
}

void ExtensionRuntime::register_runtime_functions()
{
    register_function("runtime.getPlatformInfo"sv, [](auto const&, auto const&, Reply reply) {
        JsonObject info;
#if defined(AK_OS_MACOS)
        info.set("os"sv, "mac"sv);
#elif defined(AK_OS_WINDOWS)
        info.set("os"sv, "win"sv);
#elif defined(AK_OS_ANDROID)
        info.set("os"sv, "android"sv);
#elif defined(AK_OS_OPENBSD)
        info.set("os"sv, "openbsd"sv);
#else
        info.set("os"sv, "linux"sv);
#endif
#if ARCH(AARCH64)
        info.set("arch"sv, "arm64"sv);
        info.set("nacl_arch"sv, "arm"sv);
#else
        info.set("arch"sv, "x86-64"sv);
        info.set("nacl_arch"sv, "x86-64"sv);
#endif
        reply(JsonValue { move(info) });
    });
}

void ExtensionRuntime::set_storage_directory(ByteString directory)
{
    m_storage_directory = move(directory);
}

// Backgrounds: each runs in a hidden view of its own, from the extension's install until it's removed.
void ExtensionRuntime::extensions_changed()
{
    m_registry.for_each_extension([&](Extension const& extension) {
        auto running = m_backgrounds.find(extension.id());
        if (running != m_backgrounds.end() && running->value.version == extension.manifest().version)
            return;
        start_background(extension);
    });
    for (auto& api : m_apis)
        api->extensions_changed();
}

void ExtensionRuntime::start_background(Extension const& extension)
{
    auto url = extension.background_page_url();
    if (!url.has_value() || !create_background_view)
        return;
    auto view = create_background_view();
    if (!view)
        return;

    auto id = extension.id();
    view->on_load_finish = [this, id](URL::URL const&) {
        // Listeners are added as the background's script first runs, so they hear of the install once it has loaded.
        auto background = m_backgrounds.find(id);
        if (background == m_backgrounds.end() || background->value.installed_event_sent)
            return;
        background->value.installed_event_sent = true;
        JsonObject details;
        details.set("reason"sv, "install"sv);
        dispatch_event(id, "runtime.onInstalled"sv, JsonArray { Vector<JsonValue> { move(details) } });

        auto waiting = move(background->value.waiting);
        for (auto& callback : waiting)
            callback();
    };
    view->load(*url);
    m_backgrounds.set(id, Background { .version = extension.manifest().version, .view = move(view), .installed_event_sent = false, .waiting = {} });
}

void ExtensionRuntime::shut_down()
{
    for (auto& api : m_apis)
        api->shut_down();
    m_backgrounds.clear();
}

bool ExtensionRuntime::background_is_starting(StringView extension_id) const
{
    auto background = m_backgrounds.find(extension_id);
    return background != m_backgrounds.end() && !background->value.installed_event_sent;
}

void ExtensionRuntime::wait_for_background(StringView extension_id, Function<void()> callback)
{
    auto background = m_backgrounds.find(extension_id);
    VERIFY(background != m_backgrounds.end());
    background->value.waiting.append(move(callback));
}

ViewImplementation* ExtensionRuntime::background_view(StringView extension_id) const
{
    auto background = m_backgrounds.find(extension_id);
    return background == m_backgrounds.end() ? nullptr : background->value.view.ptr();
}

bool ExtensionRuntime::is_background_page(WebContentPage const& page) const
{
    for (auto const& [id, background] : m_backgrounds) {
        if (background.view && &page.view() == background.view.ptr())
            return true;
    }
    return false;
}

// runtime.MessageSender
JsonObject ExtensionRuntime::sender_of(Context const& context) const
{
    JsonObject sender;
    sender.set("id"sv, context.extension_id);
    sender.set("url"sv, context.url.serialize());
    sender.set("origin"sv, context.url.origin().serialize());
    if (context.page && !is_background_page(*context.page)) {
        JsonObject tab;
        tab.set("id"sv, static_cast<i64>(context.page->view().view_id()));
        tab.set("url"sv, context.page->view().url().serialize());
        sender.set("tab"sv, move(tab));
        sender.set("frameId"sv, context.is_top_level ? 0 : static_cast<i64>(context.id));
    }
    if (context.type == "content_script"sv)
        sender.set("documentLifecycle"sv, "active"sv);
    return sender;
}

bool ExtensionRuntime::Context::has_listener(StringView event) const
{
    auto it = listeners.find(event);
    return it != listeners.end() && !it->value.is_empty();
}

static constexpr auto no_receiver_error = "Could not establish connection. Receiving end does not exist."sv;

void ExtensionRuntime::register_messaging_functions()
{
    // runtime.sendMessage([extensionId], message, [options]): to the extension's pages and background, not its
    // content scripts. The first answer wins; with none, it resolves to nothing.
    register_function("runtime.sendMessage"sv, [this](Context const& caller, JsonArray const& arguments, Reply reply) {
        auto target = arguments.size() > 0 && arguments.at(0).is_string() ? arguments.at(0).as_string() : caller.extension_id;
        // FIXME: Messages to other extensions (runtime.onMessageExternal).
        if (target != caller.extension_id)
            return reply(MUST(String::from_utf8(no_receiver_error)));
        auto message = arguments.size() > 1 ? arguments.at(1) : JsonValue {};

        Vector<Context const*> receivers;
        for (auto const& [id, context] : m_contexts) {
            if (context->id != caller.id && context->extension_id == target && context->type != "content_script"sv && context->has_listener("runtime.onMessage"sv))
                receivers.append(context.ptr());
        }
        if (receivers.is_empty()) {
            if (!background_is_starting(target))
                return reply(MUST(String::from_utf8(no_receiver_error)));
            // Sent as the background starts: it's heard once the background can listen.
            wait_for_background(target, [this, caller_id = caller.id, arguments, reply = move(reply)]() mutable {
                auto const* caller = find_context(caller_id);
                if (!caller)
                    return;
                m_functions.get("runtime.sendMessage"sv).value()(*caller, arguments, move(reply));
            });
            return;
        }

        auto message_id = m_next_message_id++;
        PendingMessage pending { .caller = caller.id, .reply = move(reply), .awaiting = {} };
        JsonObject delivery;
        delivery.set("type"sv, "message"sv);
        delivery.set("message_id"sv, message_id);
        delivery.set("message"sv, message);
        delivery.set("sender"sv, sender_of(caller));
        for (auto const* receiver : receivers) {
            pending.awaiting.set(receiver->id);
            send(*receiver, delivery);
        }
        m_pending_messages.set(message_id, move(pending));
    });

    // runtime.connect([extensionId], [connectInfo]): a port to each of the extension's pages and background listening.
    register_function("runtime.connect"sv, [this](Context const& caller, JsonArray const& arguments, Reply reply) {
        reply(JsonValue {});
        if (arguments.size() < 3 || !arguments.at(2).is_string())
            return;
        auto target = arguments.at(0).is_string() ? arguments.at(0).as_string() : caller.extension_id;
        auto name = arguments.at(1).is_object() ? arguments.at(1).as_object().get_string("name"sv).value_or({}) : String {};

        Vector<Context const*> receivers;
        if (target == caller.extension_id) {
            for (auto const& [id, context] : m_contexts) {
                if (context->id != caller.id && context->extension_id == target && context->type != "content_script"sv && context->has_listener("runtime.onConnect"sv))
                    receivers.append(context.ptr());
            }
        }
        open_channel(caller, arguments.at(2).as_string(), move(receivers), name);
    });
}

void ExtensionRuntime::message_response_received(Context& context, JsonObject const& message)
{
    auto message_id = message.get_u64("message_id"sv);
    if (!message_id.has_value())
        return;
    auto pending = m_pending_messages.find(*message_id);
    if (pending == m_pending_messages.end() || !pending->value.awaiting.contains(context.id))
        return;

    if (message.get_bool("handled"sv).value_or(false)) {
        auto reply = move(pending->value.reply);
        m_pending_messages.remove(pending);
        reply(message.get("response"sv).value_or(JsonValue {}));
        return;
    }
    pending->value.awaiting.remove(context.id);
    if (pending->value.awaiting.is_empty()) {
        auto reply = move(pending->value.reply);
        m_pending_messages.remove(pending);
        reply(JsonValue {});
    }
}

// The context is gone: it answers nothing more, and its ports close.
void ExtensionRuntime::context_stopped_answering(u64 context_id)
{
    Vector<u64> answered;
    for (auto& [message_id, pending] : m_pending_messages) {
        if (pending.caller == context_id) {
            answered.append(message_id);
            continue;
        }
        pending.awaiting.remove(context_id);
        if (pending.awaiting.is_empty())
            answered.append(message_id);
    }
    for (auto message_id : answered) {
        auto pending = m_pending_messages.take(message_id).release_value();
        if (pending.caller != context_id)
            pending.reply(JsonValue {});
    }

    Vector<PortEnd> ends;
    for (auto const& [channel_id, channel] : m_channels) {
        if (channel.opener.context_id == context_id)
            ends.append(channel.opener);
        for (auto const& receiver : channel.receivers) {
            if (receiver.context_id == context_id)
                ends.append(receiver);
        }
    }
    for (auto const& end : ends)
        disconnect_port(end);
}

Optional<u64> ExtensionRuntime::channel_of(PortEnd const& end) const
{
    for (auto const& [channel_id, channel] : m_channels) {
        if (channel.opener == end || channel.receivers.contains_slow(end))
            return channel_id;
    }
    return {};
}

void ExtensionRuntime::open_channel(Context const& opener, String opener_port_id, Vector<Context const*> receivers, StringView name)
{
    PortEnd opener_end { opener.id, move(opener_port_id) };
    if (channel_of(opener_end).has_value())
        return;
    if (receivers.is_empty()) {
        send_port_disconnected(opener_end, MUST(String::from_utf8(no_receiver_error)));
        return;
    }

    Channel channel { .opener = opener_end, .receivers = {} };
    for (auto const* receiver : receivers) {
        PortEnd end { receiver->id, MUST(String::formatted("browser:{}", m_next_port_id++)) };
        JsonObject connect;
        connect.set("type"sv, "port_connect"sv);
        connect.set("port_id"sv, end.port_id);
        connect.set("name"sv, name);
        connect.set("sender"sv, sender_of(opener));
        send(*receiver, connect);
        channel.receivers.append(move(end));
    }
    m_channels.set(m_next_channel_id++, move(channel));
}

// A message from one end goes to the other: from the opener to every receiver, from a receiver to the opener.
void ExtensionRuntime::post_to_port(Context const& context, JsonObject const& message)
{
    auto port_id = message.get_string("port_id"sv);
    if (!port_id.has_value())
        return;
    PortEnd from { context.id, *port_id };
    auto channel_id = channel_of(from);
    if (!channel_id.has_value())
        return;
    auto const& channel = m_channels.get(*channel_id).value();

    auto deliver = [&](PortEnd const& to) {
        auto const* receiver = find_context(to.context_id);
        if (!receiver)
            return;
        JsonObject delivery;
        delivery.set("type"sv, "port_message"sv);
        delivery.set("port_id"sv, to.port_id);
        delivery.set("message"sv, message.get("message"sv).value_or(JsonValue {}));
        send(*receiver, delivery);
    };
    if (channel.opener == from) {
        for (auto const& receiver : channel.receivers)
            deliver(receiver);
    } else {
        deliver(channel.opener);
    }
}

// An end closing: the opener's closes the whole channel; a receiver's closes the opener's too once none are left.
void ExtensionRuntime::disconnect_port(PortEnd const& end, Optional<String> error)
{
    auto channel_id = channel_of(end);
    if (!channel_id.has_value())
        return;
    auto& channel = m_channels.get(*channel_id).value();

    if (channel.opener == end) {
        auto receivers = move(channel.receivers);
        m_channels.remove(*channel_id);
        for (auto const& receiver : receivers)
            send_port_disconnected(receiver, error);
        return;
    }
    channel.receivers.remove_all_matching([&](auto const& receiver) { return receiver == end; });
    if (channel.receivers.is_empty()) {
        auto opener = channel.opener;
        m_channels.remove(*channel_id);
        send_port_disconnected(opener, error);
    }
}

void ExtensionRuntime::send_port_disconnected(PortEnd const& end, Optional<String> error)
{
    auto const* context = find_context(end.context_id);
    if (!context)
        return;
    JsonObject message;
    message.set("type"sv, "port_disconnected"sv);
    message.set("port_id"sv, end.port_id);
    if (error.has_value())
        message.set("error"sv, *error);
    send(*context, message);
}

}
