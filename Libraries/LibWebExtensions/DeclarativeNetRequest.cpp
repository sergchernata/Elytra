/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/CharacterTypes.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonParser.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16String.h>
#include <LibRegex/ECMAScriptRegex.h>
#include <LibURL/Host.h>
#include <LibURL/Parser.h>
#include <LibWebExtensions/DeclarativeNetRequest.h>

namespace WebExtensions::DNR {

namespace {

class LibRegexFilter final : public CompiledRegex {
public:
    explicit LibRegexFilter(regex::ECMAScriptRegex regex)
        : m_regex(move(regex))
    {
    }

    regex::ECMAScriptRegex const& regex() const { return m_regex; }

private:
    regex::ECMAScriptRegex m_regex;
};

}

static constexpr Array resource_type_names {
    "main_frame"sv, "sub_frame"sv, "stylesheet"sv, "script"sv, "image"sv, "font"sv, "object"sv, "xmlhttprequest"sv,
    "ping"sv, "csp_report"sv, "media"sv, "websocket"sv, "webtransport"sv, "webbundle"sv, "other"sv
};

Optional<ResourceType> resource_type_from_string(StringView name)
{
    for (size_t i = 0; i < resource_type_names.size(); ++i) {
        if (resource_type_names[i] == name)
            return static_cast<ResourceType>(i);
    }
    return {};
}

StringView resource_type_to_string(ResourceType type)
{
    return resource_type_names[to_underlying(type)];
}

static constexpr Array request_method_names { "connect"sv, "delete"sv, "get"sv, "head"sv, "options"sv, "patch"sv, "post"sv, "put"sv, "other"sv };

// The request headers Chrome lets rules append to.
static constexpr Array appendable_request_headers {
    "accept"sv, "accept-encoding"sv, "accept-language"sv, "access-control-request-headers"sv, "cache-control"sv,
    "connection"sv, "content-language"sv, "cookie"sv, "forwarded"sv, "if-match"sv, "if-none-match"sv, "keep-alive"sv,
    "range"sv, "te"sv, "trailer"sv, "transfer-encoding"sv, "upgrade"sv, "user-agent"sv, "via"sv, "want-digest"sv,
    "x-forwarded-for"sv
};

// Errors, in Chrome's words.
static String error(i64 id, StringView what)
{
    return MUST(String::formatted("Rule with id {} {}", id, what));
}

static String missing(i64 id, StringView key)
{
    return error(id, MUST(String::formatted("does not specify the value for \"{}\" key.", key)));
}

static String invalid(i64 id, StringView key)
{
    return error(id, MUST(String::formatted("specifies an incorrect value for the \"{}\" key.", key)));
}

static String empty_list(i64 id, StringView key)
{
    return error(id, MUST(String::formatted("specifies an empty value for the \"{}\" key.", key)));
}

static Optional<i64> integer_in(JsonValue const& value)
{
    if (!value.is_number())
        return {};
    auto number = value.get_double_with_precision_loss().value_or(0.5);
    if (number != static_cast<double>(static_cast<i64>(number)))
        return {};
    return value.get_integer<i64>();
}

// RFC 9110 (as LibHTTP's is_header_value()): no leading or trailing tab or space, no NUL, CR or LF.
static bool is_header_value(StringView value)
{
    if (value.is_empty())
        return true;
    auto is_tab_or_space = [](char c) { return c == '\t' || c == ' '; };
    if (is_tab_or_space(value[0]) || is_tab_or_space(value[value.length() - 1]))
        return false;
    return !any_of(value, [](char c) { return c == '\0' || c == '\r' || c == '\n'; });
}

static bool is_header_name(StringView name)
{
    if (name.is_empty())
        return false;
    for (auto c : name) {
        // RFC 9110 token characters.
        if (!(is_ascii_alphanumeric(c) || "!#$%&'*+-.^_`|~"sv.contains(c)))
            return false;
    }
    return true;
}

// An optional list of strings; an empty list is an error.
static ErrorOr<Optional<Vector<String>>, String> strings_in(JsonObject const& object, StringView key, i64 id, bool lowercase)
{
    auto value = object.get(key);
    if (!value.has_value())
        return Optional<Vector<String>> {};
    if (!value->is_array())
        return invalid(id, MUST(String::formatted("condition.{}", key)));
    Vector<String> strings;
    for (auto const& entry : value->as_array().values()) {
        if (!entry.is_string() || entry.as_string().is_empty())
            return invalid(id, MUST(String::formatted("condition.{}", key)));
        strings.append(lowercase ? entry.as_string().to_ascii_lowercase() : entry.as_string());
    }
    return Optional<Vector<String>> { move(strings) };
}

static ErrorOr<Optional<Vector<String>>, String> nonempty_strings_in(JsonObject const& object, StringView key, i64 id, bool lowercase)
{
    auto strings = TRY(strings_in(object, key, id, lowercase));
    if (strings.has_value() && strings->is_empty())
        return empty_list(id, MUST(String::formatted("condition.{}", key)));
    return strings;
}

static ErrorOr<Optional<Vector<ResourceType>>, String> resource_types_in(JsonObject const& object, StringView key, i64 id)
{
    auto names = TRY(strings_in(object, key, id, false));
    if (!names.has_value())
        return Optional<Vector<ResourceType>> {};
    Vector<ResourceType> types;
    for (auto const& name : *names) {
        auto type = resource_type_from_string(name);
        if (!type.has_value())
            return invalid(id, MUST(String::formatted("condition.{}", key)));
        types.append(*type);
    }
    return Optional<Vector<ResourceType>> { move(types) };
}

static ErrorOr<Optional<Vector<String>>, String> methods_in(JsonObject const& object, StringView key, i64 id)
{
    auto methods = TRY(strings_in(object, key, id, true));
    if (!methods.has_value())
        return methods;
    for (auto const& method : *methods) {
        if (!request_method_names.contains_slow(method.bytes_as_string_view()))
            return invalid(id, MUST(String::formatted("condition.{}", key)));
    }
    return methods;
}

static ErrorOr<Optional<Vector<i64>>, String> tab_ids_in(JsonObject const& object, StringView key, i64 id)
{
    auto value = object.get(key);
    if (!value.has_value())
        return Optional<Vector<i64>> {};
    if (!value->is_array())
        return invalid(id, MUST(String::formatted("condition.{}", key)));
    Vector<i64> ids;
    for (auto const& entry : value->as_array().values()) {
        auto tab = integer_in(entry);
        if (!tab.has_value())
            return invalid(id, MUST(String::formatted("condition.{}", key)));
        ids.append(*tab);
    }
    return Optional<Vector<i64>> { move(ids) };
}

static ErrorOr<RefPtr<CompiledRegex>, String> compile_regex(StringView pattern, bool case_sensitive)
{
    regex::ECMAScriptCompileFlags flags;
    flags.ignore_case = !case_sensitive;
    auto utf16 = Utf16String::from_utf8(pattern);
    auto compiled = regex::ECMAScriptRegex::compile(utf16.utf16_view(), flags);
    if (compiled.is_error())
        return compiled.release_error();
    return RefPtr<CompiledRegex> { adopt_ref(*new LibRegexFilter(compiled.release_value())) };
}

Optional<String> regex_unsupported_reason(StringView pattern, bool case_sensitive)
{
    if (pattern.is_empty())
        return "syntaxError"_string;
    if (compile_regex(pattern, case_sensitive).is_error())
        return "syntaxError"_string;
    return {};
}

static ErrorOr<Vector<HeaderInfo>, String> headers_in(JsonObject const& action, StringView key, i64 id, bool is_request)
{
    auto key_name = MUST(String::formatted("action.{}", key));
    auto list = action.get(key);
    if (!list.has_value())
        return Vector<HeaderInfo> {};
    if (!list->is_array())
        return invalid(id, key_name);
    if (list->as_array().is_empty())
        return empty_list(id, key_name);
    Vector<HeaderInfo> headers;
    for (auto const& entry : list->as_array().values()) {
        if (!entry.is_object())
            return invalid(id, key_name);
        auto const& object = entry.as_object();
        auto name = object.get_string("header"sv);
        if (!name.has_value() || !is_header_name(*name))
            return error(id, "must specify a valid header name for \"header\"."sv);
        auto operation_name = object.get_string("operation"sv);
        HeaderInfo info;
        info.header = name->to_ascii_lowercase();
        if (operation_name == "append"sv)
            info.operation = HeaderInfo::Operation::Append;
        else if (operation_name == "set"sv)
            info.operation = HeaderInfo::Operation::Set;
        else if (operation_name == "remove"sv)
            info.operation = HeaderInfo::Operation::Remove;
        else
            return invalid(id, MUST(String::formatted("{}.operation", key_name)));

        auto value = object.get("value"sv);
        if (info.operation == HeaderInfo::Operation::Remove) {
            if (value.has_value())
                return error(id, "must not provide a \"value\" for the \"remove\" header operation."sv);
        } else {
            if (!value.has_value() || !value->is_string())
                return error(id, MUST(String::formatted("must provide a \"value\" for the \"{}\" header operation.", *operation_name)));
            if (!is_header_value(value->as_string()))
                return error(id, "specifies an invalid header value."sv);
            info.value = value->as_string();
        }
        if (is_request && info.operation == HeaderInfo::Operation::Append && !appendable_request_headers.contains_slow(info.header.bytes_as_string_view()))
            return error(id, "specifies the \"append\" operation for a request header that can't be appended to."sv);
        headers.append(move(info));
    }
    return headers;
}

static ErrorOr<Optional<String>, String> optional_string_in(JsonObject const& object, StringView key, i64 id, StringView key_name)
{
    auto value = object.get(key);
    if (!value.has_value())
        return Optional<String> {};
    if (!value->is_string())
        return invalid(id, key_name);
    return Optional<String> { value->as_string() };
}

static ErrorOr<URLTransform, String> transform_in(JsonValue const& value, i64 id)
{
    if (!value.is_object())
        return invalid(id, "action.redirect.transform"sv);
    auto const& object = value.as_object();
    URLTransform transform;
    transform.scheme = TRY(optional_string_in(object, "scheme"sv, id, "action.redirect.transform.scheme"sv));
    if (transform.scheme.has_value() && !transform.scheme->is_one_of("http"sv, "https"sv, "ftp"sv, "chrome-extension"sv))
        return invalid(id, "action.redirect.transform.scheme"sv);
    transform.host = TRY(optional_string_in(object, "host"sv, id, "action.redirect.transform.host"sv));
    transform.port = TRY(optional_string_in(object, "port"sv, id, "action.redirect.transform.port"sv));
    if (transform.port.has_value() && !transform.port->is_empty() && !transform.port->to_number<u16>().has_value())
        return invalid(id, "action.redirect.transform.port"sv);
    transform.path = TRY(optional_string_in(object, "path"sv, id, "action.redirect.transform.path"sv));
    transform.query = TRY(optional_string_in(object, "query"sv, id, "action.redirect.transform.query"sv));
    if (transform.query.has_value() && !transform.query->is_empty() && !transform.query->starts_with('?'))
        return invalid(id, "action.redirect.transform.query"sv);
    transform.fragment = TRY(optional_string_in(object, "fragment"sv, id, "action.redirect.transform.fragment"sv));
    if (transform.fragment.has_value() && !transform.fragment->is_empty() && !transform.fragment->starts_with('#'))
        return invalid(id, "action.redirect.transform.fragment"sv);
    transform.username = TRY(optional_string_in(object, "username"sv, id, "action.redirect.transform.username"sv));
    transform.password = TRY(optional_string_in(object, "password"sv, id, "action.redirect.transform.password"sv));

    if (auto query_transform = object.get("queryTransform"sv); query_transform.has_value()) {
        if (!query_transform->is_object())
            return invalid(id, "action.redirect.transform.queryTransform"sv);
        transform.has_query_transform = true;
        auto const& query_object = query_transform->as_object();
        if (auto remove = query_object.get("removeParams"sv); remove.has_value()) {
            if (!remove->is_array())
                return invalid(id, "action.redirect.transform.queryTransform.removeParams"sv);
            for (auto const& key : remove->as_array().values()) {
                if (!key.is_string())
                    return invalid(id, "action.redirect.transform.queryTransform.removeParams"sv);
                transform.remove_params.append(key.as_string());
            }
        }
        if (auto add = query_object.get("addOrReplaceParams"sv); add.has_value()) {
            if (!add->is_array())
                return invalid(id, "action.redirect.transform.queryTransform.addOrReplaceParams"sv);
            for (auto const& entry : add->as_array().values()) {
                if (!entry.is_object())
                    return invalid(id, "action.redirect.transform.queryTransform.addOrReplaceParams"sv);
                auto key = entry.as_object().get_string("key"sv);
                auto param_value = entry.as_object().get_string("value"sv);
                auto replace_only = entry.as_object().get("replaceOnly"sv);
                if (!key.has_value() || !param_value.has_value() || (replace_only.has_value() && !replace_only->is_bool()))
                    return invalid(id, "action.redirect.transform.queryTransform.addOrReplaceParams"sv);
                transform.add_or_replace_params.append({ *key, *param_value, replace_only.has_value() && replace_only->as_bool() });
            }
        }
    }
    return transform;
}

ErrorOr<Rule, String> Rule::parse(JsonValue const& value, RuleSource source)
{
    if (!value.is_object())
        return "A rule must be an object."_string;
    auto const& object = value.as_object();

    Rule rule;
    auto id = object.get("id"sv);
    auto id_value = id.has_value() ? integer_in(*id) : Optional<i64> {};
    if (!id_value.has_value() || *id_value < 1 || *id_value > NumericLimits<i32>::max())
        return id.has_value() ? "A rule must have a positive integer value for the \"id\" key."_string : "A rule does not specify the value for \"id\" key."_string;
    rule.m_id = *id_value;
    auto rule_id = rule.m_id;

    if (auto priority = object.get("priority"sv); priority.has_value()) {
        auto priority_value = integer_in(*priority);
        if (!priority_value.has_value() || *priority_value < 1 || *priority_value > NumericLimits<i32>::max())
            return error(rule_id, "must have a positive priority value."sv);
        rule.m_priority = *priority_value;
    }

    // The action.
    auto action = object.get("action"sv);
    if (!action.has_value())
        return missing(rule_id, "action"sv);
    if (!action->is_object())
        return invalid(rule_id, "action"sv);
    auto const& action_object = action->as_object();
    auto type = action_object.get_string("type"sv);
    if (!type.has_value())
        return missing(rule_id, "action.type"sv);
    if (type == "block"sv)
        rule.m_action_type = ActionType::Block;
    else if (type == "redirect"sv)
        rule.m_action_type = ActionType::Redirect;
    else if (type == "allow"sv)
        rule.m_action_type = ActionType::Allow;
    else if (type == "upgradeScheme"sv)
        rule.m_action_type = ActionType::UpgradeScheme;
    else if (type == "modifyHeaders"sv)
        rule.m_action_type = ActionType::ModifyHeaders;
    else if (type == "allowAllRequests"sv)
        rule.m_action_type = ActionType::AllowAllRequests;
    else
        return invalid(rule_id, "action.type"sv);

    // The condition.
    auto condition_value = object.get("condition"sv);
    if (!condition_value.has_value())
        return missing(rule_id, "condition"sv);
    if (!condition_value->is_object())
        return invalid(rule_id, "condition"sv);
    auto const& condition_object = condition_value->as_object();
    auto& condition = rule.m_condition;

    if (auto case_sensitive = condition_object.get("isUrlFilterCaseSensitive"sv); case_sensitive.has_value()) {
        if (!case_sensitive->is_bool())
            return invalid(rule_id, "condition.isUrlFilterCaseSensitive"sv);
        condition.is_url_filter_case_sensitive = case_sensitive->as_bool();
    }
    condition.url_filter = TRY(optional_string_in(condition_object, "urlFilter"sv, rule_id, "condition.urlFilter"sv));
    if (condition.url_filter.has_value()) {
        if (condition.url_filter->is_empty())
            return empty_list(rule_id, "condition.urlFilter"sv);
        if (!all_of(condition.url_filter->bytes_as_string_view(), [](char c) { return is_ascii(c); }))
            return error(rule_id, "specifies a non-ascii \"condition.urlFilter\" value."sv);
        if (condition.url_filter->bytes().size() > max_url_filter_length)
            return error(rule_id, "specifies a \"condition.urlFilter\" value that is too long."sv);
        if (condition.url_filter->starts_with_bytes("||*"sv))
            return error(rule_id, "has a \"condition.urlFilter\" value starting with \"||*\". Use \"*\" instead."sv);
    }
    condition.regex_filter = TRY(optional_string_in(condition_object, "regexFilter"sv, rule_id, "condition.regexFilter"sv));
    if (condition.regex_filter.has_value()) {
        if (condition.url_filter.has_value())
            return error(rule_id, "specifies both \"condition.urlFilter\" and \"condition.regexFilter\" keys. Only one is allowed."sv);
        if (condition.regex_filter->is_empty())
            return empty_list(rule_id, "condition.regexFilter"sv);
        auto compiled = compile_regex(*condition.regex_filter, condition.is_url_filter_case_sensitive);
        if (compiled.is_error())
            return error(rule_id, "specified an invalid regular expression in the \"condition.regexFilter\" key."sv);
        condition.regex = compiled.release_value();
    }

    condition.request_domains = TRY(nonempty_strings_in(condition_object, "requestDomains"sv, rule_id, true));
    condition.excluded_request_domains = TRY(strings_in(condition_object, "excludedRequestDomains"sv, rule_id, true)).value_or({});
    condition.initiator_domains = TRY(nonempty_strings_in(condition_object, "initiatorDomains"sv, rule_id, true));
    condition.excluded_initiator_domains = TRY(strings_in(condition_object, "excludedInitiatorDomains"sv, rule_id, true)).value_or({});

    condition.resource_types = TRY(resource_types_in(condition_object, "resourceTypes"sv, rule_id));
    if (condition.resource_types.has_value() && condition.resource_types->is_empty())
        return empty_list(rule_id, "condition.resourceTypes"sv);
    condition.excluded_resource_types = TRY(resource_types_in(condition_object, "excludedResourceTypes"sv, rule_id)).value_or({});
    if (condition.resource_types.has_value()) {
        for (auto type : condition.excluded_resource_types) {
            if (condition.resource_types->contains_slow(type))
                return error(rule_id, "specifies the same resource type in \"condition.resourceTypes\" and \"condition.excludedResourceTypes\"."sv);
        }
    }

    condition.request_methods = TRY(methods_in(condition_object, "requestMethods"sv, rule_id));
    if (condition.request_methods.has_value() && condition.request_methods->is_empty())
        return empty_list(rule_id, "condition.requestMethods"sv);
    condition.excluded_request_methods = TRY(methods_in(condition_object, "excludedRequestMethods"sv, rule_id)).value_or({});

    if (auto domain_type = condition_object.get("domainType"sv); domain_type.has_value()) {
        if (domain_type->is_string() && domain_type->as_string() == "firstParty"sv)
            condition.domain_type = DomainType::FirstParty;
        else if (domain_type->is_string() && domain_type->as_string() == "thirdParty"sv)
            condition.domain_type = DomainType::ThirdParty;
        else
            return invalid(rule_id, "condition.domainType"sv);
    }

    condition.tab_ids = TRY(tab_ids_in(condition_object, "tabIds"sv, rule_id));
    condition.excluded_tab_ids = TRY(tab_ids_in(condition_object, "excludedTabIds"sv, rule_id)).value_or({});
    if (condition.tab_ids.has_value() && condition.tab_ids->is_empty())
        return empty_list(rule_id, "condition.tabIds"sv);
    if ((condition.tab_ids.has_value() || condition_object.has("excludedTabIds"sv)) && source != RuleSource::Session)
        return error(rule_id, "specifies \"condition.tabIds\" or \"condition.excludedTabIds\", which only session rules can."sv);

    // What the action needs.
    switch (rule.m_action_type) {
    case ActionType::AllowAllRequests: {
        bool only_frames = condition.resource_types.has_value() && all_of(*condition.resource_types, [](auto type) { return type == ResourceType::MainFrame || type == ResourceType::SubFrame; });
        if (!only_frames)
            return error(rule_id, "is an \"allowAllRequests\" rule and must specify \"condition.resourceTypes\" of only \"main_frame\" or \"sub_frame\"."sv);
        break;
    }
    case ActionType::Redirect: {
        auto redirect_value = action_object.get("redirect"sv);
        if (!redirect_value.has_value())
            return missing(rule_id, "action.redirect"sv);
        if (!redirect_value->is_object())
            return invalid(rule_id, "action.redirect"sv);
        auto const& redirect_object = redirect_value->as_object();
        Redirect redirect;
        redirect.url = TRY(optional_string_in(redirect_object, "url"sv, rule_id, "action.redirect.url"sv));
        if (redirect.url.has_value()) {
            auto parsed = URL::Parser::basic_parse(*redirect.url);
            if (!parsed.has_value() || !parsed->scheme().is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv, "data"sv, "chrome-extension"sv))
                return invalid(rule_id, "action.redirect.url"sv);
        }
        redirect.extension_path = TRY(optional_string_in(redirect_object, "extensionPath"sv, rule_id, "action.redirect.extensionPath"sv));
        if (redirect.extension_path.has_value() && !redirect.extension_path->starts_with('/'))
            return error(rule_id, "specifies an \"action.redirect.extensionPath\" that doesn't start with \"/\"."sv);
        if (auto transform = redirect_object.get("transform"sv); transform.has_value())
            redirect.transform = TRY(transform_in(*transform, rule_id));
        redirect.regex_substitution = TRY(optional_string_in(redirect_object, "regexSubstitution"sv, rule_id, "action.redirect.regexSubstitution"sv));
        if (redirect.regex_substitution.has_value() && !condition.regex_filter.has_value())
            return error(rule_id, "specifies \"action.redirect.regexSubstitution\" without \"condition.regexFilter\"."sv);
        if (!redirect.url.has_value() && !redirect.extension_path.has_value() && !redirect.transform.has_value() && !redirect.regex_substitution.has_value())
            return missing(rule_id, "action.redirect"sv);
        rule.m_redirect = move(redirect);
        break;
    }
    case ActionType::ModifyHeaders:
        rule.m_request_headers = TRY(headers_in(action_object, "requestHeaders"sv, rule_id, true));
        rule.m_response_headers = TRY(headers_in(action_object, "responseHeaders"sv, rule_id, false));
        if (rule.m_request_headers.is_empty() && rule.m_response_headers.is_empty())
            return missing(rule_id, "action.requestHeaders\" or \"action.responseHeaders"sv);
        break;
    default:
        break;
    }

