/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibURL/Parser.h>
#include <LibWebExtensions/MatchPattern.h>
#include <LibWebExtensions/NativeMessaging.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebExtensions/ExtensionId.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/ExtensionAPI/Action.h>
#include <LibWebView/ExtensionAPI/ContextMenus.h>
#include <LibWebView/ExtensionAPI/Downloads.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionAPI/Management.h>
#include <LibWebView/ExtensionAPI/Notifications.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

ExtensionRuntime::ExtensionRuntime(ExtensionRegistry& registry)
    : m_registry(registry)
{
    register_runtime_functions();
    register_messaging_functions();

    // What's kept for an extension goes with it, so another installed later with its ID starts afresh.
    m_registry.add_uninstall_observer([this](StringView id) {
        stop_background(id);
        set_content_scripts_may_use_session_storage(id, false);
        for (auto& api : m_apis)
            api->extension_uninstalled(id);
        if (m_storage_directory.has_value() && WebExtensions::is_valid_extension_id(id)) {
            auto directory = LexicalPath::join(*m_storage_directory, id).string();
            if (auto result = ExtensionRegistry::remove_without_following_links(directory); result.is_error() && result.error().code() != ENOENT)
                dbgln("Extensions: Can't remove {}: {}", directory, result.error());
        }
    });

    m_apis.append(create_storage_api(*this));
    m_apis.append(create_action_api(*this));
    m_action = static_cast<ActionAPI*>(m_apis.last().ptr());
    m_apis.append(create_alarms_api(*this));
    m_apis.append(create_native_messaging_api(*this));
    m_apis.append(create_tabs_api(*this));
    m_apis.append(create_management_api(*this));
    m_management = static_cast<ManagementAPI*>(m_apis.last().ptr());
    m_apis.append(create_notifications_api(*this));
    m_notifications = static_cast<NotificationsAPI*>(m_apis.last().ptr());
    m_apis.append(create_context_menus_api(*this));
    m_context_menus = static_cast<ContextMenusAPI*>(m_apis.last().ptr());
    m_apis.append(create_downloads_api(*this));
    m_downloads = static_cast<DownloadsAPI*>(m_apis.last().ptr());
    m_apis.append(create_web_request_api(*this));
}

ExtensionRuntime::~ExtensionRuntime() = default;

bool ExtensionRuntime::is_private_page(WebContentPage const& page)
{
    return page.view().is_private() == IsPrivate::Yes || (page.is_open() && page.client().is_private() == IsPrivate::Yes);
}

String ExtensionRuntime::installed_extensions_for(WebContentPage const& page) const
{
    if (is_private_page(page))
        return R"({"extensions":[]})"_string;
    return m_registry.serialize_for_renderers();
}

void ExtensionRuntime::context_created(WebContentPage& page, u64 page_context_id, String extension_id, String type, URL::URL url, bool is_top_level)
{
    if (is_private_page(page))
        return;
    // A page process may only speak for an extension that's installed, and only one hosting the extension's pages may
    // have its pages' API: any other gets a content script's at most.
    if (!m_registry.find(extension_id))
        return;
    if (type != "content_script"sv && !page.client().may_speak_for_extension(extension_id)) {
        page.client().did_misbehave("did_create_extension_context"sv, "an extension page outside the extension's process"sv);
        return;
    }
    if (type != "content_script"sv && type != "page"sv && type != "background"sv && type != "popup"sv)
        return;
    // A page's context IDs are its own, each used once.
    if (find_context(page, page_context_id) || find_unverified_context(page, page_context_id).has_value()) {
        dbgln("Extensions: A page created context {} twice", page_context_id);
        return;
    }

    auto context = make<Context>();
    context->id = m_next_context_id++;
    context->page = page;
    context->page_context_id = page_context_id;
    context->extension_id = move(extension_id);
    context->type = move(type);
    context->url = move(url);
    context->is_top_level = is_top_level;
    // A content script is one once its frame is verified ("frame" message), with the browser's URL for it. A page has
    // only so many waiting: one per extension per document it shows.
    if (context->type == "content_script"sv) {
        static constexpr size_t maximum_unverified_contexts_per_page = 64;
        size_t waiting = 0;
        for (auto const& unverified : m_unverified_contexts)
            waiting += unverified->page.ptr() == &page ? 1 : 0;
        if (waiting >= maximum_unverified_contexts_per_page) {
            dbgln("Extensions: A page made too many content script contexts");
            return;
        }
        m_unverified_contexts.append(move(context));
        return;
    }
    m_contexts.set(context->id, move(context));
}

