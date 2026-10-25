/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <LibCore/Resource.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Script.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ExtensionBridge.h>
#include <LibWeb/Extensions/ExtensionContexts.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <LibWebExtensions/MatchPattern.h>

namespace Web::Extensions {

namespace {

enum class RunAt : u8 {
    DocumentStart,
    DocumentEnd,
    DocumentIdle,
};

struct ScriptSource {
    String url;
    String source;
};

struct ContentScript {
    WebExtensions::MatchPatternSet matches;
    WebExtensions::MatchPatternSet exclude_matches;
    Vector<String> include_globs;
    Vector<String> exclude_globs;
    RunAt run_at { RunAt::DocumentIdle };
    bool all_frames { false };
    bool match_about_blank { false };
    bool match_origin_as_fallback { false };
    bool main_world { false };
    Vector<ScriptSource> js;
    Vector<ScriptSource> css;
};

struct InstalledExtension {
    String id;
    JsonValue manifest;
    JsonValue messages;
    String ui_locale;
    bool allow_file_access { false };
    Vector<ContentScript> content_scripts;
};

struct Context {
    GC::Root<ExtensionBridge> bridge;
    GC::Weak<DOM::Document> document;
    String extension_id;
    // Content scripts' worlds; extension pages use their own, main, world.
    GC::Root<Bindings::WrapperWorld> world;
    GC::Root<JS::Realm> realm;
    OwnPtr<JS::ExecutionContext> execution_context;
};

}

// Never destroyed: contexts hold GC roots, which must not outlive the heap.
static Vector<InstalledExtension>& s_extensions = *new Vector<InstalledExtension>;
static HashMap<u64, Context>& s_contexts = *new HashMap<u64, Context>;
static u64 s_next_context_id = 1;

static Vector<String> strings_in(JsonObject const& object, StringView key)
{
    Vector<String> strings;
    if (auto array = object.get_array(key); array.has_value()) {
        array->for_each([&](JsonValue const& value) {
            if (value.is_string())
                strings.append(value.as_string());
        });
    }
    return strings;
}

static Vector<ScriptSource> sources_in(JsonObject const& object, StringView key)
{
    Vector<ScriptSource> sources;
    if (auto array = object.get_array(key); array.has_value()) {
        array->for_each([&](JsonValue const& value) {
            if (!value.is_object())
                return;
            auto url = value.as_object().get_string("url"sv);
            auto source = value.as_object().get_string("source"sv);
            if (url.has_value() && source.has_value())
                sources.append({ *url, *source });
        });
    }
    return sources;
}

static WebExtensions::MatchPatternSet patterns_in(JsonObject const& object, StringView key)
{
    WebExtensions::MatchPatternSet set;
    for (auto const& string : strings_in(object, key)) {
        if (auto pattern = WebExtensions::MatchPattern::parse(string); !pattern.is_error())
            set.add(pattern.release_value());
    }
    return set;
}

void set_installed_extensions(StringView json)
{
    auto parsed = JsonParser::parse(json);
    if (parsed.is_error() || !parsed.value().is_object()) {
        dbgln("Extensions: The browser's list of extensions isn't valid JSON");
        return;
    }

    Vector<InstalledExtension> extensions;
    if (auto list = parsed.value().as_object().get_array("extensions"sv); list.has_value()) {
        list->for_each([&](JsonValue const& value) {
            if (!value.is_object())
                return;
            auto const& object = value.as_object();
            auto id = object.get_string("id"sv);
            if (!id.has_value())
                return;

            InstalledExtension extension;
            extension.id = *id;
            extension.manifest = object.get("manifest"sv).value_or(JsonObject {});
            extension.messages = object.get("messages"sv).value_or(JsonObject {});
            extension.ui_locale = object.get_string("uiLocale"sv).value_or("en"_string);
            extension.allow_file_access = object.get_bool("allowFileAccess"sv).value_or(false);

            if (auto scripts = object.get_array("contentScripts"sv); scripts.has_value()) {
                scripts->for_each([&](JsonValue const& script_value) {
                    if (!script_value.is_object())
                        return;
                    auto const& script = script_value.as_object();
                    ContentScript content_script;
                    content_script.matches = patterns_in(script, "matches"sv);
                    content_script.exclude_matches = patterns_in(script, "excludeMatches"sv);
                    content_script.include_globs = strings_in(script, "includeGlobs"sv);
                    content_script.exclude_globs = strings_in(script, "excludeGlobs"sv);
                    auto run_at = script.get_string("runAt"sv).value_or("document_idle"_string);
                    content_script.run_at = run_at == "document_start"sv ? RunAt::DocumentStart : run_at == "document_end"sv ? RunAt::DocumentEnd : RunAt::DocumentIdle;
                    content_script.all_frames = script.get_bool("allFrames"sv).value_or(false);
                    content_script.match_about_blank = script.get_bool("matchAboutBlank"sv).value_or(false);
                    content_script.match_origin_as_fallback = script.get_bool("matchOriginAsFallback"sv).value_or(false);
                    content_script.main_world = script.get_string("world"sv) == "MAIN"sv;
                    content_script.js = sources_in(script, "js"sv);
                    content_script.css = sources_in(script, "css"sv);
                    extension.content_scripts.append(move(content_script));
                });
            }
            extensions.append(move(extension));
        });
    }
    s_extensions = move(extensions);
}

static InstalledExtension const* find_extension(StringView id)
{
    for (auto const& extension : s_extensions) {
        if (extension.id == id)
            return &extension;
    }
    return nullptr;
}

static String const& api_source()
{
    static String& source = *new String([] {
        auto resource = Core::Resource::load_from_uri("resource://ladybird/extensions/chrome-api.js"sv);
        if (resource.is_error()) {
            dbgln("Extensions: Can't load the chrome.* API: {}", resource.error());
            return String {};
        }
        return MUST(String::from_utf8(resource.value()->data()));
    }());
    return source;
}

// Runs a script in a realm: the page's own, or a content script's world.
static void run_script(JS::Realm& realm, JS::ExecutionContext* world_execution_context, StringView source, StringView filename)
{
    auto& vm = realm.vm();
    HTML::TemporaryExecutionContext temporary_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    if (world_execution_context)
        vm.push_execution_context(*world_execution_context);

    auto script = JS::Script::parse(Utf16String::from_utf8(source), realm, filename);
    if (script.is_error()) {
        dbgln("Extensions: {} doesn't parse: {}", filename, script.error().first().to_utf16_string());
    } else {
        auto result = vm.run(script.value());
        if (result.is_throw_completion())
            HTML::report_exception(result, realm);
    }

    if (world_execution_context)
        vm.pop_execution_context();
}

static String context_info(InstalledExtension const& extension, u64 context_id, StringView context_type)
{
    JsonObject info;
    info.set("extensionId"sv, extension.id);
    info.set("contextId"sv, context_id);
    info.set("contextType"sv, context_type);
    info.set("manifest"sv, extension.manifest);
    info.set("messages"sv, extension.messages);
    info.set("uiLocale"sv, extension.ui_locale);
    return info.serialized();
}

static bool is_top_level(DOM::Document const& document)
{
    auto navigable = document.navigable();
    return navigable && navigable->is_top_level_traversable();
}

// Gives a realm the chrome.* API, through a new bridge, and tells the browser of the context.
static void start_context(DOM::Document& document, InstalledExtension const& extension, StringView context_type, JS::Realm& realm, Context context)
{
    auto window = document.window();
    auto context_id = s_next_context_id++;
    bool is_content_script = context_type == "content_script"sv;
    auto bridge = ExtensionBridge::create(*window, context_id, is_content_script, Utf16String::from_utf8(context_info(extension, context_id, context_type)));

    realm.global_object().define_direct_property("__extensionBridge"_utf16_fly_string, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, bridge), JS::Attribute::Configurable | JS::Attribute::Writable);