    rule.m_json = object;
    rule.m_json.set("priority"sv, rule.m_priority);
    return rule;
}

bool Rule::is_safe() const
{
    return m_action_type == ActionType::Block || m_action_type == ActionType::Allow || m_action_type == ActionType::AllowAllRequests || m_action_type == ActionType::UpgradeScheme;
}

// urlFilter: '*' is anything, '^' a separator (or the end), '|' an end and '||' the host or a subdomain of it.
static bool is_separator(char c)
{
    return !(is_ascii_alphanumeric(c) || c == '_' || c == '-' || c == '.' || c == '%');
}

// Whether the segment (no '*') matches the text at the index, and where it ends if it does. '^' at the text's end
// matches nothing.
static Optional<size_t> segment_matches_at(StringView segment, StringView text, size_t index)
{
    for (auto c : segment) {
        if (index == text.length()) {
            if (c != '^')
                return {};
            continue;
        }
        if (c == '^' ? !is_separator(text[index]) : text[index] != c)
            return {};
        ++index;
    }
    return index;
}

// Where the segment's first match from the index ends.
static Optional<size_t> find_segment(StringView segment, StringView text, size_t from)
{
    if (!segment.contains('^')) {
        auto found = text.substring_view(from).find(segment);
        if (!found.has_value())
            return {};
        return from + *found + segment.length();
    }
    for (size_t i = from; i <= text.length(); ++i) {
        if (auto end = segment_matches_at(segment, text, i); end.has_value())
            return end;
    }
    return {};
}

