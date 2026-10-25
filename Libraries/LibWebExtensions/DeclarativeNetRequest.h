/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebExtensions/MatchPattern.h>

// chrome.declarativeNetRequest's rules, and how they decide what happens to a request.
// https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest
namespace WebExtensions::DNR {

// Chrome's limits.
constexpr size_t guaranteed_minimum_static_rules = 30000;
constexpr size_t max_number_of_static_rulesets = 100;
constexpr size_t max_number_of_enabled_static_rulesets = 50;
constexpr size_t max_number_of_dynamic_rules = 30000;
constexpr size_t max_number_of_unsafe_dynamic_rules = 5000;
constexpr size_t max_number_of_session_rules = 5000;
constexpr size_t max_number_of_unsafe_session_rules = 5000;
constexpr size_t max_number_of_regex_rules = 1000;
// Longer urlFilters are refused: matching takes time in proportion to the filter's length and the URL's.
constexpr size_t max_url_filter_length = 2048;

enum class ResourceType : u8 {
    MainFrame,
    SubFrame,
    Stylesheet,
    Script,
    Image,
    Font,
    Object,
    XMLHttpRequest,
    Ping,
    CSPReport,
    Media,
    WebSocket,
    WebTransport,
    WebBundle,
    Other,
};
Optional<ResourceType> resource_type_from_string(StringView);
StringView resource_type_to_string(ResourceType);

// Where a rule comes from: a static ruleset in the extension, or updateDynamicRules / updateSessionRules.
enum class RuleSource : u8 {
    Static,
    Dynamic,
    Session,
};

struct HeaderInfo {
    enum class Operation : u8 {
        Append,
        Set,
        Remove,
    };
    String header; // Lowercase.
    Operation operation { Operation::Set };
    Optional<String> value;
};

struct QueryKeyValue {
    String key;
    String value;
    bool replace_only { false };
};

struct URLTransform {
    Optional<String> scheme;
    Optional<String> host;
    Optional<String> port;
    Optional<String> path;
    Optional<String> query;
    Optional<String> fragment;
    Optional<String> username;
    Optional<String> password;
    Vector<String> remove_params;
    Vector<QueryKeyValue> add_or_replace_params;
    bool has_query_transform { false };
};

struct Redirect {
    Optional<String> url;
    Optional<String> extension_path;
    Optional<URLTransform> transform;
    Optional<String> regex_substitution;
};

enum class ActionType : u8 {
    Block,
    Redirect,
    Allow,
    UpgradeScheme,
    ModifyHeaders,
    AllowAllRequests,
};

enum class DomainType : u8 {
    FirstParty,
    ThirdParty,
};

// A regexFilter, compiled (DeclarativeNetRequest.cpp).
class CompiledRegex : public RefCounted<CompiledRegex> {
public:
    virtual ~CompiledRegex() = default;
};

struct Condition {
    Optional<String> url_filter;
    Optional<String> regex_filter;
    bool is_url_filter_case_sensitive { false };
    Optional<Vector<String>> initiator_domains;
    Vector<String> excluded_initiator_domains;
    Optional<Vector<String>> request_domains;
    Vector<String> excluded_request_domains;
    Optional<Vector<ResourceType>> resource_types;
    Vector<ResourceType> excluded_resource_types;
    Optional<Vector<String>> request_methods; // Lowercase.
    Vector<String> excluded_request_methods;
    Optional<DomainType> domain_type;
    Optional<Vector<i64>> tab_ids;
    Vector<i64> excluded_tab_ids;

    RefPtr<CompiledRegex> regex;
};

// What a rule is matched against.
struct Request {
    URL::URL url;
    // The origin of the document or worker that made the request; none for one the browser started (typing a URL).
    Optional<URL::Origin> initiator;
    String method { "get"_string }; // Lowercase.
    ResourceType type { ResourceType::Other };
    i64 tab_id { -1 };
};

class Rule {
public:
    // A rule as an extension gives it. The errors are Chrome's.
    static ErrorOr<Rule, String> parse(JsonValue const&, RuleSource);