Optional<size_t> ExtensionRuntime::find_unverified_context(WebContentPage const& page, u64 page_context_id) const
{
    for (size_t i = 0; i < m_unverified_contexts.size(); ++i) {
        if (m_unverified_contexts[i]->page.ptr() == &page && m_unverified_contexts[i]->page_context_id == page_context_id)
            return i;
    }
    return {};
}

static Optional<Web::HTML::CrossProcessId> cross_process_id_in(JsonObject const& message, StringView key)
{
    auto object = message.get_object(key);
    if (!object.has_value())
        return {};
    auto namespace_id = object->get_u64("ns"sv);
    auto local_id = object->get_u64("id"sv);
    if (!namespace_id.has_value() || !local_id.has_value())
        return {};
    return Web::HTML::CrossProcessId { .namespace_id = *namespace_id, .local_id = *local_id };
}

static bool needs_creator(URL::URL const& url)
{
    return url.scheme().is_one_of("about"sv, "data"sv, "blob"sv, "filesystem"sv);
}

Optional<URL::URL> ExtensionRuntime::creator_url_for_matching(CanonicalNavigable const& navigable)
{
    auto const& origin = navigable.active_document().origin();
    bool is_about = navigable.active_document_url().scheme() == "about"sv;
    for (auto const* frame = navigable.parent(); frame; frame = frame->parent()) {
        // An about:blank or srcdoc document another document made has that one's origin, not its parent's.
        // FIXME: Look at the opener too, and use an opaque origin's precursor, which origins don't keep yet.
        if (is_about && !frame->active_document().origin().is_same_origin(origin))
            break;
        if (!needs_creator(frame->active_document_url()))
            return frame->active_document_url();
    }
    if (is_about && !origin.is_opaque())
        return URL::Parser::basic_parse(ByteString::formatted("{}/", origin.serialize()));
    return {};
}

bool ExtensionRuntime::content_scripts_are_for(Extension const& extension, CanonicalNavigable const& navigable) const
{
    // As the page process matches them (LibWeb/Extensions/ExtensionContexts.cpp), with the browser's URL.
    auto const& frame_url = navigable.active_document_url();
    auto creator_url = creator_url_for_matching(navigable);
    auto allows_file_access = m_registry.allows_file_access(extension.id());
    auto patterns = [](Vector<String> const& strings) {
        WebExtensions::MatchPatternSet set;
        for (auto const& string : strings) {
            if (auto pattern = WebExtensions::MatchPattern::parse(string); !pattern.is_error())
                set.add(pattern.release_value());
        }
        return set;
    };
    for (auto const& script : extension.manifest().content_scripts) {
        // Scripts of the page's own world make no context.
        if (script.world == WebExtensions::ContentScript::World::Main)
            continue;
        if (!script.all_frames && !navigable.is_top_level_traversable())
            continue;
        auto url = WebExtensions::url_for_matching(frame_url, creator_url, script.match_about_blank, script.match_origin_as_fallback);
        if (!url.has_value() || WebExtensions::is_restricted_url(*url, allows_file_access, extension.id().bytes_as_string_view()))
            continue;
        if (WebExtensions::should_inject_content_script(*url, patterns(script.matches), patterns(script.exclude_matches), script.include_globs, script.exclude_globs))
            return true;
    }
    return false;
}

void ExtensionRuntime::verify_content_script(size_t unverified_index, WebContentPage& page, JsonObject const& message)
{
    auto context = m_unverified_contexts.take(unverified_index);
    auto navigable_id = cross_process_id_in(message, "navigable"sv);
    auto document_id = cross_process_id_in(message, "document"sv);
    if (!navigable_id.has_value() || !document_id.has_value() || !page.is_open())
        return;
    auto navigable = page.hosted_navigable(*navigable_id);
    if (!navigable.has_value() || navigable->active_document().id() != *document_id)
        return;
    auto const* extension = m_registry.find(context->extension_id);
    if (!extension)
        return;
    bool injecting = tabs_hooks.is_injecting && tabs_hooks.is_injecting(page, *navigable_id, *document_id, context->extension_id);
    if (!injecting && !content_scripts_are_for(*extension, *navigable)) {
        dbgln("Extensions: A page said it has a content script of {} where it has none", context->extension_id);
        return;
    }
    // An extension has one isolated world, so one content script context, in a document.
    for (auto const& [id, existing] : m_contexts) {
        if (existing->type == "content_script"sv && existing->page.ptr() == &page && existing->extension_id == context->extension_id
            && existing->navigable_id == navigable_id && existing->document_id == document_id) {
            dbgln("Extensions: A page made a second content script context of {} in a document", context->extension_id);
            return;
        }
    }
    context->url = navigable->active_document_url();
    context->is_top_level = navigable->is_top_level_traversable();
    context->navigable_id = *navigable_id;
    context->document_id = *document_id;
    m_contexts.set(context->id, move(context));
}