    context.bridge = GC::make_root(bridge);
    context.document = document;
    context.extension_id = extension.id;
    auto* execution_context = context.execution_context.ptr();
    s_contexts.set(context_id, move(context));

    document.page().client().page_did_create_extension_context(context_id, extension.id, MUST(String::from_utf8(context_type)), document.url(), is_top_level(document));
    run_script(realm, execution_context, api_source(), "chrome-api.js"sv);
}

static Context* find_content_script_context(DOM::Document& document, StringView extension_id)
{
    for (auto& [id, context] : s_contexts) {
        if (context.world && context.document.ptr().ptr() == &document && context.extension_id == extension_id)
            return &context;
    }
    return nullptr;
}

// The extension's isolated world in this document, made when its first content script runs.
static Context* content_script_context(DOM::Document& document, InstalledExtension const& extension)
{
    if (auto* context = find_content_script_context(document, extension.id))
        return context;

    auto window = document.window();
    auto browsing_context = document.browsing_context();
    if (!window || !browsing_context)
        return nullptr;

    auto& vm = Bindings::main_thread_vm();
    auto world = vm.heap().allocate<Bindings::WrapperWorld>(Bindings::WrapperWorld::Type::Extension);
    auto execution_context = Bindings::create_a_new_javascript_realm(
        vm,
        HTML::CrossOriginIsolationMode::None,
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return Bindings::create_global_object_wrapper(realm, GC::Ref { *window });
        },
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return browsing_context->window_proxy_for(*world, realm)->object();
        });
    auto& realm = *execution_context->realm;
    auto intrinsics = realm.create<Bindings::Intrinsics>(realm);
    realm.set_host_defined(realm.heap().allocate<Bindings::HostDefined>(intrinsics, *world, window->principal_realm()));
    Bindings::cache_global_object_wrapper(realm);
    if (Bindings::initialize_window_web_interfaces(*window, realm).is_error()) {
        world->detach();
        return nullptr;
    }

    Context context;
    context.world = GC::make_root(world);
    context.realm = GC::make_root(realm);
    context.execution_context = move(execution_context);
    start_context(document, extension, "content_script"sv, realm, move(context));
    return find_content_script_context(document, extension.id);
}

