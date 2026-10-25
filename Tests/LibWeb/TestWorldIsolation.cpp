/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// What a script stores on a DOM object (an event's detail, an event handler) doesn't hand its world's objects to
// another world, which could reach the other world's globals through them.

#include <LibCore/AnonymousBuffer.h>
#include <LibCore/EventLoop.h>
#include <LibGC/CellAllocator.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace {

class TestPageClient final : public Web::PageClient {
    GC_CELL(TestPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(TestPageClient);

public:
    TestPageClient()
        : m_palette(Gfx::PaletteImpl::create_with_anonymous_buffer(MUST(Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme)))))
    {
    }

    virtual Web::PageId id() const override { return Web::PageId { 1 }; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { return m_palette; }
    virtual Web::DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1; }
    virtual double device_pixel_ratio() const override { return 1; }
    virtual double device_pixels_per_css_pixel() const override { return 1; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Auto; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::Auto; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::NoPreference; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { VERIFY_NOT_REACHED(); }
    virtual void report_finished_handling_input_event(Web::PageId, u64, Web::EventResult) override { }
    virtual Web::HTML::CrossProcessId allocate_cross_process_id() override { return { 1, m_next_cross_process_id++ }; }
    virtual void request_frame() override { }
    virtual void request_file(Web::FileRequest) override { }
    virtual bool is_headless() const override { return true; }
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    GC::Ptr<Web::Page> m_page;
    u64 m_next_cross_process_id { 1 };
    Gfx::Palette m_palette;
};

GC_DEFINE_ALLOCATOR(TestPageClient);

// A document with its main world, and an extension's isolated world in it, as content scripts have.
struct Worlds {
    AK_ALLOC_WITH_KMALLOC;

    GC::Root<Web::HTML::Window> window;
    GC::Root<Web::Bindings::WrapperWorld> extension_world;
    JS::Realm* extension_realm { nullptr };
    NonnullOwnPtr<JS::ExecutionContext> extension_execution_context;

    JS::Realm& main_realm() { return window->principal_realm(); }

    JS::Value run(JS::Realm& realm, Utf16View source)
    {
        auto& vm = Web::Bindings::main_thread_vm();
        bool is_extension = &realm == extension_realm;
        if (is_extension)
            vm.push_execution_context(*extension_execution_context);
        auto script_or_error = JS::Script::parse(source, realm);
        VERIFY(!script_or_error.is_error());
        auto result = vm.run(script_or_error.value());
        if (is_extension)
            vm.pop_execution_context();
        if (result.is_throw_completion())
            warnln("Script threw: {}", result.error_value().to_utf16_string_without_side_effects());
        VERIFY(!result.is_throw_completion());
        return result.release_value();
    }

    ByteString string(JS::Realm& realm, Utf16View source)
    {
        return run(realm, source).to_utf16_string_without_side_effects().to_byte_string();
    }
};

Worlds& worlds()
{
    static OwnPtr<Worlds> s_worlds;
    if (s_worlds)
        return *s_worlds;

    static Core::EventLoop event_loop;
    static Web::Platform::FontPlugin font_plugin(false);
    Web::Platform::FontPlugin::install(font_plugin);

    auto principal_realm = Web::Bindings::create_a_principal_javascript_realm();
    VERIFY(principal_realm.ptr());
    auto& vm = Web::Bindings::main_thread_vm();

    auto client = vm.heap().allocate<TestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();

    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto document = GC::Ref { *traversable->active_document() };
    auto browsing_context = GC::Ref { *document->browsing_context() };
    auto window = document->window();

    auto extension_world = vm.heap().allocate<Web::Bindings::WrapperWorld>(Web::Bindings::WrapperWorld::Type::Extension);
    auto extension_execution_context = Web::Bindings::create_a_new_javascript_realm(
        vm,
        Web::HTML::CrossOriginIsolationMode::None,
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return Web::Bindings::create_global_object_wrapper(realm, GC::Ref { *window });
        },
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return browsing_context->window_proxy_for(*extension_world, realm)->object();
        });
    auto& extension_realm = *extension_execution_context->realm;
    auto intrinsics = extension_realm.create<Web::Bindings::Intrinsics>(extension_realm);
    extension_realm.set_host_defined(extension_realm.heap().allocate<Web::Bindings::HostDefined>(intrinsics, *extension_world, window->principal_realm()));
    Web::Bindings::cache_global_object_wrapper(extension_realm);
    MUST(Web::Bindings::initialize_window_web_interfaces(*window, extension_realm));

    s_worlds = make<Worlds>(Worlds {
        .window = GC::make_root(*window),
        .extension_world = GC::make_root(*extension_world),
        .extension_realm = &extension_realm,
        .extension_execution_context = move(extension_execution_context),
    });

    // Each world tells whether a value is its own, and whether it leads to the other world's globals.
    auto helpers = u"globalThis.kindOf = v => (v === null || v === undefined) ? String(v) : (typeof v !== 'object' && typeof v !== 'function') ? typeof v : (v instanceof Object ? 'own' : 'foreign');"
                   u"globalThis.reaches = v => { try { return String(v.constructor.constructor('return marker')()); } catch (e) { return 'threw'; } };"sv;
    s_worlds->run(s_worlds->main_realm(), helpers);
    s_worlds->run(extension_realm, helpers);
    s_worlds->run(s_worlds->main_realm(), u"globalThis.marker = 'main';"sv);
    s_worlds->run(extension_realm, u"globalThis.marker = 'extension';"sv);
    return *s_worlds;
}

}

