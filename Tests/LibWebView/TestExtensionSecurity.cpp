/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// What the browser lets a page process do in an extension's name: a context it says is a content script is one only in
// a frame it hosts whose document the extension's content scripts (or a script the extension is injecting) are for, at
// the URL the browser knows; storage needs the "storage" permission, and storage.session stays with the extension's own
// pages until it opens it to content scripts; content scripts hear only the events they have.

#include "ExtensionTestHarness.h"

#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/WebContentPage.h>

namespace ExtensionTests {

namespace {

constexpr auto background_id = "kmobjomgmefomhbkbcnefilkmldpddmk"sv;
constexpr auto content_id = "dndcleoheaadbohbloonmemmljamfbjj"sv;
constexpr auto no_storage_id = "ejfeefaeeolngdacgcabnfhahndajece"sv;
constexpr auto about_blank_id = "kokgohejghgkodjfdcbglfifahacinhf"sv;

constexpr auto untrusted_error = "Access to storage is not allowed from this context."sv;

WebView::ExtensionRuntime& runtime()
{
    return WebView::Application::extension_runtime();
}

void spin_for(AK::Duration duration)
{
    auto deadline = MonotonicTime::now() + duration;
    auto wake = Core::Timer::create_repeating(AK::Duration::from_milliseconds(20), [] { });
    wake->start();
    Core::EventLoop::current().spin_until([&] { return MonotonicTime::now() > deadline; });
    wake->stop();
}

template<typename Condition>
bool wait_until(Condition condition)
{
    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(10);
    auto wake = Core::Timer::create_repeating(AK::Duration::from_milliseconds(20), [] { });
    wake->start();
    Core::EventLoop::current().spin_until([&] { return condition() || MonotonicTime::now() > deadline; });
    wake->stop();
    return condition();
}

WebView::ExtensionRuntime::Context const* context_of(WebView::WebContentPage const& page, u64 page_context_id)
{
    WebView::ExtensionRuntime::Context const* found = nullptr;
    runtime().for_each_context([&](auto const& context) {
        if (context.page.ptr() == &page && context.page_context_id == page_context_id)
            found = &context;
    });
    return found;
}

WebView::ExtensionRuntime::Context const* find_context(StringView extension_id, StringView type)
{
    WebView::ExtensionRuntime::Context const* found = nullptr;
    runtime().for_each_context([&](auto const& context) {
        if (context.extension_id == extension_id && context.type == type)
            found = &context;
    });
    return found;
}

WebView::ExtensionRuntime::Context const* background_of(StringView extension_id)
{
    WebView::ExtensionRuntime::Context const* background = nullptr;
    wait_until([&] { return (background = find_context(extension_id, "background"sv)) != nullptr; });
    if (!background)
        warnln("\033[31;1mFAIL\033[0m: {} has no background", extension_id);
    return background;
}

ErrorOr<JsonValue, String> call(StringView name, WebView::ExtensionRuntime::Context const& context, JsonArray arguments = {})
{
    Optional<ErrorOr<JsonValue, String>> result;
    runtime().call_function(name, context, arguments, [&](ErrorOr<JsonValue, String> value) { result = move(value); });
    if (!wait_until([&] { return result.has_value(); }))
        return "(no reply)"_string;
    return result.release_value();
}

JsonObject id_json(Web::HTML::CrossProcessId id)
{
    JsonObject json;
    json.set("ns"sv, id.namespace_id);
    json.set("id"sv, id.local_id);
    return json;
}

// The "frame" message a context's page sends after creating it.
String frame_message(Web::HTML::CrossProcessId navigable, Web::HTML::CrossProcessId document)
{
    JsonObject message;
    message.set("type"sv, "frame"sv);
    message.set("navigable"sv, id_json(navigable));
    message.set("document"sv, id_json(document));
    return message.serialized();
}

String frame_message(WebView::CanonicalNavigable const& navigable)
{
    return frame_message(navigable.id(), navigable.active_document().id());
}

// The extension's content script context in the frame's document, which the page made: gone, as if its world went.
// A document has one per extension, so a forged one is taken only in its place.
bool drop_content_script_context(WebView::WebContentPage& page, WebView::CanonicalNavigable const& navigable, StringView extension_id)
{
    Optional<u64> page_context_id;
    WebView::Application::extension_runtime().for_each_context([&](auto const& context) {
        if (context.page.ptr() == &page && context.type == "content_script"sv && context.extension_id == extension_id && context.navigable_id == navigable.id())
            page_context_id = context.page_context_id;
    });
    if (!page_context_id.has_value())
        return false;
    WebView::Application::extension_runtime().context_destroyed(page, *page_context_id);
    return true;
}

String call_message(StringView api, JsonArray arguments)
{
    JsonObject message;
    message.set("type"sv, "call"sv);
    message.set("id"sv, 1);
    message.set("api"sv, api);
    message.set("args"sv, move(arguments));
    return message.serialized();
}

String listen_message(StringView event, i64 listener)
{
    JsonObject message;
    message.set("type"sv, "listen"sv);
    message.set("event"sv, event);
    message.set("listener"sv, listener);
    return message.serialized();
}

String last_sender(WebView::ExtensionRuntime::Context const& background)
{
    auto items = call("storage.local.get"sv, background, JsonArray { Vector<JsonValue> { JsonValue { "last_sender"_string } } });
    if (items.is_error() || !items.value().is_object())
        return "(error)"_string;
    return items.value().as_object().get_string("last_sender"sv).value_or("(nobody)"_string);
}

void load_page(WebView::HeadlessWebView& view, ByteString const& path)
{
    auto report = report_of(view, url(ByteString::formatted("file://{}", path)));
    (void)report;
}

// The reviewer's attack: a page process says it has a content script of an extension that has none for its page, and
// claims another site's URL. With no frame, a frame it doesn't host, or a frame the extension isn't for, the context
// is never one, so it reaches neither the extension's storage nor its background.
void forged_content_scripts_are_refused(Harness& harness)
{
    MUST(harness.registry.load_unpacked(fixture("security-background"sv)));
    auto const* background = background_of(background_id);
    if (!background)
        return;
    CHECK(wait_until([&] {
        auto items = call("storage.session.get"sv, *background);
        return !items.is_error() && items.value().is_object() && items.value().as_object().has("secret"sv);
    }));

    auto page_path = write_temporary_file(harness.directory, "plain.html"sv, R"(<!doctype html><script>document.title = JSON.stringify({ loaded: true });</script>)"sv);
    load_page(harness.view, page_path);
    auto& page = harness.view.page();
    auto& top = page.traversable();
    auto bank = url("https://bank.example/login"sv);

    // No frame yet: nothing it says counts.
    runtime().context_created(page, 9001, MUST(String::from_utf8(background_id)), "content_script"_string, bank, true);
    CHECK(!context_of(page, 9001));
    runtime().message_received(page, 9001, call_message("runtime.sendMessage"sv, JsonArray { Vector<JsonValue> { JsonValue {}, JsonValue { "from nowhere"_string } } }));

    // Its real frame, but the extension's content scripts are for https://bank.example, not this file.
    runtime().message_received(page, 9001, frame_message(top));
    CHECK(!context_of(page, 9001));
    runtime().message_received(page, 9001, call_message("runtime.sendMessage"sv, JsonArray { Vector<JsonValue> { JsonValue {}, JsonValue { "from the file"_string } } }));
    runtime().message_received(page, 9001, call_message("storage.session.get"sv, {}));

    // A frame the page doesn't host.
    runtime().context_created(page, 9002, MUST(String::from_utf8(background_id)), "content_script"_string, bank, true);
    runtime().message_received(page, 9002, frame_message({ .namespace_id = 4242, .local_id = 4242 }, top.active_document().id()));
    CHECK(!context_of(page, 9002));

    spin_for(AK::Duration::from_milliseconds(500));
    CHECK_EQ(last_sender(*background), "(nobody)"sv);
}

// A context the extension's content scripts are for gets the URL, frame and top-levelness the browser knows, whatever
// its page says; a document that isn't the frame's, a frame the scripts aren't for (not top-level, without
// all_frames), a file without file access, or an ID already in use, is refused.
void content_scripts_are_what_the_browser_knows(Harness& harness)
{
    MUST(harness.registry.load_unpacked(fixture("security-content"sv)));
    harness.registry.set_allows_file_access(content_id, true);
    if (!background_of(content_id))
        return;

    write_temporary_file(harness.directory, "storage-inner.html"sv, "<!doctype html><p>inner</p>"sv);
    auto page_path = write_temporary_file(harness.directory, "storage-forged.html"sv, R"(<!doctype html><iframe src="storage-inner.html"></iframe>)"sv);
    auto page_url = MUST(String::formatted("file://{}", page_path));
    auto report = report_of(harness.view, url(page_url));
    CHECK_EQ(report.get_integer<i64>("local"sv).value_or(0), 1);

    auto& page = harness.view.page();
    auto& top = page.traversable();
    auto bank = url("https://bank.example/login"sv);
    auto content = MUST(String::from_utf8(content_id));

    // The real one.
    auto const* real = find_context(content_id, "content_script"sv);
    CHECK(real != nullptr);
    if (real) {
        CHECK_EQ(real->url.serialize(), page_url);
        CHECK(real->is_top_level);
    }

    // A second one in the same document is refused: the extension has one world there.
    runtime().context_created(page, 9100, content, "content_script"_string, bank, false);
    runtime().message_received(page, 9100, frame_message(top));
    CHECK(context_of(page, 9100) == nullptr);

    // A forged one for the real frame: what the page claims is ignored.
    CHECK(drop_content_script_context(page, top, content_id));
    runtime().context_created(page, 9101, content, "content_script"_string, bank, false);
    runtime().message_received(page, 9101, frame_message(top));
    auto const* forged = context_of(page, 9101);
    CHECK(forged != nullptr);
    if (forged) {
        CHECK_EQ(forged->url.serialize(), page_url);
        CHECK(forged->is_top_level);
        auto sender = runtime().sender_of(*forged);
        CHECK_EQ(string_in(sender, "url"sv), page_url);
        CHECK_EQ(sender.get_integer<i64>("frameId"sv).value_or(-1), 0);
    }

    // The same ID again, for another extension: the first stays.
    runtime().context_created(page, 9101, MUST(String::from_utf8(background_id)), "content_script"_string, bank, true);
    runtime().message_received(page, 9101, frame_message(top));
    forged = context_of(page, 9101);
    CHECK(forged && forged->extension_id == content_id);
    size_t with_id = 0;
    runtime().for_each_context([&](auto const& context) {
        if (context.page.ptr() == &page && context.page_context_id == 9101)
            ++with_id;
    });
    CHECK_EQ(with_id, 1u);

    // A document that isn't the frame's.
    runtime().context_created(page, 9102, content, "content_script"_string, bank, true);
    runtime().message_received(page, 9102, frame_message(top.id(), { .namespace_id = 4242, .local_id = 4242 }));
    CHECK(!context_of(page, 9102));

    // The iframe matches, but the content script isn't for all frames.
    CHECK(!top.children().is_empty());
    if (!top.children().is_empty()) {
        auto const& inner = *top.children().first();
        CHECK(wait_until([&] { return inner.active_document_url().serialize().ends_with_bytes("storage-inner.html"sv); }));
        runtime().context_created(page, 9103, content, "content_script"_string, bank, true);
        runtime().message_received(page, 9103, frame_message(inner));
        CHECK(!context_of(page, 9103));
    }

    // Without file access, no content script is for a file.
    harness.registry.set_allows_file_access(content_id, false);
    runtime().context_created(page, 9104, content, "content_script"_string, bank, true);
    runtime().message_received(page, 9104, frame_message(top));
    CHECK(!context_of(page, 9104));
    harness.registry.set_allows_file_access(content_id, true);

    // A content script asks to hear what content scripts don't have: refused; what they have: heard.
    // (The real one made way for the forged one, above: that one's the frame's now.)
    if (context_of(page, 9101)) {
        u64 real_id = 9101;
        runtime().message_received(page, real_id, listen_message("runtime.onInstalled"sv, 900));
        runtime().message_received(page, real_id, listen_message("alarms.onAlarm"sv, 901));
        runtime().message_received(page, real_id, listen_message("storage.sync.onChanged"sv, 902));
        auto const* again = context_of(page, real_id);
        CHECK(again && !again->has_listener("runtime.onInstalled"sv));
        CHECK(again && !again->has_listener("alarms.onAlarm"sv));
        CHECK(again && again->has_listener("storage.sync.onChanged"sv));
    }
}

// A content script has storage.local; storage.session only once the extension opens it from one of its own pages, and
// only it can; the content script hears of session changes only then.
void content_scripts_get_session_storage_only_when_opened(Harness& harness)
{
    auto page_path = write_temporary_file(harness.directory, "storage-session.html"sv, "<!doctype html><p>page</p>"sv);
    auto page_url = MUST(String::formatted("file://{}", page_path));
    auto report = report_of(harness.view, url(page_url));

    CHECK_EQ(report.get_integer<i64>("local"sv).value_or(0), 1);
    CHECK_EQ(string_in(report, "session_closed"sv), MUST(String::formatted("error: {}", untrusted_error)));
    CHECK_EQ(string_in(report, "session_set_closed"sv), MUST(String::formatted("error: {}", untrusted_error)));
    CHECK_EQ(string_in(report, "level_from_content"sv), MUST(String::formatted("error: {}", untrusted_error)));
    CHECK_EQ(string_in(report, "sender_url"sv), page_url);
    CHECK_EQ(report.get_integer<i64>("sender_frame"sv).value_or(-1), 0);
    CHECK_EQ(string_in(report, "heard_closed"sv), "local:mine"sv);

    auto opened = report.get_object("opened"sv).value_or({});
    CHECK_EQ(string_in(opened, "local_level"sv), "This StorageArea is not available for setting access level"sv);
    CHECK(string_in(opened, "bad_level"sv).starts_with_bytes("Error in invocation"sv));
    CHECK(string_in(opened, "no_level"sv).starts_with_bytes("Error in invocation"sv));
    CHECK_EQ(string_in(opened, "open"sv), "ok"sv);

    CHECK_EQ(report.get_integer<i64>("session_open"sv).value_or(0), 1);
    CHECK_EQ(string_in(report, "level_from_content_open"sv), "error: Context cannot set the storage access level"sv);
    CHECK_EQ(string_in(report, "heard_open"sv), "local:mine|session-area:shown|session:shown"sv);
    CHECK_EQ(string_in(report, "session_reclosed"sv), MUST(String::formatted("error: {}", untrusted_error)));

    // The extension's own pages always have it.
    if (auto const* background = background_of(content_id)) {
        auto items = call("storage.session.get"sv, *background);
        CHECK(!items.is_error());
        if (!items.is_error())
            CHECK(items.value().is_object() && items.value().as_object().has("shown"sv));
    }
}

// Without the "storage" permission, there's no chrome.storage, and the browser refuses its functions.
void storage_needs_its_permission(Harness& harness)
{
    MUST(harness.registry.load_unpacked(fixture("security-nostorage"sv)));
    auto report = report_of(harness.view, url(ByteString::formatted("chrome-extension://{}/page.html", no_storage_id)));
    CHECK_EQ(string_in(report, "storage"sv), "undefined"sv);
    CHECK_EQ(string_in(report, "runtime"sv), "object"sv);

    auto const* background = background_of(no_storage_id);
    if (!background)
        return;
    for (auto name : { "storage.local.get"sv, "storage.local.set"sv, "storage.sync.get"sv, "storage.session.get"sv, "storage.session.setAccessLevel"sv }) {
        JsonArray arguments;
        if (name.ends_with("set"sv))
            arguments.must_append(JsonObject {});
        auto result = call(name, *background, move(arguments));
        CHECK(result.is_error());
        if (result.is_error())
            CHECK_EQ(result.error(), "Access to extension API denied."sv);
    }
}

// An about:blank frame is matched with its creator's URL only when it has its creator's origin: one another document
// navigated has that document's origin, so the top page's content scripts aren't for it.
void about_blank_frames_are_matched_by_their_origin(Harness& harness)
{
    MUST(harness.registry.load_unpacked(fixture("security-aboutblank"sv)));
    harness.registry.set_allows_file_access(about_blank_id, true);

    write_temporary_file(harness.directory, "blank-other.html"sv, R"(<!doctype html><script>
        window.open("about:blank", "victim");
    </script>)"sv);
    auto page_path = write_temporary_file(harness.directory, "blank-top.html"sv, R"(<!doctype html>
        <iframe name="own" srcdoc="<p>own</p>"></iframe>
        <iframe name="victim" src="data:text/html,victim"></iframe>
        <script>
            window.onload = () => {
                const victim = document.querySelector("iframe[name=victim]");
                victim.onload = () => setTimeout(() => {
                    let url;
                    try { url = victim.contentWindow.location.href; } catch (e) { url = "cross-origin"; }
                    document.title = JSON.stringify({ victim: url });
                }, 300);
                const other = document.createElement("iframe");
                other.src = "blank-other.html";
                document.body.appendChild(other);
            };
        </script>
    )"sv);
    auto report = report_of(harness.view, url(ByteString::formatted("file://{}", page_path)));
    // The victim frame was navigated to about:blank by the other document, whose origin it has.
    CHECK_EQ(string_in(report, "victim"sv), "cross-origin"sv);

    auto& top = harness.view.page().traversable();
    Optional<String> own_url;
    Optional<String> victim_url;
    WebView::CanonicalNavigable const* own = nullptr;
    WebView::CanonicalNavigable const* victim = nullptr;
    for (auto const& child : top.children()) {
        auto child_url = child->active_document_url().serialize();
        if (child_url.starts_with_bytes("about:srcdoc"sv)) {
            own_url = child_url;
            own = child.ptr();
        } else if (child_url.starts_with_bytes("about:blank"sv) && !child->active_document().origin().is_same_origin(top.active_document().origin())) {
            victim_url = child_url;
            victim = child.ptr();
        }
    }

    // The browser, too, takes a content script's context in the victim frame for none of the extension's, and one in
    // the frame the top page made for one.
    auto forge = [&](WebView::CanonicalNavigable const& frame, u64 page_context_id) -> bool {
        auto host = frame.active_document().host();
        if (!host)
            return false;
        runtime().context_created(*host, page_context_id, MUST(String::from_utf8(about_blank_id)), "content_script"_string, url("https://bank.example/"sv), false);
        runtime().message_received(*host, page_context_id, frame_message(frame));
        return context_of(*host, page_context_id) != nullptr;
    };
    CHECK(own && own->active_document().host() && !forge(*own, 9300));
    CHECK(own && own->active_document().host() && drop_content_script_context(*own->active_document().host(), *own, about_blank_id));
    CHECK(own && forge(*own, 9301));
    CHECK(victim && !forge(*victim, 9302));
    // The content script ran in the frame the top page made, and not in the other's.
    CHECK_EQ(own_url.value_or("(none)"_string), "about:srcdoc#content-script-ran"sv);
    CHECK_EQ(victim_url.value_or("(none)"_string), "about:blank"sv);
}

// Extensions stay out of private windows (Chrome's default, and Elytra has no setting to let one in): no content script
// runs there, and the browser takes no context from a private page.
void private_windows_get_no_content_scripts(Harness& harness)
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto private_view = WebView::HeadlessWebView::create(MUST(Gfx::load_system_theme(theme_path.string())), { 800, 600 }, WebView::IsPrivate::Yes);
    // The content script reports with "local"; the page, if nothing else did, with "page".
    auto page_path = write_temporary_file(harness.directory, "storage-private.html"sv, R"(<!doctype html><script>
        setTimeout(() => { if (!document.title.startsWith("{")) document.title = JSON.stringify({ page: 1 }); }, 1500);
    </script>)"sv);
    auto report = report_of(*private_view, url(ByteString::formatted("file://{}", page_path)));
    CHECK(!report.has("local"sv));
    CHECK_EQ(report.get_integer<i64>("page"sv).value_or(0), 1);

    auto& page = private_view->page();
    runtime().context_created(page, 9401, MUST(String::from_utf8(content_id)), "content_script"_string, url(ByteString::formatted("file://{}", page_path)), true);
    runtime().message_received(page, 9401, frame_message(page.traversable()));
    CHECK(!context_of(page, 9401));
}

}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    using namespace ExtensionTests;
    return run_extension_tests(arguments, [](Harness& harness) {
        runtime().set_storage_directory(ByteString::formatted("{}/storage", harness.directory));
        forged_content_scripts_are_refused(harness);
        content_scripts_are_what_the_browser_knows(harness);
        private_windows_get_no_content_scripts(harness);
        content_scripts_get_session_storage_only_when_opened(harness);
        storage_needs_its_permission(harness);
        about_blank_frames_are_matched_by_their_origin(harness);
    });
}