// Whether the segment matches from the index on, ending at the text's end: only the last few places can.
static bool matches_at_end(StringView segment, StringView text, size_t from)
{
    auto earliest = text.length() >= segment.length() ? text.length() - segment.length() : 0;
    for (auto start = max(earliest, from); start <= text.length(); ++start) {
        if (segment_matches_at(segment, text, start) == text.length())
            return true;
    }
    return false;
}

// The segments after the first, which ended at the index: each star once, left to right. The earliest place a segment
// matches leaves the most for the rest, so the first found is the one to take.
static bool rest_matches(ReadonlySpan<StringView> rest, StringView text, size_t index, bool anchored_at_end)
{
    for (size_t i = 0; i < rest.size(); ++i) {
        if (i + 1 == rest.size() && anchored_at_end)
            return matches_at_end(rest[i], text, index);
        auto end = find_segment(rest[i], text, index);
        if (!end.has_value())
            return false;
        index = *end;
    }
    return true;
}

bool url_filter_matches(StringView filter, URL::URL const& url, bool case_sensitive)
{
    auto serialized = url.serialize(URL::ExcludeFragment::Yes);
    String lowered_text;
    String lowered_filter;
    StringView text = serialized;
    if (!case_sensitive) {
        lowered_text = serialized.to_ascii_lowercase();
        lowered_filter = MUST(String::from_utf8(filter)).to_ascii_lowercase();
        text = lowered_text;
        filter = lowered_filter;
    }

    enum class Anchor : u8 {
        None,
        Start,
        Host,
    };
    auto anchor = Anchor::None;
    if (filter.starts_with("||"sv)) {
        anchor = Anchor::Host;
        filter = filter.substring_view(2);
    } else if (filter.starts_with('|')) {
        anchor = Anchor::Start;
        filter = filter.substring_view(1);
    }
    bool anchored_at_end = false;
    if (filter.ends_with('|')) {
        anchored_at_end = true;
        filter = filter.substring_view(0, filter.length() - 1);
    }

    // The segments between the stars. The first is where the anchor is (empty when the filter starts with a star).
    Vector<StringView> segments;
    filter.for_each_split_view('*', SplitBehavior::KeepEmpty, [&](StringView segment) { segments.append(segment); });
    if (segments.is_empty())
        segments.append({});
    auto first = segments.first();
    auto rest = segments.span().slice(1);

    auto matches_from = [&](size_t start) {
        auto end = segment_matches_at(first, text, start);
        if (!end.has_value())
            return false;
        if (rest.is_empty())
            return !anchored_at_end || *end == text.length();
        return rest_matches(rest, text, *end, anchored_at_end);
    };

    if (anchor == Anchor::Start)
        return matches_from(0);

    if (anchor == Anchor::Host) {
        auto host = url.serialized_host();
        if (host.is_empty())
            return false;
        // Where the host starts: after "scheme://" and any credentials.
        auto start = url.scheme().length() + 3;
        if (url.includes_credentials()) {
            auto at = text.find('@', start);
            if (!at.has_value())
                return false;
            start = *at + 1;
        }
        for (size_t i = 0; i < host.length(); ++i) {
            if (i != 0 && host[i - 1] != '.')
                continue;
            // With stars, the first label the first segment matches at leaves the most for the rest.
            if (!rest.is_empty() && segment_matches_at(first, text, start + i).has_value())
                return matches_from(start + i);
            if (rest.is_empty() && matches_from(start + i))
                return true;
        }
        return false;
    }

    if (rest.is_empty())
        return anchored_at_end ? matches_at_end(first, text, 0) : find_segment(first, text, 0).has_value();
    auto end = find_segment(first, text, 0);
    return end.has_value() && rest_matches(rest, text, *end, anchored_at_end);
}