TEST_CASE(custom_event_detail_is_cloned_for_other_worlds)
{
    auto& w = worlds();
    auto& main = w.main_realm();
    auto& extension = *w.extension_realm;

    w.run(main, u"document.addEventListener('ext', e => { globalThis.got = e.detail; globalThis.gotTwice = e.detail; });"sv);
    w.run(extension, u"globalThis.sent = { nested: { value: 42 }, list: [1, 2] }; globalThis.ev = new CustomEvent('ext', { detail: sent }); document.dispatchEvent(ev);"sv);
    EXPECT_EQ(w.string(main, u"kindOf(got)"sv), "own"sv);
    EXPECT_EQ(w.string(main, u"got.nested.value + got.list.length"sv), "44"sv);
    EXPECT_EQ(w.string(main, u"reaches(got)"sv), "main"sv);
    EXPECT_EQ(w.string(main, u"reaches(got.list)"sv), "main"sv);
    // The world that made the event still sees its own object.
    EXPECT_EQ(w.string(extension, u"String(ev.detail === sent)"sv), "true"sv);

    // A page's object reaches the extension's world as a copy too.
    w.run(extension, u"document.addEventListener('page', e => { globalThis.fromPage = e.detail; });"sv);
    w.run(main, u"document.dispatchEvent(new CustomEvent('page', { detail: { value: 7 } }));"sv);
    EXPECT_EQ(w.string(extension, u"kindOf(fromPage)"sv), "own"sv);
    EXPECT_EQ(w.string(extension, u"reaches(fromPage)"sv), "extension"sv);
    EXPECT_EQ(w.string(extension, u"String(fromPage.value)"sv), "7"sv);

    // What can't be cloned is null in the other world.
    w.run(main, u"document.dispatchEvent(new CustomEvent('page', { detail: { fn() {} } }));"sv);
    EXPECT_EQ(w.string(extension, u"kindOf(fromPage)"sv), "null"sv);
    w.run(main, u"document.dispatchEvent(new CustomEvent('page', { detail: () => 1 }));"sv);
    EXPECT_EQ(w.string(extension, u"kindOf(fromPage)"sv), "null"sv);

    // Primitives cross as they are; a node as the world's own wrapper of it.
    w.run(main, u"document.dispatchEvent(new CustomEvent('page', { detail: 'text' }));"sv);
    EXPECT_EQ(w.string(extension, u"fromPage"sv), "text"sv);
    w.run(main, u"document.dispatchEvent(new CustomEvent('page', { detail: document }));"sv);
    EXPECT_EQ(w.string(extension, u"kindOf(fromPage)"sv), "own"sv);
    EXPECT_EQ(w.string(extension, u"String(fromPage === document)"sv), "true"sv);
    w.run(main, u"document.dispatchEvent(new CustomEvent('page', { detail: window }));"sv);
    EXPECT_EQ(w.string(extension, u"String(fromPage === window)"sv), "true"sv);

    // initCustomEvent's detail too.
    w.run(extension, u"globalThis.ev2 = document.createEvent('CustomEvent'); ev2.initCustomEvent('ext', false, false, { a: 1 }); document.dispatchEvent(ev2);"sv);
    EXPECT_EQ(w.string(main, u"kindOf(got) + reaches(got)"sv), "ownmain"sv);
}