// The frame a content script's context is in, while its document is the frame's active one.
CanonicalNavigable* ExtensionRuntime::content_script_frame(Context const& context) const
{
    if (!context.page || !context.page->is_open() || !context.navigable_id.has_value() || !context.document_id.has_value())
        return nullptr;
    auto navigable = context.page->hosted_navigable(*context.navigable_id);
    if (!navigable.has_value() || navigable->active_document().id() != *context.document_id)
        return nullptr;
    return navigable.ptr();
}

void ExtensionRuntime::set_content_scripts_may_use_session_storage(StringView extension_id, bool allowed)
{
    auto id = MUST(String::from_utf8(extension_id));
    if (allowed)
        m_session_storage_for_content_scripts.set(id);
    else
        m_session_storage_for_content_scripts.remove(id);
}

// Content scripts have runtime.onMessage and onConnect (which aren't events here), and the storage changes they may see.
static bool is_content_script_event(StringView event)
{
    return event.is_one_of("runtime.onMessage"sv, "runtime.onConnect"sv, "storage.onChanged"sv, "storage.local.onChanged"sv,
        "storage.sync.onChanged"sv, "storage.session.onChanged"sv, "storage.managed.onChanged"sv);
}

bool ExtensionRuntime::content_script_may_hear(Context const& context, StringView event, JsonArray const& arguments) const
{
    if (!is_content_script_event(event) || !m_management->has_permission(context.extension_id, "storage"sv))
        return false;
    bool session = event == "storage.session.onChanged"sv
        || (event == "storage.onChanged"sv && (arguments.size() < 2 || !arguments.at(1).is_string() || arguments.at(1).as_string() == "session"sv));
    return !session || content_scripts_may_use_session_storage(context.extension_id);
}