static bool domain_matches(StringView host, ReadonlySpan<String> domains)
{
    for (auto const& domain : domains) {
        if (host == domain)
            return true;
        if (host.length() > domain.bytes_as_string_view().length() && host.ends_with(domain) && host[host.length() - domain.bytes_as_string_view().length() - 1] == '.')
            return true;
    }
    return false;
}

static String lowercase_host_of(URL::URL const& url)
{
    return MUST(String::from_utf8(url.serialized_host())).to_ascii_lowercase();
}

static String lowercase_host_of(URL::Origin const& origin)
{
    if (origin.is_opaque())
        return {};
    return origin.host().serialize().to_ascii_lowercase();
}

static String site_of(URL::Host const& host)
{
    if (auto domain = host.registrable_domain(); domain.has_value())
        return domain->to_ascii_lowercase();
    return host.serialize().to_ascii_lowercase();
}

bool Rule::matches(Request const& request) const
{
    auto const& condition = m_condition;

    if (condition.resource_types.has_value()) {
        if (!condition.resource_types->contains_slow(request.type))
            return false;
    } else if (request.type == ResourceType::MainFrame && !condition.excluded_resource_types.is_empty()) {
        // With only excludedResourceTypes, main frames count.
    } else if (request.type == ResourceType::MainFrame) {
        return false;
    }
    if (condition.excluded_resource_types.contains_slow(request.type))
        return false;

    bool is_http = request.url.scheme().is_one_of("http"sv, "https"sv);
    if (condition.request_methods.has_value() && (!is_http || !condition.request_methods->contains_slow(request.method)))
        return false;
    if (condition.excluded_request_methods.contains_slow(request.method))
        return false;

    if (condition.tab_ids.has_value() && !condition.tab_ids->contains_slow(request.tab_id))
        return false;
    if (condition.excluded_tab_ids.contains_slow(request.tab_id))
        return false;

    auto host = lowercase_host_of(request.url);
    if (condition.request_domains.has_value() && !domain_matches(host, *condition.request_domains))
        return false;
    if (domain_matches(host, condition.excluded_request_domains))
        return false;

    if (condition.initiator_domains.has_value() || !condition.excluded_initiator_domains.is_empty()) {
        auto initiator_host = request.initiator.has_value() ? lowercase_host_of(*request.initiator) : String {};
        if (condition.initiator_domains.has_value() && (initiator_host.is_empty() || !domain_matches(initiator_host, *condition.initiator_domains)))
            return false;
        if (!initiator_host.is_empty() && domain_matches(initiator_host, condition.excluded_initiator_domains))
            return false;
    }

    if (condition.domain_type.has_value()) {
        bool third_party = false;
        if (request.initiator.has_value() && !request.initiator->is_opaque()) {
            auto request_host = request.url.host();
            third_party = !request_host.has_value() || site_of(*request_host) != site_of(request.initiator->host());
        }
        if ((*condition.domain_type == DomainType::ThirdParty) != third_party)
            return false;
    }

    if (condition.url_filter.has_value())
        return url_filter_matches(*condition.url_filter, request.url, condition.is_url_filter_case_sensitive);
    if (condition.regex) {
        auto const& regex = static_cast<LibRegexFilter const&>(*condition.regex).regex();
        auto text = Utf16String::from_utf8(request.url.serialize(URL::ExcludeFragment::Yes));
        return regex.test(text.utf16_view(), 0) == regex::MatchResult::Match;
    }
    return true;
}