TEST_CASE(other_any_attributes_are_cloned_for_other_worlds)
{
    auto& w = worlds();
    auto& main = w.main_realm();
    auto& extension = *w.extension_realm;

    // A MessageEvent made in one world and read in another.
    w.run(main, u"window.addEventListener('message-test', e => { globalThis.data = e.data; });"sv);
    w.run(extension, u"window.dispatchEvent(new MessageEvent('message-test', { data: { a: [1] } }));"sv);
    EXPECT_EQ(w.string(main, u"kindOf(data) + reaches(data) + reaches(data.a)"sv), "ownmainmain"sv);

    // An abort reason.
    w.run(extension, u"globalThis.controller = new AbortController(); controller.abort({ why: 1 }); document.dispatchEvent(new CustomEvent('ext', { detail: controller.signal }));"sv);
    EXPECT_EQ(w.string(main, u"kindOf(got) + (got instanceof AbortSignal)"sv), "owntrue"sv);
    EXPECT_EQ(w.string(main, u"kindOf(got.reason) + reaches(got.reason) + got.reason.why"sv), "ownmain1"sv);

    // ErrorEvent.error and PromiseRejectionEvent.reason/promise.
    w.run(main, u"window.addEventListener('error-test', e => { globalThis.error = e.error; });"sv);
    w.run(extension, u"window.dispatchEvent(new ErrorEvent('error-test', { error: new TypeError('x') }));"sv);
    EXPECT_EQ(w.string(main, u"kindOf(error) + reaches(error) + (error instanceof TypeError)"sv), "ownmaintrue"sv);
    w.run(main, u"window.addEventListener('rejection-test', e => { globalThis.reason = e.reason; globalThis.promise = e.promise; });"sv);
    w.run(extension, u"const p = Promise.reject({ r: 1 }); p.catch(() => {}); window.dispatchEvent(new PromiseRejectionEvent('rejection-test', { promise: p, reason: { r: 1 } }));"sv);
    EXPECT_EQ(w.string(main, u"kindOf(reason) + reaches(reason)"sv), "ownmain"sv);
    // NB: Our structured serialization doesn't reject objects with internal slots yet (a promise clones as a plain
    //     object), but the copy is the reader's own all the same.
    EXPECT_EQ(w.string(main, u"reaches(promise)"sv), "main"sv);
}

TEST_CASE(event_handler_attributes_are_per_world)
{
    auto& w = worlds();
    auto& main = w.main_realm();
    auto& extension = *w.extension_realm;

    w.run(main, u"globalThis.mainCalls = 0; globalThis.mainHandler = () => { mainCalls++; }; document.onclick = mainHandler;"sv);
    // The extension's world doesn't see the page's handler...
    EXPECT_EQ(w.string(extension, u"kindOf(document.onclick)"sv), "null"sv);

    // ...and setting its own leaves the page's alone, and hidden from the page.
    w.run(extension, u"globalThis.extensionCalls = 0; globalThis.extensionHandler = () => { extensionCalls++; }; document.onclick = extensionHandler;"sv);
    EXPECT_EQ(w.string(main, u"String(document.onclick === mainHandler)"sv), "true"sv);
    EXPECT_EQ(w.string(extension, u"String(document.onclick === extensionHandler)"sv), "true"sv);

    // Both run.
    w.run(main, u"document.dispatchEvent(new Event('click'));"sv);
    EXPECT_EQ(w.string(main, u"String(mainCalls)"sv), "1"sv);
    EXPECT_EQ(w.string(extension, u"String(extensionCalls)"sv), "1"sv);

    // Replacing or clearing one world's handler doesn't touch the other's.
    w.run(extension, u"globalThis.extensionHandler2 = () => { extensionCalls += 10; }; document.onclick = extensionHandler2;"sv);
    w.run(main, u"document.onclick = null;"sv);
    EXPECT_EQ(w.string(extension, u"String(document.onclick === extensionHandler2)"sv), "true"sv);
    EXPECT_EQ(w.string(main, u"kindOf(document.onclick)"sv), "null"sv);
    w.run(main, u"document.dispatchEvent(new Event('click'));"sv);
    EXPECT_EQ(w.string(main, u"String(mainCalls)"sv), "1"sv);
    EXPECT_EQ(w.string(extension, u"String(extensionCalls)"sv), "11"sv);

    w.run(extension, u"document.onclick = null;"sv);
    EXPECT_EQ(w.string(extension, u"kindOf(document.onclick)"sv), "null"sv);
    w.run(main, u"document.dispatchEvent(new Event('click'));"sv);
    EXPECT_EQ(w.string(extension, u"String(extensionCalls)"sv), "11"sv);

    // Window handlers, and the body's that forward to the window, are per world too.
    w.run(extension, u"window.onmessage = () => {};"sv);
    EXPECT_EQ(w.string(main, u"kindOf(window.onmessage)"sv), "null"sv);
    // A content attribute's handler is the page's.
    w.run(main, u"document.documentElement.setAttribute('onclick', 'mainCalls += 100');"sv);
    EXPECT_EQ(w.string(extension, u"kindOf(document.documentElement.onclick)"sv), "null"sv);
    EXPECT_EQ(w.string(main, u"typeof document.documentElement.onclick"sv), "function"sv);
}

