/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/Time.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibHTTP/Header.h>
#include <LibURL/Parser.h>
#include <LibURL/Site.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/WebRequests.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Page/Page.h>
#include <LibWebExtensions/DeclarativeNetRequest.h>
#include <LibWebExtensions/MatchPattern.h>

namespace Web::Extensions {

namespace DNR = WebExtensions::DNR;
using Fetch::Infrastructure::Request;

namespace {

// What the browser said of a page: the webRequest events extensions listen to, and their rules.
struct PageState {
    GC::Weak<Page> page;
    HashTable<String> events;
    Vector<DNR::ExtensionRules> rules;
};

// A request's attempt at the network: one per redirect, and per retry with credentials.
struct Attempt {
    AK_ALLOC_WITH_KMALLOC;

    GC::Weak<Page> page;
    u64 request_id { 0 };
    URL::URL url;
    JsonObject details;
    Optional<JsonObject> navigable;
    Vector<DNR::HeaderModification> response_modifications;
    UnixDateTime started;

    enum class State : u8 {
        Sending,
        Received,
        Redirecting,
        // A 401 or 407 that an extension may still answer, whose onResponseStarted waits for it.
        Challenged,
        Asking,
        Retried,
        Done,
    };
    State state { State::Sending };
    bool finished_while_asking { false };
    Optional<String> error_while_asking;
    // The realm of the challenge extensions were asked about.
    String realm;
};

struct PendingAuthentication {
    GC::Weak<Page> page;
    u64 attempt { 0 };
    GC::Root<GC::Function<void(AuthenticationAnswer)>> callback;
};

}

// Never destroyed, like the rest of this process's extension state.
static HashMap<Page const*, PageState>& s_pages = *new HashMap<Page const*, PageState>;
static HashMap<u64, NonnullOwnPtr<Attempt>>& s_attempts = *new HashMap<u64, NonnullOwnPtr<Attempt>>;
static HashMap<u64, PendingAuthentication>& s_pending_authentications = *new HashMap<u64, PendingAuthentication>;
// Credentials extensions gave, in memory only: by top-level site and origin, newest last, with the realm they're for.
struct AuthenticationEntry {
    String realm;
    ByteString value;
};
static HashMap<String, Vector<AuthenticationEntry>>& s_authentication_entries = *new HashMap<String, Vector<AuthenticationEntry>>;
// webRequest events listened to in this process, until a word from the browser includes them.
static HashTable<String>& s_unconfirmed_events = *new HashTable<String>;
// Requests that extensions gave credentials for, until they're retried.
static HashTable<u64>& s_retrying_requests = *new HashTable<u64>;
static u64 s_next_request_id = 1;
static u64 s_next_attempt = 1;
static u64 s_next_authentication = 1;

static PageState* state_of(Page const& page)
{
    auto it = s_pages.find(&page);
    if (it == s_pages.end() || !it->value.page)
        return nullptr;
    return &it->value;
}

static void read_config(Page& page, JsonObject const& message)
{
    // Forget pages that are gone.
    s_pages.remove_all_matching([](auto const&, auto const& state) { return !state.page; });

    PageState state;
    state.page = page;
    if (auto events = message.get_array("events"sv); events.has_value()) {
        events->for_each([&](JsonValue const& event) {
            if (event.is_string())
                state.events.set(event.as_string());
        });
    }
    // Listeners added here that the browser hadn't heard of yet when it wrote this still count.
    s_unconfirmed_events.remove_all_matching([&](auto const& event) { return state.events.contains(event); });
    for (auto const& event : s_unconfirmed_events)
        state.events.set(event);
    if (auto extensions = message.get_array("extensions"sv); extensions.has_value()) {
        extensions->for_each([&](JsonValue const& value) {
            if (!value.is_object())
                return;
            auto const& object = value.as_object();
            auto id = object.get_string("id"sv);
            if (!id.has_value())
                return;
            DNR::ExtensionRules rules;
            rules.extension_id = *id;
            rules.all_actions_need_host_access = object.get_bool("allActionsNeedHostAccess"sv).value_or(true);
            if (auto patterns = object.get_array("hostPermissions"sv); patterns.has_value()) {
                patterns->for_each([&](JsonValue const& pattern) {
                    if (!pattern.is_string())
                        return;
                    if (auto parsed = WebExtensions::MatchPattern::parse(pattern.as_string()); !parsed.is_error())
                        rules.host_permissions.add(parsed.release_value());
                });
            }
            if (auto list = object.get_array("rules"sv); list.has_value()) {
                list->for_each([&](JsonValue const& rule_value) {
                    // Session rules come with their tab conditions already applied by the browser.
                    if (auto rule = DNR::Rule::parse(rule_value, DNR::RuleSource::Session); !rule.is_error())
                        rules.rules.append(rule.release_value());
                });
            }
            if (!rules.rules.is_empty())
                state.rules.append(move(rules));
        });
    }
    s_pages.set(&page, move(state));
}

static void authentication_answered(JsonObject const& message);

void page_message_from_browser(Page& page, StringView text)
{
    auto json = JsonParser::parse(text);
    if (json.is_error() || !json.value().is_object())
        return;
    auto const& message = json.value().as_object();
    auto type = message.get_string("type"sv);
    if (type == "web_request_config"sv)
        read_config(page, message);
    else if (type == "web_request_auth_reply"sv)
        authentication_answered(message);
}

void listener_may_have_been_added(StringView text)
{
    if (!text.contains("\"listen\""sv) || !text.contains("webRequest."sv))
        return;
    auto json = JsonParser::parse(text);
    if (json.is_error() || !json.value().is_object())
        return;
    auto const& message = json.value().as_object();
    auto event = message.get_string("event"sv);
    if (message.get_string("type"sv) != "listen"sv || !event.has_value() || !event->starts_with_bytes("webRequest."sv))
        return;
    // Any page of this process may make requests the listener hears of; the browser decides who hears what.
    s_unconfirmed_events.set(*event);
    for (auto& [page, state] : s_pages) {
        if (state.page)
            state.events.set(*event);
    }
}

void ensure_request_id(Request& request)
{
    if (!request.extension_request_id().has_value())
        request.set_extension_request_id(s_next_request_id++);
}

static DNR::ResourceType resource_type_of(Request const& request)
{
    using Destination = Request::Destination;
    if (request.destination().has_value()) {
        switch (*request.destination()) {
        case Destination::Document:
            return DNR::ResourceType::MainFrame;
        case Destination::Frame:
        case Destination::IFrame:
            return DNR::ResourceType::SubFrame;
        case Destination::Style:
        case Destination::XSLT:
            return DNR::ResourceType::Stylesheet;
        case Destination::Script:
        case Destination::ServiceWorker:
        case Destination::SharedWorker:
        case Destination::Worker:
        case Destination::AudioWorklet:
        case Destination::PaintWorklet:
            return DNR::ResourceType::Script;
        case Destination::Image:
            return DNR::ResourceType::Image;
        case Destination::Font:
            return DNR::ResourceType::Font;
        case Destination::Object:
        case Destination::Embed:
            return DNR::ResourceType::Object;
        case Destination::Audio:
        case Destination::Video:
        case Destination::Track:
            return DNR::ResourceType::Media;
        case Destination::Report:
            return DNR::ResourceType::CSPReport;
        default:
            return DNR::ResourceType::Other;
        }
    }
    if (request.initiator_type().has_value()) {
        switch (*request.initiator_type()) {
        case Request::InitiatorType::Fetch:
        case Request::InitiatorType::XMLHttpRequest:
            return DNR::ResourceType::XMLHttpRequest;
        case Request::InitiatorType::Beacon:
        case Request::InitiatorType::Ping:
            return DNR::ResourceType::Ping;
        default:
            break;
        }
    }
    return DNR::ResourceType::Other;
}

namespace {

struct RequestInfo {
    URL::URL url;
    String method;
    DNR::ResourceType type { DNR::ResourceType::Other };
    Optional<URL::Origin> initiator;
    GC::Ptr<HTML::Navigable> navigable;
    Vector<DNR::FrameDocument> frames;
};

}

// What extensions may know of a request: nothing of the browser's own pages', workers' (for now), or CORS preflights.
static Optional<RequestInfo> info_of(Page& page, Request& request)
{
    auto const& url = request.current_url();
    if (!url.scheme().is_one_of("http"sv, "https"sv))
        return {};
    if (!page.has_top_level_traversable())
        return {};
    if (request.method() == "OPTIONS"sv && request.header_list()->contains("Access-Control-Request-Method"sv))
        return {};

    GC::Ptr<DOM::Document> document;
    if (auto client = Fetch::Fetching::resolve_client(request))
        document = client->responsible_document();
    if (document) {
        auto const& document_url = document->url();
        if (document_url.scheme() == "resource"sv)
            return {};
        if (document_url.scheme() == "about"sv && !document_url.serialize_path().is_one_of("blank"sv, "srcdoc"sv))
            return {};
    }

    RequestInfo info;
    info.url = url;
    info.method = MUST(String::from_byte_string(request.method()));
    info.type = resource_type_of(request);

    bool is_frame = info.type == DNR::ResourceType::MainFrame || info.type == DNR::ResourceType::SubFrame;
    // A request names the frame it navigates only as top-level or nested; a nested one is placed by its requester.
    if (is_frame && request.reserved_client_snapshot()) {
        if (auto const* environment = request.reserved_client_snapshot()->kind.get_pointer<Fetch::Infrastructure::ReservedClientContext::Environment>();
            environment && environment->target_browsing_context == Fetch::Infrastructure::ReservedClientContext::TargetBrowsingContext::TopLevel)
            info.navigable = page.top_level_traversable();
    }
    if (!info.navigable && document)
        info.navigable = document->navigable();
    if (!info.navigable && is_frame)
        info.navigable = page.top_level_traversable();

    bool browser_initiated_navigation = !request.client().has_value() && request.mode() == Request::Mode::Navigate;
    if (auto const* origin = request.origin().get_pointer<URL::Origin>(); origin && !browser_initiated_navigation)
        info.initiator = *origin;

    // The documents of the frames it's made in, for allowAllRequests rules.
    auto frame = info.navigable;
    if (frame && is_frame)
        frame = frame->parent();
    for (; frame; frame = frame->parent()) {
        auto frame_url = frame->active_document_url();
        if (!frame_url.has_value())
            break;
        info.frames.append({ .url = *frame_url, .initiator = frame->active_document_origin(), .type = frame->parent() ? DNR::ResourceType::SubFrame : DNR::ResourceType::MainFrame });
    }
    return info;
}

// The headers rules made, without any that isn't a valid header (RequestServer would refuse the whole request).
static NonnullRefPtr<HTTP::HeaderList> header_list_of(Vector<DNR::Header> const& headers)
{
    auto list = HTTP::HeaderList::create();
    for (auto const& header : headers) {
        if (!HTTP::is_header_name(header.name) || !HTTP::is_header_value(header.value))
            continue;
        list->append({ header.name.to_byte_string(), header.value.to_byte_string() });
    }
    return list;
}

static double now_in_milliseconds()
{
    return static_cast<double>(UnixDateTime::now().nanoseconds_since_epoch()) / 1'000'000.0;
}

static JsonArray headers_json(HTTP::HeaderList const& headers)
{
    JsonArray array;
    for (auto const& header : headers.headers()) {
        JsonObject object;
        object.set("name"sv, String::from_utf8_with_replacement_character(header.name));
        object.set("value"sv, String::from_utf8_with_replacement_character(header.value));
        array.must_append(move(object));
    }
    return array;
}

static void report(Page& page, Attempt const& attempt, StringView event, JsonObject const& extra = {})
{
    auto const* state = state_of(page);
    auto name = MUST(String::formatted("webRequest.{}", event));
    if (!state || !state->events.contains(name))
        return;
    auto details = attempt.details;
    extra.for_each_member([&](auto const& key, auto const& value) { details.set(key, value); });
    details.set("timeStamp"sv, now_in_milliseconds());
    JsonObject message;
    message.set("type"sv, "web_request_event"sv);
    message.set("event"sv, name);
    message.set("details"sv, move(details));
    if (attempt.navigable.has_value())
        message.set("navigable"sv, *attempt.navigable);
    page.client().page_did_post_extension_message(0, message.serialized());
}

static bool anyone_cares(Page const& page)
{
    auto const* state = state_of(page);
    return state && (!state->events.is_empty() || !state->rules.is_empty());
}

static JsonObject json_of(HTML::CrossProcessId id)
{
    JsonObject object;
    object.set("ns"sv, id.namespace_id);
    object.set("id"sv, id.local_id);
    return object;
}

// Attempts the network never finished (a fetch that was dropped) don't stay forever.
static void forget_old_attempts()
{
    if (s_attempts.size() < 1024)
        return;
    auto limit = UnixDateTime::now() - AK::Duration::from_seconds(600);
    s_attempts.remove_all_matching([&](auto const&, auto const& attempt) { return !attempt->page || attempt->started < limit; });
}

RequestVerdict will_send_request(Page& page, Request& request, NonnullRefPtr<HTTP::HeaderList>& headers)
{
    if (!anyone_cares(page))
        return {};
    auto info = info_of(page, request);
    if (!info.has_value())
        return {};
    ensure_request_id(request);
    auto request_id = *request.extension_request_id();
    auto const& state = *state_of(page);

    // A retry with credentials is the same request to webRequest: it isn't "before request" again.
    bool is_retry = s_retrying_requests.remove(request_id);

    forget_old_attempts();
    auto attempt_id = s_next_attempt++;
    auto attempt = make<Attempt>();
    attempt->page = page;
    attempt->request_id = request_id;
    attempt->url = info->url;
    attempt->started = UnixDateTime::now();
    attempt->details.set("requestId"sv, String::number(request_id));
    attempt->details.set("url"sv, info->url.serialize());
    attempt->details.set("method"sv, info->method);
    attempt->details.set("type"sv, DNR::resource_type_to_string(info->type));
    if (info->initiator.has_value())
        attempt->details.set("initiator"sv, info->initiator->serialize());
    if (info->navigable)
        attempt->navigable = json_of(info->navigable->id());

    if (!is_retry)
        report(page, *attempt, "onBeforeRequest"sv);

    // onBeforeSendHeaders has the headers as they were.
    JsonObject before_send;
    if (state.events.contains("webRequest.onBeforeSendHeaders"sv))
        before_send.set("requestHeaders"sv, headers_json(*headers));

    // declarativeNetRequest.
    RequestVerdict verdict;
    if (!state.rules.is_empty()) {
        DNR::Request dnr_request { .url = info->url, .initiator = info->initiator, .method = info->method.to_ascii_lowercase(), .type = info->type, .tab_id = -1 };
        auto outcome = DNR::evaluate(state.rules, dnr_request, info->frames);
        if (outcome.blocked) {
            JsonObject extra;
            extra.set("error"sv, "net::ERR_BLOCKED_BY_CLIENT"sv);
            extra.set("fromCache"sv, false);
            report(page, *attempt, "onErrorOccurred"sv, extra);
            verdict.kind = RequestVerdict::Kind::Block;
            return verdict;
        }
        if (outcome.redirect_url.has_value()) {
            JsonObject extra;
            extra.set("redirectUrl"sv, outcome.redirect_url->serialize());
            extra.set("statusCode"sv, 307);
            extra.set("statusLine"sv, "HTTP/1.1 307 Internal Redirect"sv);
            extra.set("fromCache"sv, false);
            report(page, *attempt, "onBeforeRedirect"sv, extra);
            verdict.kind = RequestVerdict::Kind::Redirect;
            verdict.redirect_url = move(outcome.redirect_url);
            return verdict;
        }
        if (!outcome.request_headers.is_empty()) {
            Vector<DNR::Header> modified;
            for (auto const& header : headers->headers())
                modified.append({ String::from_utf8_with_replacement_character(header.name), String::from_utf8_with_replacement_character(header.value) });
            DNR::apply_header_modifications(modified, outcome.request_headers, DNR::HeaderStage::Request);
            headers = header_list_of(modified);
        }
        attempt->response_modifications = move(outcome.response_headers);
    }

    report(page, *attempt, "onBeforeSendHeaders"sv, before_send);
    JsonObject sent;
    sent.set("requestHeaders"sv, headers_json(*headers));
    report(page, *attempt, "onSendHeaders"sv, sent);

    verdict.attempt = attempt_id;
    s_attempts.set(attempt_id, move(attempt));
    return verdict;
}

static Attempt* find_attempt(u64 id)
{
    auto it = s_attempts.find(id);
    if (it == s_attempts.end() || !it->value->page)
        return nullptr;
    return it->value.ptr();
}

static bool has_challenge(u32 status, HTTP::HeaderList const& headers)
{
    if (status == 401)
        return headers.contains("WWW-Authenticate"sv);
    if (status == 407)
        return headers.contains("Proxy-Authenticate"sv);
    return false;
}

Optional<NonnullRefPtr<HTTP::HeaderList>> did_receive_response_headers(Page& page, u64 attempt_id, u32 status, Optional<String> const& reason_phrase, HTTP::HeaderList const& received, bool from_cache)
{
    auto* attempt = find_attempt(attempt_id);
    if (!attempt || attempt->state != Attempt::State::Sending)
        return {};

    Optional<NonnullRefPtr<HTTP::HeaderList>> modified;
    if (!attempt->response_modifications.is_empty()) {
        Vector<DNR::Header> headers;
        for (auto const& header : received.headers())
            headers.append({ String::from_utf8_with_replacement_character(header.name), String::from_utf8_with_replacement_character(header.value) });
        DNR::apply_header_modifications(headers, attempt->response_modifications, DNR::HeaderStage::Response);
        modified = header_list_of(headers);
    }
    HTTP::HeaderList const& headers = modified.has_value() ? **modified : received;

    attempt->details.set("statusCode"sv, status);
    attempt->details.set("statusLine"sv, MUST(String::formatted("HTTP/1.1 {}{}{}", status, reason_phrase.has_value() && !reason_phrase->is_empty() ? " "sv : ""sv, reason_phrase.value_or({}))));
    attempt->details.set("fromCache"sv, from_cache);
    JsonObject with_headers;
    with_headers.set("responseHeaders"sv, headers_json(headers));
    report(page, *attempt, "onHeadersReceived"sv, with_headers);

    auto location = headers.get("Location"sv);
    if (status >= 300 && status < 400 && location.has_value()) {
        auto redirect_url = URL::Parser::basic_parse(*location, attempt->url);
        if (redirect_url.has_value()) {
            JsonObject extra = with_headers;
            extra.set("redirectUrl"sv, redirect_url->serialize());
            report(page, *attempt, "onBeforeRedirect"sv, extra);
            attempt->state = Attempt::State::Redirecting;
            return modified;
        }
    }

    if (has_challenge(status, headers)) {
        attempt->state = Attempt::State::Challenged;
        return modified;
    }

    attempt->state = Attempt::State::Received;
    report(page, *attempt, "onResponseStarted"sv, with_headers);
    return modified;
}

static void report_completion(Page& page, Attempt& attempt, Optional<StringView> error)
{
    attempt.state = Attempt::State::Done;
    JsonObject extra;
    if (error.has_value()) {
        extra.set("error"sv, error->contains("refused"sv) ? "net::ERR_CONNECTION_REFUSED"sv : "net::ERR_FAILED"sv);
        if (!attempt.details.has("fromCache"sv))
            extra.set("fromCache"sv, false);
        report(page, attempt, "onErrorOccurred"sv, extra);
        return;
    }
    report(page, attempt, "onCompleted"sv);
}

void did_finish_request(Page& page, u64 attempt_id, Optional<StringView> error)
{
    auto* attempt = find_attempt(attempt_id);
    if (!attempt)
        return;
    switch (attempt->state) {
    case Attempt::State::Sending:
    case Attempt::State::Received:
        report_completion(page, *attempt, error);
        break;
    case Attempt::State::Challenged:
        // Nobody asked extensions: it's an ordinary response.
        if (!error.has_value())
            report(page, *attempt, "onResponseStarted"sv);
        report_completion(page, *attempt, error);
        break;
    case Attempt::State::Asking:
        attempt->finished_while_asking = true;
        if (error.has_value())
            attempt->error_while_asking = MUST(String::from_utf8(*error));
        return;
    case Attempt::State::Redirecting:
    case Attempt::State::Retried:
    case Attempt::State::Done:
        break;
    }
    s_attempts.remove(attempt_id);
}

static Attempt* latest_attempt_of(u64 request_id, u64* attempt_id = nullptr)
{
    Attempt* latest = nullptr;
    u64 latest_id = 0;
    for (auto& [id, attempt] : s_attempts) {
        if (attempt->request_id == request_id && id > latest_id && attempt->page) {
            latest = attempt.ptr();
            latest_id = id;
        }
    }
    if (attempt_id)
        *attempt_id = latest_id;
    return latest;
}

// The response's Basic challenge: the only scheme the credentials extensions give are used for.
static Optional<StringView> basic_challenge_of(StringView header)
{
    for (auto challenge : header.split_view(',')) {
        challenge = challenge.trim_whitespace();
        if (challenge.starts_with("Basic"sv, CaseSensitivity::CaseInsensitive) && (challenge.length() == 5 || challenge[5] == ' '))
            return challenge;
    }
    return {};
}

bool may_answer_authentication(Page& page, Request const& request, Fetch::Infrastructure::Response const& response)
{
    if (!basic_challenge_of(response.header_list()->get("WWW-Authenticate"sv).value_or({})).has_value())
        return false;
    auto const* state = state_of(page);
    if (!state || !state->events.contains("webRequest.onAuthRequired"sv) || !request.extension_request_id().has_value())
        return false;
    auto const* attempt = latest_attempt_of(*request.extension_request_id());
    return attempt && attempt->state == Attempt::State::Challenged;
}

// The challenge's scheme and realm: `Basic realm="Test Realm"`.
static void describe_challenge(StringView challenge, JsonObject& details)
{
    challenge = challenge.trim_whitespace();
    auto space = challenge.find(' ');
    auto scheme = space.has_value() ? challenge.substring_view(0, *space) : challenge;
    details.set("scheme"sv, MUST(String::from_utf8(scheme)).to_ascii_lowercase());
    auto realm = challenge.find("realm="sv);
    if (!realm.has_value())
        return;
    auto value = challenge.substring_view(*realm + 6);
    if (value.starts_with('"')) {
        value = value.substring_view(1);
        if (auto end = value.find('"'); end.has_value())
            value = value.substring_view(0, *end);
    } else if (auto end = value.find(','); end.has_value()) {
        value = value.substring_view(0, *end);
    }
    details.set("realm"sv, MUST(String::from_utf8(value)));
}

void request_authentication(Page& page, Request& request, Fetch::Infrastructure::Response const& response, GC::Ref<GC::Function<void(AuthenticationAnswer)>> callback)
{
    u64 attempt_id = 0;
    auto* attempt = request.extension_request_id().has_value() ? latest_attempt_of(*request.extension_request_id(), &attempt_id) : nullptr;
    if (!attempt || attempt->state != Attempt::State::Challenged) {
        callback->function()({});
        return;
    }
    attempt->state = Attempt::State::Asking;

    bool is_proxy = response.status() == 407;
    auto details = attempt->details;
    details.set("isProxy"sv, is_proxy);
    auto header = response.header_list()->get(is_proxy ? "Proxy-Authenticate"sv : "WWW-Authenticate"sv).value_or({});
    describe_challenge(basic_challenge_of(header).value_or(header), details);
    attempt->realm = details.get_string("realm"sv).value_or({});
    auto const& url = request.current_url();
    JsonObject challenger;
    challenger.set("host"sv, MUST(String::from_utf8(url.serialized_host())));
    challenger.set("port"sv, url.port_or_default());
    details.set("challenger"sv, move(challenger));
    details.set("responseHeaders"sv, headers_json(*response.header_list()));
    details.set("timeStamp"sv, now_in_milliseconds());

    auto id = s_next_authentication++;
    s_pending_authentications.set(id, { page, attempt_id, GC::make_root(callback) });

    JsonObject message;
    message.set("type"sv, "web_request_auth"sv);
    message.set("request"sv, id);
    message.set("details"sv, move(details));
    if (attempt->navigable.has_value())
        message.set("navigable"sv, *attempt->navigable);
    page.client().page_did_post_extension_message(0, message.serialized());
}

static void authentication_answered(JsonObject const& message)
{
    auto id = message.get_u64("request"sv);
    if (!id.has_value())
        return;
    auto pending = s_pending_authentications.take(*id);
    if (!pending.has_value())
        return;

    AuthenticationAnswer answer;
    if (auto credentials = message.get_object("credentials"sv); credentials.has_value()) {
        answer.username = credentials->get_string("username"sv).copy();
        answer.password = credentials->get_string("password"sv).copy();
        if (!answer.username.has_value() || !answer.password.has_value())
            answer = {};
    }
    answer.cancel = !answer.username.has_value() && message.get_bool("cancel"sv).value_or(false);

    if (auto* attempt = find_attempt(pending->attempt); attempt && pending->page) {
        answer.realm = attempt->realm;
        if (answer.username.has_value()) {
            attempt->state = Attempt::State::Retried;
            s_retrying_requests.set(attempt->request_id);
            if (attempt->finished_while_asking)
                s_attempts.remove(pending->attempt);
        } else if (attempt->finished_while_asking) {
            auto error = attempt->error_while_asking;
            if (!error.has_value())
                report(*pending->page, *attempt, "onResponseStarted"sv);
            report_completion(*pending->page, *attempt, error.has_value() ? Optional<StringView> { *error } : Optional<StringView> {});
            s_attempts.remove(pending->attempt);
        } else {
            attempt->state = Attempt::State::Challenged;
        }
    }
    pending->callback->function()(move(answer));
}

// Entries are partitioned by the top-level site, as Chrome's HTTP auth cache is: what an extension gave for a site's
// request isn't sent for another site's frame or fetch. None for an opaque top-level origin.
static Optional<String> authentication_key(Request const& request)
{
    Optional<URL::Origin> top_level_origin;
    if (request.destination() == Request::Destination::Document)
        top_level_origin = request.current_url().origin();
    else if (auto const& reserved = request.reserved_client_snapshot(); reserved && reserved->top_level_origin.has_value())
        top_level_origin = reserved->top_level_origin;
    else if (auto const& client = request.client_snapshot(); client && client->top_level_origin.has_value())
        top_level_origin = client->top_level_origin;
    if (!top_level_origin.has_value())
        return {};
    auto partition = URL::Site::serialize_for_partitioning(*top_level_origin);
    if (!partition.has_value())
        return {};
    return MUST(String::formatted("{} {}", partition->to_utf8(), request.current_url().origin().serialize()));
}

Optional<ByteString> authentication_entry_for(Request const& request)
{
    if (s_authentication_entries.is_empty())
        return {};
    auto key = authentication_key(request);
    if (!key.has_value())
        return {};
    auto entries = s_authentication_entries.get(*key);
    if (!entries.has_value() || entries->is_empty())
        return {};
    return entries->last().value;
}

void set_authentication_entry(Request const& request, String const& realm, String const& username, String const& password)
{
    auto key = authentication_key(request);
    if (!key.has_value())
        return;
    auto payload = ByteString::formatted("{}:{}", username, password);
    auto& entries = s_authentication_entries.ensure(*key);
    entries.remove_all_matching([&](auto const& entry) { return entry.realm == realm; });
    // A few realms per origin at most.
    if (entries.size() >= 8)
        entries.remove(0);
    entries.append({ realm, ByteString::formatted("Basic {}", MUST(encode_base64(payload.bytes()))) });
    if (s_authentication_entries.size() > 1024)
        s_authentication_entries.remove_all_matching([&](auto const& entry_key, auto const&) { return entry_key != *key; });
}

void forget_authentication_entry(Request const& request)
{
    auto key = authentication_key(request);
    if (!key.has_value())
        return;
    auto entries = s_authentication_entries.find(*key);
    if (entries == s_authentication_entries.end() || entries->value.is_empty())
        return;
    // The newest was the one sent.
    entries->value.take_last();
    if (entries->value.is_empty())
        s_authentication_entries.remove(entries);
}

}