// The query's key=value pairs, transformed.
static String transformed_query(StringView query, URLTransform const& transform)
{
    struct Pair {
        String key;
        String text;
    };
    Vector<Pair> pairs;
    for (auto part : query.split_view('&')) {
        auto equals = part.find('=');
        auto key = MUST(String::from_utf8(equals.has_value() ? part.substring_view(0, *equals) : part));
        pairs.append({ key, MUST(String::from_utf8(part)) });
    }
    pairs.remove_all_matching([&](auto const& pair) { return transform.remove_params.contains_slow(pair.key); });
    for (auto const& param : transform.add_or_replace_params) {
        auto text = MUST(String::formatted("{}={}", param.key, param.value));
        auto existing = pairs.find_if([&](auto const& pair) { return pair.key == param.key; });
        if (existing != pairs.end())
            existing->text = move(text);
        else if (!param.replace_only)
            pairs.append({ param.key, move(text) });
    }
    StringBuilder builder;
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (i != 0)
            builder.append('&');
        builder.append(pairs[i].text);
    }
    return builder.to_string_without_validation();
}

static Optional<URL::URL> apply_transform(URL::URL const& url, URLTransform const& transform)
{
    auto scheme = transform.scheme.has_value() ? transform.scheme->bytes_as_string_view() : url.scheme();
    auto username = transform.username.has_value() ? transform.username->bytes_as_string_view() : url.username();
    auto password = transform.password.has_value() ? transform.password->bytes_as_string_view() : url.password();
    auto host = transform.host.has_value() ? transform.host->bytes_as_string_view() : url.serialized_host();
    Optional<String> port;
    if (transform.port.has_value())
        port = transform.port->is_empty() ? Optional<String> {} : Optional<String> { *transform.port };
    else if (url.port().has_value())
        port = String::number(*url.port());
    auto path = transform.path.has_value() ? transform.path->bytes_as_string_view() : url.serialize_path();

    Optional<String> query;
    if (transform.query.has_value())
        query = transform.query->is_empty() ? Optional<String> {} : Optional<String> { MUST(transform.query->substring_from_byte_offset(1)) };
    else if (url.query().has_value())
        query = MUST(String::from_utf8(*url.query()));
    if (transform.has_query_transform) {
        auto transformed = transformed_query(query.has_value() ? query->bytes_as_string_view() : ""sv, transform);
        query = transformed.is_empty() ? Optional<String> {} : Optional<String> { transformed };
    }

    Optional<String> fragment;
    if (transform.fragment.has_value())
        fragment = transform.fragment->is_empty() ? Optional<String> {} : Optional<String> { MUST(transform.fragment->substring_from_byte_offset(1)) };
    else if (url.fragment().has_value())
        fragment = MUST(String::from_utf8(*url.fragment()));

    StringBuilder builder;
    builder.appendff("{}://", scheme);
    if (!username.is_empty() || !password.is_empty()) {
        builder.append(username);
        if (!password.is_empty())
            builder.appendff(":{}", password);
        builder.append('@');
    }
    builder.append(host);
    if (port.has_value())
        builder.appendff(":{}", *port);
    if (!path.starts_with('/'))
        builder.append('/');
    builder.append(path);
    if (query.has_value())
        builder.appendff("?{}", *query);
    if (fragment.has_value())
        builder.appendff("#{}", *fragment);
    return URL::Parser::basic_parse(builder.string_view());
}