TEST_CASE(adopted_style_sheets_are_each_worlds_own)
{
    auto& w = worlds();
    auto& main = w.main_realm();
    auto& extension = *w.extension_realm;

    w.run(main, u"globalThis.mainSheet = new CSSStyleSheet(); document.adoptedStyleSheets = [mainSheet];"sv);
    w.run(extension, u"globalThis.sheet = new CSSStyleSheet(); document.adoptedStyleSheets = [...document.adoptedStyleSheets, sheet];"sv);

    // The page sees its own wrappers of both sheets.
    EXPECT_EQ(w.string(main, u"String(document.adoptedStyleSheets.length)"sv), "2"sv);
    EXPECT_EQ(w.string(main, u"String(document.adoptedStyleSheets[0] === mainSheet)"sv), "true"sv);
    EXPECT_EQ(w.string(main, u"kindOf(document.adoptedStyleSheets[1]) + reaches(document.adoptedStyleSheets[1])"sv), "ownmain"sv);
    EXPECT_EQ(w.string(main, u"reaches(document.adoptedStyleSheets)"sv), "main"sv);

    // And the extension's world its own, so that it can find (and take away) what it added.
    EXPECT_EQ(w.string(extension, u"kindOf(document.adoptedStyleSheets) + reaches(document.adoptedStyleSheets)"sv), "ownextension"sv);
    EXPECT_EQ(w.string(extension, u"String(document.adoptedStyleSheets[1] === sheet)"sv), "true"sv);
    EXPECT_EQ(w.string(extension, u"kindOf(document.adoptedStyleSheets[0]) + reaches(document.adoptedStyleSheets[0])"sv), "ownextension"sv);
    w.run(extension, u"document.adoptedStyleSheets = document.adoptedStyleSheets.filter(candidate => candidate !== sheet);"sv);
    EXPECT_EQ(w.string(main, u"String(document.adoptedStyleSheets.length === 1 && document.adoptedStyleSheets[0] === mainSheet)"sv), "true"sv);
}

TEST_CASE(custom_elements_are_the_main_worlds)
{
    auto& w = worlds();
    auto& main = w.main_realm();
    auto& extension = *w.extension_realm;

    // As in Chrome, an isolated world has no customElements, and can't define one through the registry otherwise.
    EXPECT_EQ(w.string(extension, u"kindOf(window.customElements)"sv), "null"sv);
    w.run(main, u"globalThis.registry = customElements; document.dispatchEvent(new CustomEvent('page', { detail: customElements }));"sv);
    EXPECT_EQ(w.string(extension, u"kindOf(fromPage)"sv), "own"sv);
    EXPECT_EQ(w.string(extension, u"(() => { try { fromPage.define('x-isolated', class extends HTMLElement {}); return 'defined'; } catch (e) { return e.name; } })()"sv), "NotSupportedError"sv);
    EXPECT_EQ(w.string(main, u"kindOf(customElements.get('x-isolated'))"sv), "undefined"sv);

    // The page's own still work.
    w.run(main, u"customElements.define('x-page', class extends HTMLElement {});"sv);
    EXPECT_EQ(w.string(main, u"typeof customElements.get('x-page')"sv), "function"sv);
}