// The URL a document's content scripts are matched with, or nothing when none may run in it.
static Optional<URL::URL> url_to_match(DOM::Document const& document, ContentScript const& script, InstalledExtension const& extension)
{
    Optional<URL::URL> creator_url;
    // FIXME: Use the creator (or opener) when that's not the parent, and the precursor origin when the parent is in
    //        another process.
    if (auto parent_document = document.container_document())
        creator_url = parent_document->url();
    auto url = WebExtensions::url_for_matching(document.url(), creator_url, script.match_about_blank, script.match_origin_as_fallback);
    if (!url.has_value() || WebExtensions::is_restricted_url(*url, extension.allow_file_access, extension.id.bytes_as_string_view()))
        return {};
    return url;
}

static void run_content_scripts(DOM::Document& document, RunAt run_at)
{
    if (s_extensions.is_empty() || !document.window() || !document.browsing_context())
        return;

    for (auto const& extension : s_extensions) {
        for (auto const& script : extension.content_scripts) {
            if (script.run_at != run_at || (!script.all_frames && !is_top_level(document)))
                continue;
            auto url = url_to_match(document, script, extension);
            if (!url.has_value() || !WebExtensions::should_inject_content_script(*url, script.matches, script.exclude_matches, script.include_globs, script.exclude_globs))
                continue;

            if (script.main_world) {
                auto& realm = document.window()->principal_realm();
                for (auto const& js : script.js)
                    run_script(realm, nullptr, js.source, js.url);
                continue;
            }

            auto* context = content_script_context(document, extension);
            if (!context)
                continue;
            for (auto const& js : script.js)
                run_script(*context->realm, context->execution_context.ptr(), js.source, js.url);
        }
    }
}

void document_is_ready_to_run_scripts(DOM::Document& document)
{
    auto const& url = document.url();
    if (url.scheme() == "chrome-extension"sv && document.window()) {
        if (auto const* extension = find_extension(url.serialized_host())) {
            auto context_type = url.serialize_path() == "/_generated_background_page.html"sv ? "background"sv : "page"sv;
            start_context(document, *extension, context_type, document.window()->principal_realm(), {});
        }
    }
    run_content_scripts(document, RunAt::DocumentStart);
}

void document_readiness_changed(DOM::Document& document, HTML::DocumentReadyState readiness)
{
    if (readiness == HTML::DocumentReadyState::Interactive)
        run_content_scripts(document, RunAt::DocumentEnd);
    else if (readiness == HTML::DocumentReadyState::Complete)
        run_content_scripts(document, RunAt::DocumentIdle);
}

void document_is_going_away(DOM::Document& document)
{
    Vector<u64> ended;
    for (auto& [id, context] : s_contexts) {
        if (context.document.ptr().ptr() == &document || !context.document)
            ended.append(id);
    }
    for (auto id : ended) {
        auto context = s_contexts.take(id).release_value();
        context.bridge->close();
        if (context.world)
            context.world->detach();
        document.page().client().page_did_destroy_extension_context(id);
    }
}

void deliver_message(u64 context_id, String const& message)
{
    auto it = s_contexts.find(context_id);
    if (it == s_contexts.end() || !it->value.document)
        return;
    auto document = it->value.document;
    auto bridge = GC::Ref { *it->value.bridge };
    auto window = document->window();
    if (!window)
        return;
    HTML::queue_global_task(HTML::Task::Source::PostedMessage, HTML::relevant_global_object(*document), GC::create_function(GC::Heap::the(), [bridge, message = Utf16String::from_utf8(message)] {
        bridge->deliver(message);
    }));
}

}