Optional<URL::URL> Rule::redirect_url_for(Request const& request, StringView extension_id) const
{
    Optional<URL::URL> target;
    if (m_action_type == ActionType::UpgradeScheme) {
        auto scheme = request.url.scheme();
        if (scheme != "http"sv && scheme != "ws"sv)
            return {};
        target = request.url;
        target->set_scheme(scheme == "http"sv ? "https"sv : "wss"sv);
        // The default port goes with the scheme.
        if (request.url.port() == 80)
            target->set_port({});
    } else if (m_action_type == ActionType::Redirect && m_redirect.has_value()) {
        auto const& redirect = *m_redirect;
        if (redirect.extension_path.has_value()) {
            target = URL::Parser::basic_parse(MUST(String::formatted("chrome-extension://{}{}", extension_id, *redirect.extension_path)));
        } else if (redirect.transform.has_value()) {
            target = apply_transform(request.url, *redirect.transform);
        } else if (redirect.regex_substitution.has_value() && m_condition.regex) {
            auto const& regex = static_cast<LibRegexFilter const&>(*m_condition.regex).regex();
            auto serialized = request.url.serialize(URL::ExcludeFragment::Yes);
            auto text = Utf16String::from_utf8(serialized);
            if (regex.exec(text.utf16_view(), 0) != regex::MatchResult::Match)
                return {};
            auto group = [&](unsigned index) -> Utf16View {
                if (index >= regex.total_groups())
                    return {};
                auto start = regex.capture_slot(index * 2);
                auto end = regex.capture_slot(index * 2 + 1);
                if (start < 0 || end < start)
                    return {};
                return text.utf16_view().substring_view(start, end - start);
            };
            StringBuilder builder;
            builder.append(text.utf16_view().substring_view(0, regex.capture_slot(0)));
            auto substitution = redirect.regex_substitution->bytes_as_string_view();
            for (size_t i = 0; i < substitution.length(); ++i) {
                if (substitution[i] == '\\' && i + 1 < substitution.length() && is_ascii_digit(substitution[i + 1])) {
                    builder.append(group(substitution[i + 1] - '0'));
                    ++i;
                    continue;
                }
                builder.append(substitution[i]);
            }
            builder.append(text.utf16_view().substring_view(regex.capture_slot(1)));
            target = URL::Parser::basic_parse(builder.string_view());
        } else if (redirect.url.has_value()) {
            target = URL::Parser::basic_parse(*redirect.url);
        }
    }
    if (!target.has_value() || target->serialize() == request.url.serialize())
        return {};
    // Only to the web, data: URLs, or the extension's own pages (which the browser serves only if web accessible).
    auto scheme = target->scheme();
    if (scheme == "chrome-extension"sv)
        return target->serialized_host() == extension_id ? target : Optional<URL::URL> {};
    if (!scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv, "data"sv))
        return {};
    return target;
}