// What the engine creates for a world's script (a callback's arguments, a promise it returns, what it throws) is that
// world's: a page that replaced the builtins of its own world mustn't be handed the content script's callbacks by them.
static void replace_main_world_builtins(Worlds& w)
{
    w.run(w.main_realm(), u"{ globalThis.stolen = [];"
                          u"const steal = name => function (...args) { for (const a of args) if (typeof a === 'function') stolen.push(name + ':' + reaches(a)); return undefined; };"
                          u"Array.prototype.forEach = steal('Array.forEach');"
                          u"NodeList.prototype.forEach = steal('NodeList.forEach');"
                          u"Promise.prototype.then = steal('Promise.then');"
                          u"EventTarget.prototype.addEventListener = steal('addEventListener');"
                          u"for (const name of ['value', 'done', 'target', 'detail', 'data', 'result', 'then']) Object.defineProperty(Object.prototype, name, { __proto__: null, set(v) { stolen.push('setter:' + name + ':' + reaches(v)); }, configurable: true }); }"sv);
}

static void restore_main_world_builtins(Worlds& w)
{
    w.run(w.main_realm(), u"for (const name of ['value', 'done', 'target', 'detail', 'data', 'result', 'then']) delete Object.prototype[name];"sv);
}

static void checkpoint()
{
    // A checkpoint runs with no script on the stack, which the realms' set-up leaves there.
    auto& vm = Web::Bindings::main_thread_vm();
    Vector<JS::ExecutionContext*> contexts;
    while (!vm.execution_context_stack().is_empty()) {
        contexts.append(&vm.running_execution_context());
        vm.pop_execution_context();
    }
    Web::HTML::main_thread_event_loop().perform_a_microtask_checkpoint();
    // And the tasks they queue, a few rounds of them.
    for (int round = 0; round < 10; ++round) {
        (void)Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
        Web::HTML::main_thread_event_loop().perform_a_microtask_checkpoint();
    }
    for (auto* context : contexts.in_reverse())
        vm.push_execution_context(*context);
}

TEST_CASE(observer_callbacks_get_their_own_worlds_arguments)
{
    auto& w = worlds();
    auto& main = w.main_realm();
    auto& extension = *w.extension_realm;
    replace_main_world_builtins(w);

    w.run(extension, u"globalThis.seen = [];"
                     u"globalThis.observer = new MutationObserver((records, o) => {"
                     u"  seen.push(kindOf(records) + reaches(records), String(records[0] instanceof MutationRecord), String(records[0].target instanceof Node));"
                     u"  seen.push(String(o === observer), String(this === undefined || this === o || kindOf(this) === 'own'));"
                     u"  records.forEach(record => seen.push('record:' + record.type));"
                     u"  records[0].addedNodes.forEach(node => seen.push('added:' + kindOf(node) + reaches(node)));"
                     u"  records[0].target.addEventListener('x', () => {});"
                     u"});"
                     u"observer.observe(document.documentElement, { childList: true });"
                     u"document.documentElement.appendChild(document.createElement('span'));"sv);
    checkpoint();
    EXPECT_EQ(w.string(extension, u"seen.join()"sv), "ownextension,true,true,true,true,record:childList,added:ownextension"sv);
    EXPECT_EQ(w.string(main, u"stolen.join()"sv), ""sv);

    // The page's own observers still get its world's.
    w.run(main, u"globalThis.pageSeen = ''; new MutationObserver(records => { pageSeen = kindOf(records) + reaches(records) + (records[0] instanceof MutationRecord) + reaches(records[0]); }).observe(document.documentElement, { attributes: true });"sv);
    w.run(extension, u"observer.disconnect(); document.documentElement.setAttribute('data-x', '1');"sv);
    checkpoint();
    EXPECT_EQ(w.string(main, u"pageSeen"sv), "ownmaintruemain"sv);
    restore_main_world_builtins(w);
}

TEST_CASE(promises_the_engine_returns_are_the_callers_worlds)
{
    auto& w = worlds();
    auto& main = w.main_realm();
    auto& extension = *w.extension_realm;
    replace_main_world_builtins(w);

    w.run(extension, u"{ globalThis.results = [];"
                     u"const check = (name, p) => { results.push(name + ':' + kindOf(p) + reaches(p) + (p instanceof Promise)); return p; };"
                     u"check('replace', new CSSStyleSheet().replace('a { color: red }')).then(sheet => results.push('replaced:' + kindOf(sheet) + reaches(sheet)));"
                     u"check('stream', new ReadableStream({ start(c) { c.enqueue(new Uint8Array([7])); c.close(); } }).getReader().read()).then(r => results.push('read:' + kindOf(r) + reaches(r) + r.value[0] + r.done));"
                     u"check('body', new Response('x').body.getReader().read());"
                     u"check('fonts', document.fonts.ready);"
                     u"check('json', new Response('{\"a\":[1]}').json()); }"sv);
    checkpoint();
    EXPECT_EQ(w.string(extension, u"results.slice(0, 5).join()"sv), "replace:ownextensiontrue,stream:ownextensiontrue,body:ownextensiontrue,fonts:ownextensiontrue,json:ownextensiontrue"sv);
    EXPECT_EQ(w.string(extension, u"results.slice(5).sort().join()"sv), "read:ownextension7false,replaced:ownextension"sv);
    EXPECT_EQ(w.string(main, u"stolen.join()"sv), ""sv);

    // A rejection is the caller's too, and so is what it's rejected with.
    w.run(extension, u"globalThis.rejection = ''; document.createElement('img').decode().then(null, e => { rejection = kindOf(e) + reaches(e) + (e instanceof DOMException); });"sv);
    checkpoint();
    EXPECT_EQ(w.string(extension, u"rejection"sv), "ownextensiontrue"sv);
    restore_main_world_builtins(w);
}