    i64 id() const { return m_id; }
    i64 priority() const { return m_priority; }
    ActionType action_type() const { return m_action_type; }
    Optional<Redirect> const& redirect() const { return m_redirect; }
    Vector<HeaderInfo> const& request_headers() const { return m_request_headers; }
    Vector<HeaderInfo> const& response_headers() const { return m_response_headers; }
    Condition const& condition() const { return m_condition; }
    // The rule as given, for getDynamicRules and getSessionRules (and to send it elsewhere).
    JsonObject const& json() const { return m_json; }

    // Block, allow, allowAllRequests and upgradeScheme: what Chrome counts as "safe".
    bool is_safe() const;
    bool uses_regex() const { return m_condition.regex_filter.has_value(); }

    bool matches(Request const&) const;

    // Where a redirect or upgradeScheme rule sends the request; nothing when it can't (or would send it nowhere new).
    Optional<URL::URL> redirect_url_for(Request const&, StringView extension_id) const;

private:
    Rule() = default;

    i64 m_id { 0 };
    i64 m_priority { 1 };
    ActionType m_action_type { ActionType::Block };
    Optional<Redirect> m_redirect;
    Vector<HeaderInfo> m_request_headers;
    Vector<HeaderInfo> m_response_headers;
    Condition m_condition;
    JsonObject m_json;
};

// urlFilter matching, exposed for tests: "||example.com^", "|https://*/x|", "abc*def".
bool url_filter_matches(StringView filter, URL::URL const&, bool case_sensitive);

// Whether LibRegex takes the regex (isRegexSupported); the reason when it doesn't.
Optional<String> regex_unsupported_reason(StringView regex, bool case_sensitive);

// A static ruleset's file. Invalid rules are left out, with a warning each, as Chrome does for packed extensions.
Vector<Rule> parse_ruleset(StringView json, Vector<String>& warnings);

// updateDynamicRules and updateSessionRules: the rules after removing removeRuleIds and adding addRules, or Chrome's
// error, in which case nothing changes.
ErrorOr<Vector<Rule>, String> update_rules(Vector<Rule> const& rules, JsonValue const& options, RuleSource);

// One extension's rules, as the browser hands them out.
struct ExtensionRules {
    String extension_id;
    Vector<Rule> rules;
    // declarativeNetRequestWithHostAccess without declarativeNetRequest: every action needs host access.
    bool all_actions_need_host_access { false };
    // The host permissions it has; redirect and modifyHeaders always need them (for the URL and the initiator).
    MatchPatternSet host_permissions;
};

// A document the request is made from, nearest frame first: allowAllRequests rules that matched its navigation
// allow the requests made in it, too.
struct FrameDocument {
    URL::URL url;
    Optional<URL::Origin> initiator;
    ResourceType type { ResourceType::MainFrame };
};

struct HeaderModification {
    String extension_id;
    HeaderInfo info;
};

struct Outcome {
    bool blocked { false };
    Optional<URL::URL> redirect_url;
    // Applied in order (apply_header_modifications()).
    Vector<HeaderModification> request_headers;
    Vector<HeaderModification> response_headers;
    // The extension whose rule blocked or redirected.
    Optional<String> extension_id;

    bool changes_anything() const { return blocked || redirect_url.has_value() || !request_headers.is_empty() || !response_headers.is_empty(); }
};

// What the extensions' rules do to a request; the extensions most recently installed first.
Outcome evaluate(ReadonlySpan<ExtensionRules>, Request const&, ReadonlySpan<FrameDocument> frames = {});

struct Header {
    String name;
    String value;
};
enum class HeaderStage : u8 {
    Request,
    Response,
};
// Applies modifications to request or response headers (names are matched without regard to case). Appending to a
// request header adds to its value; appending to a response header adds another.
void apply_header_modifications(Vector<Header>&, ReadonlySpan<HeaderModification>, HeaderStage);

}