Vector<Rule> parse_ruleset(StringView json, Vector<String>& warnings)
{
    auto parsed = JsonParser::parse(json);
    if (parsed.is_error() || !parsed.value().is_array()) {
        warnings.append("The ruleset isn't a list of rules."_string);
        return {};
    }
    Vector<Rule> rules;
    HashTable<i64> ids;
    size_t regex_rules = 0;
    for (auto const& value : parsed.value().as_array().values()) {
        auto rule = Rule::parse(value, RuleSource::Static);
        if (rule.is_error()) {
            warnings.append(rule.release_error());
            continue;
        }
        if (ids.set(rule.value().id()) != HashSetResult::InsertedNewEntry) {
            warnings.append(error(rule.value().id(), "does not have a unique ID."sv));
            continue;
        }
        if (rule.value().uses_regex() && ++regex_rules > max_number_of_regex_rules) {
            warnings.append(error(rule.value().id(), "is over the limit of regex rules."sv));
            continue;
        }
        rules.append(rule.release_value());
    }
    return rules;
}

ErrorOr<Vector<Rule>, String> update_rules(Vector<Rule> const& rules, JsonValue const& options, RuleSource source)
{
    if (!options.is_object())
        return "Invalid options: the update must be an object."_string;
    auto const& object = options.as_object();

    HashTable<i64> removed;
    if (auto remove = object.get("removeRuleIds"sv); remove.has_value()) {
        if (!remove->is_array())
            return "Invalid value for 'removeRuleIds'."_string;
        for (auto const& id : remove->as_array().values()) {
            auto value = integer_in(id);
            if (!value.has_value())
                return "Invalid value for 'removeRuleIds'."_string;
            removed.set(*value);
        }
    }

    Vector<Rule> updated;
    HashTable<i64> ids;
    for (auto const& rule : rules) {
        if (removed.contains(rule.id()))
            continue;
        updated.append(rule);
        ids.set(rule.id());
    }

    if (auto add = object.get("addRules"sv); add.has_value()) {
        if (!add->is_array())
            return "Invalid value for 'addRules'."_string;
        for (auto const& value : add->as_array().values()) {
            auto rule = TRY(Rule::parse(value, source));
            if (ids.set(rule.id()) != HashSetResult::InsertedNewEntry)
                return error(rule.id(), "does not have a unique ID."sv);
            updated.append(move(rule));
        }
    }

    size_t unsafe = 0;
    size_t regex = 0;
    for (auto const& rule : updated) {
        if (!rule.is_safe())
            ++unsafe;
        if (rule.uses_regex())
            ++regex;
    }
    auto kind = source == RuleSource::Session ? "Session"sv : "Dynamic"sv;
    auto max_rules = source == RuleSource::Session ? max_number_of_session_rules : max_number_of_dynamic_rules;
    auto max_unsafe = source == RuleSource::Session ? max_number_of_unsafe_session_rules : max_number_of_unsafe_dynamic_rules;
    if (updated.size() > max_rules)
        return MUST(String::formatted("{} rule count exceeded.", kind));
    if (unsafe > max_unsafe)
        return MUST(String::formatted("{} rule count for unsafe rules exceeded.", kind));
    if (regex > max_number_of_regex_rules)
        return MUST(String::formatted("{} rule count for regex rules exceeded.", kind));
    return updated;
}

namespace {

// The order of actions at one priority: allow and allowAllRequests, block, upgradeScheme, redirect.
u8 tier_of(ActionType type)
{
    switch (type) {
    case ActionType::Allow:
    case ActionType::AllowAllRequests:
        return 4;
    case ActionType::Block:
        return 3;
    case ActionType::UpgradeScheme:
        return 2;
    case ActionType::Redirect:
        return 1;
    case ActionType::ModifyHeaders:
        return 0;
    }
    VERIFY_NOT_REACHED();
}

struct Decision {
    i64 priority { 0 };
    u8 tier { 0 };
    ActionType type { ActionType::Allow };
    Optional<URL::URL> redirect_url;
};

}

static bool may_act_on(ExtensionRules const& extension, Request const& request, bool needs_host_access)
{
    if (!needs_host_access)
        return true;
    if (!extension.host_permissions.matches_ignoring_path(request.url))
        return false;
    if (request.initiator.has_value() && request.initiator->is_opaque()) {
        // A sandboxed document's, say, which could be any site's: only an extension with access to every host may.
        return any_of(extension.host_permissions.patterns(), [](auto const& pattern) {
            return pattern.matches_all_urls() || (pattern.host().is_empty() && pattern.matches_subdomains());
        });
    }
    if (request.initiator.has_value()) {
        auto const& scheme = request.initiator->scheme();
        if (scheme == "chrome-extension"sv)
            return true;
        if (!extension.host_permissions.matches_origin(*request.initiator))
            return false;
    }
    return true;
}