TEST_CASE(stored_values_dont_leave_their_world)
{
    auto& w = worlds();
    auto& main = w.main_realm();
    auto& extension = *w.extension_realm;

    // A reason the page aborted with, thrown by throwIfAborted() in the extension's world.
    w.run(main, u"globalThis.pageController = new AbortController(); pageController.abort({ why: 'page' }); document.dispatchEvent(new CustomEvent('page', { detail: pageController.signal }));"sv);
    EXPECT_EQ(w.string(extension, u"(() => { try { fromPage.throwIfAborted(); return 'none'; } catch (e) { return kindOf(e) + reaches(e) + e.why; } })()"sv), "ownextensionpage"sv);
    // And the other way around.
    w.run(extension, u"globalThis.c = new AbortController(); c.abort({ why: 'ext' }); document.dispatchEvent(new CustomEvent('ext', { detail: c.signal }));"sv);
    EXPECT_EQ(w.string(main, u"(() => { try { got.throwIfAborted(); return 'none'; } catch (e) { return kindOf(e) + reaches(e) + e.why; } })()"sv), "ownmainext"sv);
    EXPECT_EQ(w.string(extension, u"(() => { try { c.signal.throwIfAborted(); return 'none'; } catch (e) { return String(e === c.signal.reason); } })()"sv), "true"sv);

    // A TreeWalker's or NodeIterator's filter is its world's.
    w.run(extension, u"globalThis.filter = () => NodeFilter.FILTER_ACCEPT; globalThis.walker = document.createTreeWalker(document, NodeFilter.SHOW_ALL, filter); globalThis.iterator = document.createNodeIterator(document, NodeFilter.SHOW_ALL, { acceptNode: filter });"
                     u"document.dispatchEvent(new CustomEvent('ext', { detail: [walker, iterator] }));"sv);
    EXPECT_EQ(w.string(main, u"kindOf(got[0].filter) + kindOf(got[1].filter)"sv), "nullnull"sv);
    EXPECT_EQ(w.string(extension, u"String(walker.filter === filter) + String(iterator.filter.acceptNode === filter)"sv), "truetrue"sv);
    // The page's filter, run for the extension's walk, gets the page's nodes.
    w.run(main, u"globalThis.filterSaw = ''; globalThis.pageWalker = document.createTreeWalker(document.documentElement, NodeFilter.SHOW_ELEMENT, node => { filterSaw = reaches(node); return NodeFilter.FILTER_ACCEPT; }); document.dispatchEvent(new CustomEvent('page', { detail: pageWalker }));"sv);
    EXPECT_EQ(w.string(extension, u"kindOf(fromPage.firstChild()) + reaches(fromPage.currentNode)"sv), "ownextension"sv);
    EXPECT_EQ(w.string(main, u"filterSaw"sv), "main"sv);

    // window.opener set in the extension's world stays there.
    w.run(extension, u"window.opener = { secret: 1 };"sv);
    EXPECT_EQ(w.string(main, u"kindOf(window.opener)"sv), "null"sv);
    EXPECT_EQ(w.string(extension, u"kindOf(window.opener) + reaches(window.opener) + window.opener.secret"sv), "ownextension1"sv);
    w.run(main, u"window.opener = { page: 1 };"sv);
    EXPECT_EQ(w.string(extension, u"String(window.opener.secret)"sv), "1"sv);
    EXPECT_EQ(w.string(main, u"reaches(window.opener) + window.opener.page"sv), "main1"sv);
}