void ExtensionRuntime::context_destroyed(WebContentPage& page, u64 page_context_id)
{
    if (auto unverified = find_unverified_context(page, page_context_id); unverified.has_value()) {
        m_unverified_contexts.remove(*unverified);
        return;
    }
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
    if (tabs_hooks.page_closed)
        tabs_hooks.page_closed(page);
    m_unverified_contexts.remove_all_matching([&](auto const& context) { return context->page.ptr() == &page; });
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
    for (auto& api : m_apis)
        api->page_gone(page);
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

void ExtensionRuntime::register_page_message(StringView type, PageMessageHandler handler)
{
    m_page_message_handlers.set(MUST(String::from_utf8(type)), move(handler));
}

void ExtensionRuntime::register_context_message(StringView type, ContextMessageHandler handler)
{
    m_context_message_handlers.set(MUST(String::from_utf8(type)), move(handler));
}

void ExtensionRuntime::page_created(WebContentPage& page)
{
    for (auto& api : m_apis)
        api->page_created(page);
}

void ExtensionRuntime::send(Context const& context, JsonObject const& message)
{
    if (!context.page || !context.page->is_open())
        return;
    context.page->async_deliver_extension_message(context.page_context_id, message.serialized());
}

void ExtensionRuntime::message_received(WebContentPage& page, u64 page_context_id, StringView text)
{
    if (is_private_page(page))
        return;
    auto* context = find_context(page, page_context_id);
    auto unverified = context ? Optional<size_t> {} : find_unverified_context(page, page_context_id);
    if (!context && !unverified.has_value() && page_context_id != 0)
        return;

    auto json = JsonParser::parse(text);
    if (json.is_error() || !json.value().is_object())
        return;
    auto const& message = json.value().as_object();
    auto type = message.get_string("type"sv);
    if (!type.has_value())
        return;

    // Until its frame is verified, a content script's context says nothing but which frame it's in.
    if (unverified.has_value()) {
        if (type == "frame"sv)
            verify_content_script(*unverified, page, message);
        return;
    }

    if (!context) {
        if (auto handler = m_page_message_handlers.find(*type); handler != m_page_message_handlers.end())
            handler->value(page, message);
        else if (tabs_hooks.page_message)
            tabs_hooks.page_message(page, message);
        return;
    }

    // A content script whose document is no longer its frame's active one is gone, whatever its page says.
    if (context->type == "content_script"sv && !content_script_frame(*context))
        return;

    if (auto handler = m_context_message_handlers.find(*type); handler != m_context_message_handlers.end()) {
        handler->value(*context, message);
        return;
    }

    if (type == "frame"sv) {
        // The frame a context is in: only one its page hosts, and only once.
        auto navigable = message.get_object("navigable"sv);
        if (!navigable.has_value() || context->navigable_id.has_value() || !page.is_open())
            return;
        auto namespace_id = navigable->get_u64("ns"sv);
        auto local_id = navigable->get_u64("id"sv);
        if (!namespace_id.has_value() || !local_id.has_value())
            return;
        Web::HTML::CrossProcessId id { .namespace_id = *namespace_id, .local_id = *local_id };
        if (page.hosted_navigable(id).has_value())
            context->navigable_id = id;
    } else if (type == "call"sv) {
        handle_call(*context, message);
    } else if (type == "message_response"sv) {
        message_response_received(*context, message);
    } else if (type == "document_size"sv) {
        auto width = message.get_integer<i32>("width"sv);
        auto height = message.get_integer<i32>("height"sv);
        if (on_document_size && width.has_value() && height.has_value() && *width >= 0 && *height >= 0)
            on_document_size(*context, *width, *height);
    } else if (type == "port_post"sv) {
        post_to_port(*context, message);
    } else if (type == "port_disconnect"sv) {
        if (auto port_id = message.get_string("port_id"sv); port_id.has_value())
            disconnect_port({ context->id, *port_id });
    } else if (type == "listen"sv) {
        auto event = message.get_string("event"sv);
        auto listener = message.get_integer<i64>("listener"sv);
        if (context->type == "content_script"sv && event.has_value() && !is_content_script_event(*event))
            return;
        if (event.has_value() && listener.has_value()) {
            context->listeners.ensure(*event).set(*listener, message.get_array("extra"sv).value_or({}));
            for (auto& api : m_apis)
                api->listeners_changed(context->id, *event);
        }
    } else if (type == "unlisten"sv) {
        auto event = message.get_string("event"sv);
        auto listener = message.get_integer<i64>("listener"sv);
        if (event.has_value() && listener.has_value()) {
            if (auto listeners = context->listeners.find(*event); listeners != context->listeners.end())
                listeners->value.remove(*listener);
            for (auto& api : m_apis)
                api->listeners_changed(context->id, *event);
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

void ExtensionRuntime::call_function(StringView name, Context const& context, JsonArray const& arguments, Reply reply)
{
    auto handler = m_functions.find(name);
    if (handler == m_functions.end())
        return reply(MUST(String::formatted("{} isn't supported", name)));
    handler->value(context, arguments, move(reply));
}

void ExtensionRuntime::dispatch_event(StringView extension_id, StringView event, JsonArray arguments)
{
    // An event while the extension's background is starting waits for it, as Chrome wakes a service worker for one:
    // else the background, which is what listens, would miss it.
    if (background_is_starting(extension_id)) {
        wait_for_background(extension_id, [this, extension_id = MUST(String::from_utf8(extension_id)), event = MUST(String::from_utf8(event)), arguments = move(arguments)]() mutable {
            dispatch_event(extension_id, event, move(arguments));
        });
        return;
    }

    for (auto const& [id, context] : m_contexts) {
        if (context->extension_id != extension_id)
            continue;
        auto listeners = context->listeners.find(event);
        if (listeners == context->listeners.end() || listeners->value.is_empty())
            continue;
        if (context->type == "content_script"sv && !content_script_may_hear(*context, event, arguments))
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
    // A disabled or uninstalled extension's background stops.
    Vector<String> gone;
    for (auto const& [id, background] : m_backgrounds) {
        if (!m_registry.find(id))
            gone.append(id);
    }
    for (auto const& id : gone)
        stop_background(id);

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
    view->on_load_finish = [this, id, events = lifecycle_events(extension)](URL::URL const&) {
        // Listeners are added as the background's script first runs, so they hear of the install once it has loaded.
        auto background = m_backgrounds.find(id);
        if (background == m_backgrounds.end() || background->value.installed_event_sent)
            return;
        background->value.installed_event_sent = true;
        for (auto const& event : events)
            dispatch_event(id, event.get_string("event"sv).value(), JsonArray { Vector<JsonValue> { event.get_object("details"sv).value() } });

        auto waiting = move(background->value.waiting);
        for (auto& callback : waiting)
            callback();
    };
    view->load(*url);
    m_backgrounds.set(id, Background { .version = extension.manifest().version, .view = move(view), .installed_event_sent = false, .waiting = {} });
}

// runtime.onInstalled and onStartup. The version and browser version an extension last started with are kept in
// <storage>/<id>/runtime.json: without them, it's an install; with them, the browser has started again (onStartup),
// and a version that changed gives onInstalled "update" (the extension's) or "chrome_update" (the browser's). A
// background restarting in the same run (reloaded, or enabled again) hears nothing, unless its version changed.
Vector<JsonObject> ExtensionRuntime::lifecycle_events(Extension const& extension)
{
    auto const& version = extension.manifest().version;
    auto browser_version = StringView { BROWSER_VERSION, __builtin_strlen(BROWSER_VERSION) };
    auto event = [](StringView name, JsonObject details) {
        JsonObject event;
        event.set("event"sv, name);
        event.set("details"sv, move(details));
        return event;
    };
    auto installed = [&](StringView reason, Optional<String> previous_version = {}) {
        JsonObject details;
        details.set("reason"sv, reason);
        if (previous_version.has_value())
            details.set("previousVersion"sv, *previous_version);
        return event("runtime.onInstalled"sv, move(details));
    };

    Vector<JsonObject> events;
    if (auto started = m_started_versions.get(extension.id()); started.has_value()) {
        if (*started != version)
            events.append(installed("update"sv, *started));
    } else {
        Optional<JsonObject> state;
        if (m_storage_directory.has_value()) {
            auto path = LexicalPath::join(*m_storage_directory, extension.id(), "runtime.json"sv).string();
            if (auto file = Core::File::open(path, Core::File::OpenMode::Read); !file.is_error()) {
                if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
                    if (auto json = JsonParser::parse(contents.value()); !json.is_error() && json.value().is_object())
                        state = json.value().as_object();
                }
            }
        }
        auto previous_version = state.has_value() ? state->get_string("version"sv) : Optional<String> {};
        if (!previous_version.has_value()) {
            events.append(installed("install"sv));
        } else {
            if (*previous_version != version)
                events.append(installed("update"sv, *previous_version));
            else if (state->get_string("browser_version"sv) != browser_version)
                events.append(installed("chrome_update"sv));
            events.append(event("runtime.onStartup"sv, {}));
        }
    }
    m_started_versions.set(extension.id(), version);

    if (m_storage_directory.has_value()) {
        auto directory = LexicalPath::join(*m_storage_directory, extension.id()).string();
        JsonObject state;
        state.set("version"sv, version);
        state.set("browser_version"sv, browser_version);
        auto saved = [&]() -> ErrorOr<void> {
            TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
            auto file = TRY(Core::File::open(LexicalPath::join(directory, "runtime.json"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
            auto text = state.serialized();
            TRY(file->write_until_depleted(text.bytes()));
            return {};
        }();
        if (saved.is_error())
            dbgln("Extensions: Can't save {}'s runtime state: {}", extension.id(), saved.error());
    }
    return events;
}

void ExtensionRuntime::stop_background(StringView extension_id)
{
    // The background may be what's calling: it goes once the call is done.
    auto background = m_backgrounds.take(extension_id);
    if (!background.has_value() || !background->view)
        return;
    background->view->on_load_finish = nullptr;
    Core::deferred_invoke([view = background->view.release_nonnull()] { (void)view; });
}

void ExtensionRuntime::restart_background(StringView extension_id)
{
    stop_background(extension_id);
    auto const* extension = m_registry.find(extension_id);
    if (extension)
        start_background(*extension);
}

void ExtensionRuntime::reload_extension(StringView extension_id)
{
    Core::deferred_invoke([this, id = MUST(String::from_utf8(extension_id))] {
        Vector<ViewImplementation*> views;
        for (auto const& [context_id, context] : m_contexts) {
            if (context->extension_id != id || context->type == "content_script"sv || context->type == "background"sv || !context->is_top_level)
                continue;
            if (!context->page || !context->page->is_open() || is_background_page(*context->page))
                continue;
            if (!views.contains_slow(&context->page->view()))
                views.append(&context->page->view());
        }
        restart_background(id);
        for (auto* view : views)
            view->reload();
    });
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
    // A background that never finishes starting doesn't gather what waits for it without end.
    static constexpr size_t maximum_waiting = 1000;
    if (background->value.waiting.size() >= maximum_waiting)
        return;
    background->value.waiting.append(move(callback));
}

ViewImplementation* ExtensionRuntime::background_view(StringView extension_id) const
{
    auto background = m_backgrounds.find(extension_id);
    return background == m_backgrounds.end() ? nullptr : background->value.view.ptr();
}

static Function<bool()>& user_activation_for_testing()
{
    static auto* source = new Function<bool()>;
    return *source;
}

void set_user_activation_for_testing(Function<bool()> source)
{
    user_activation_for_testing() = move(source);
}

bool ExtensionRuntime::consume_user_activation(Context const& context)
{
    if (context.type != "page"sv && context.type != "popup"sv)
        return false;
    if (auto& source = user_activation_for_testing())
        return source();
    if (!context.page || !context.page->is_open())
        return false;
    return context.page->consume_transient_user_activation();
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
    // A content script's document's URL, as the browser knows it now (it may have changed with the history API).
    auto url = context.url;
    if (auto* frame = context.type == "content_script"sv ? content_script_frame(context) : nullptr)
        url = frame->active_document_url();
    sender.set("url"sv, url.serialize());
    sender.set("origin"sv, url.origin().serialize());
    if (m_tabs_delegate && tabs_hooks.describe_sender) {
        tabs_hooks.describe_sender(context, sender);
    } else if (context.page && !is_background_page(*context.page)) {
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

void ExtensionRuntime::set_tabs_delegate(ExtensionTabsDelegate* delegate)
{
    m_tabs_delegate = delegate;
    tabs_changed();
}

void ExtensionRuntime::tabs_changed()
{
    if (tabs_hooks.tabs_changed)
        tabs_hooks.tabs_changed();
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

        deliver_message(caller, move(receivers), message, move(reply));
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

void ExtensionRuntime::deliver_message(Context const& caller, Vector<Context const*> receivers, JsonValue const& message, Reply reply)
{
    if (receivers.is_empty())
        return reply(MUST(String::from_utf8(no_receiver_error)));
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

    if (channel.browser_end) {
        // It may close the port as it goes; nothing of the channel is touched after.
        channel.browser_end->browser_port_message(*channel_id, message.get("message"sv).value_or(JsonValue {}));
        return;
    }

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
        if (auto* browser_end = channel.browser_end) {
            m_channels.remove(*channel_id);
            browser_end->browser_port_disconnected(*channel_id);
            return;
        }
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

Optional<u64> ExtensionRuntime::open_browser_port(Context const& opener, String opener_port_id, BrowserPortEnd& browser_end)
{
    PortEnd opener_end { opener.id, move(opener_port_id) };
    if (channel_of(opener_end).has_value())
        return {};
    auto channel_id = m_next_channel_id++;
    m_channels.set(channel_id, Channel { .opener = move(opener_end), .receivers = {}, .browser_end = &browser_end });
    return channel_id;
}

void ExtensionRuntime::post_from_browser_port(u64 port, JsonValue const& message)
{
    auto channel = m_channels.find(port);
    if (channel == m_channels.end() || !channel->value.browser_end)
        return;
    auto const* context = find_context(channel->value.opener.context_id);
    if (!context)
        return;
    JsonObject delivery;
    delivery.set("type"sv, "port_message"sv);
    delivery.set("port_id"sv, channel->value.opener.port_id);
    delivery.set("message"sv, message);
    send(*context, delivery);
}

// The browser's end closing: the context's end hears of it, with the error if there is one.
void ExtensionRuntime::close_browser_port(u64 port, Optional<String> error)
{
    auto channel = m_channels.find(port);
    if (channel == m_channels.end() || !channel->value.browser_end)
        return;
    auto opener = channel->value.opener;
    m_channels.remove(channel);
    send_port_disconnected(opener, move(error));
}

void ExtensionRuntime::set_native_host_directories(Vector<ByteString> directories)
{
    m_native_host_directories = move(directories);
}

Vector<ByteString> ExtensionRuntime::native_host_directories() const
{
    if (m_native_host_directories.has_value())
        return *m_native_host_directories;
    // The browser's own directory first, then Chrome's (where hosts such as 1Password's install their manifests).
    auto own = LexicalPath::join(Core::StandardPaths::user_data_directory(), "Ladybird"sv, "NativeMessagingHosts"sv).string();
    return WebExtensions::default_native_host_directories(move(own));
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
