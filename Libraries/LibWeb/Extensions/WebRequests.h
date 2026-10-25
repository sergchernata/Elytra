/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibGC/Function.h>
#include <LibHTTP/HeaderList.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

// What extensions do to this process's network requests, as the browser tells it (WebView's ExtensionAPI/WebRequest):
// chrome.declarativeNetRequest's rules, applied here, and chrome.webRequest's events, reported to the browser, which
// sends them to the extensions listening that may see them. Fetch calls these for each HTTP(S) request it makes of
// the network; the browser's own pages' requests, and workers', are left alone.
namespace Web::Extensions {

// A message from the browser for a page rather than a context: { type: "web_request_config", ... } or
// { type: "web_request_auth_reply", ... }.
WEB_API void page_message_from_browser(Page&, StringView message);

// A message an extension context sends the browser: one that adds a webRequest listener counts at once.
void listener_may_have_been_added(StringView message);

// The ID webRequest knows a request by, given once (a clone keeps it).
void ensure_request_id(Fetch::Infrastructure::Request&);

struct RequestVerdict {
    enum class Kind : u8 {
        Proceed,
        Block,
        Redirect,
    };
    Kind kind { Kind::Proceed };
    Optional<URL::URL> redirect_url;
    // What the later calls name this attempt by; 0 when nobody watches it.
    u64 attempt { 0 };
};

// Before a request goes to the network: its headers may change, and it may be blocked or redirected.
RequestVerdict will_send_request(Page&, Fetch::Infrastructure::Request&, NonnullRefPtr<HTTP::HeaderList>& headers);

// Its response's headers arrived: the headers the response gets instead, if rules change them.
Optional<NonnullRefPtr<HTTP::HeaderList>> did_receive_response_headers(Page&, u64 attempt, u32 status, Optional<String> const& reason_phrase, HTTP::HeaderList const&, bool from_cache);

// It finished, or failed.
void did_finish_request(Page&, u64 attempt, Optional<StringView> error);

// onAuthRequired: whether an extension may answer a challenge to the request, and asking. The answer comes later;
// none when nobody gave credentials or cancelled.
bool may_answer_authentication(Page&, Fetch::Infrastructure::Request const&, Fetch::Infrastructure::Response const&);
struct AuthenticationAnswer {
    Optional<String> username;
    Optional<String> password;
    bool cancel { false };
    // The challenge's realm.
    String realm;
};
void request_authentication(Page&, Fetch::Infrastructure::Request&, Fetch::Infrastructure::Response const&, GC::Ref<GC::Function<void(AuthenticationAnswer)>>);

// Credentials given for the request's origin (and its top-level site), as a Basic Authorization value, for the requests
// after (Fetch's "authentication entry"). Kept in memory only.
Optional<ByteString> authentication_entry_for(Fetch::Infrastructure::Request const&);
void set_authentication_entry(Fetch::Infrastructure::Request const&, String const& realm, String const& username, String const& password);
void forget_authentication_entry(Fetch::Infrastructure::Request const&);

}