static bool needs_host_access(ExtensionRules const& extension, Rule const& rule)
{
    return extension.all_actions_need_host_access || rule.action_type() == ActionType::Redirect || rule.action_type() == ActionType::ModifyHeaders;
}

Outcome evaluate(ReadonlySpan<ExtensionRules> extensions, Request const& request, ReadonlySpan<FrameDocument> frames)
{
    Outcome outcome;
    Optional<String> redirecting_extension;
    Optional<URL::URL> redirect_url;
    Vector<HeaderModification> request_headers;
    Vector<HeaderModification> response_headers;

    for (auto const& extension : extensions) {
        // Never what another extension asks for, nor what extensions may not touch.
        if (request.initiator.has_value() && !request.initiator->is_opaque() && request.initiator->scheme() == "chrome-extension"sv
            && request.initiator->host().serialize() != extension.extension_id)
            continue;
        if (is_restricted_url(request.url, false, extension.extension_id.bytes_as_string_view()))
            continue;

        Optional<Decision> best;
        auto consider = [&](Decision decision) {
            if (!best.has_value() || decision.priority > best->priority || (decision.priority == best->priority && decision.tier > best->tier))
                best = move(decision);
        };

        for (auto const& rule : extension.rules) {
            if (rule.action_type() == ActionType::ModifyHeaders)
                continue;
            if (!may_act_on(extension, request, needs_host_access(extension, rule)) || !rule.matches(request))
                continue;
            Decision decision { rule.priority(), tier_of(rule.action_type()), rule.action_type(), {} };
            if (rule.action_type() == ActionType::Redirect || rule.action_type() == ActionType::UpgradeScheme) {
                decision.redirect_url = rule.redirect_url_for(request, extension.extension_id);
                if (!decision.redirect_url.has_value())
                    continue;
            }
            consider(move(decision));
        }

        // allowAllRequests rules that matched the navigations of the frames the request is made in.
        for (auto const& frame : frames) {
            Request frame_request { .url = frame.url, .initiator = frame.initiator, .method = "get"_string, .type = frame.type, .tab_id = request.tab_id };
            for (auto const& rule : extension.rules) {
                if (rule.action_type() != ActionType::AllowAllRequests)
                    continue;
                if (may_act_on(extension, frame_request, needs_host_access(extension, rule)) && rule.matches(frame_request))
                    consider({ rule.priority(), tier_of(ActionType::AllowAllRequests), ActionType::AllowAllRequests, {} });
            }
        }

        if (best.has_value() && best->type == ActionType::Block) {
            if (!outcome.blocked) {
                outcome.blocked = true;
                outcome.extension_id = extension.extension_id;
            }
            continue;
        }
        if (best.has_value() && best->redirect_url.has_value()) {
            if (!redirect_url.has_value()) {
                redirect_url = best->redirect_url;
                redirecting_extension = extension.extension_id;
            }
            continue;
        }

        // modifyHeaders rules above any allow, highest priority first.
        auto minimum_priority = best.has_value() ? best->priority : 0;
        Vector<Rule const*> modifying;
        for (auto const& rule : extension.rules) {
            if (rule.action_type() == ActionType::ModifyHeaders && rule.priority() > minimum_priority && may_act_on(extension, request, true) && rule.matches(request))
                modifying.append(&rule);
        }
        insertion_sort(modifying, [](auto const* a, auto const* b) { return a->priority() > b->priority(); });
        for (auto const* rule : modifying) {
            for (auto const& info : rule->request_headers())
                request_headers.append({ extension.extension_id, info });
            for (auto const& info : rule->response_headers())
                response_headers.append({ extension.extension_id, info });
        }
    }

    if (outcome.blocked)
        return outcome;
    if (redirect_url.has_value()) {
        outcome.redirect_url = move(redirect_url);
        outcome.extension_id = move(redirecting_extension);
        return outcome;
    }
    outcome.request_headers = move(request_headers);
    outcome.response_headers = move(response_headers);
    return outcome;
}

void apply_header_modifications(Vector<Header>& headers, ReadonlySpan<HeaderModification> modifications, HeaderStage stage)
{
    enum class State : u8 {
        Appended,
        Set,
        Removed,
    };
    struct HeaderState {
        State state;
        String extension_id;
    };
    HashMap<String, HeaderState> states;

    auto remove_all = [&](StringView name) {
        headers.remove_all_matching([&](auto const& header) { return header.name.equals_ignoring_ascii_case(name); });
    };

    for (auto const& modification : modifications) {
        auto const& info = modification.info;
        auto state = states.get(info.header);
        switch (info.operation) {
        case HeaderInfo::Operation::Remove:
            if (state.has_value())
                continue;
            remove_all(info.header);
            states.set(info.header, { State::Removed, modification.extension_id });
            break;
        case HeaderInfo::Operation::Set:
            if (state.has_value())
                continue;
            remove_all(info.header);
            headers.append({ info.header, info.value.value_or({}) });
            states.set(info.header, { State::Set, modification.extension_id });
            break;
        case HeaderInfo::Operation::Append: {
            if (state.has_value() && (state->state == State::Removed || (state->state == State::Set && state->extension_id != modification.extension_id)))
                continue;
            auto existing = headers.find_if([&](auto const& header) { return header.name.equals_ignoring_ascii_case(info.header); });
            // Appending nothing to a request header leaves it as it was (not ending in ", ").
            if (stage == HeaderStage::Request && existing != headers.end() && info.value.value_or({}).is_empty())
                break;
            if (stage == HeaderStage::Request && existing != headers.end())
                existing->value = MUST(String::formatted("{}, {}", existing->value, info.value.value_or({})));
            else
                headers.append({ info.header, info.value.value_or({}) });
            if (!state.has_value())
                states.set(info.header, { State::Appended, modification.extension_id });
            break;
        }
        }
    }
}

}
