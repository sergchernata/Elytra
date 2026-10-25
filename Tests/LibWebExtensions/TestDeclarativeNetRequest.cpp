/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/Time.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebExtensions/DeclarativeNetRequest.h>

using namespace WebExtensions::DNR;

static URL::URL url(StringView string)
{
    return URL::Parser::basic_parse(string).release_value();
}

static JsonValue json(StringView text)
{
    return MUST(JsonValue::from_string(text));
}

static Rule rule(StringView text, RuleSource source = RuleSource::Dynamic)
{
    auto parsed = Rule::parse(json(text), source);
    if (parsed.is_error())
        FAIL(parsed.error());
    return parsed.release_value();
}

static String parse_error(StringView text, RuleSource source = RuleSource::Dynamic)
{
    auto parsed = Rule::parse(json(text), source);
    if (!parsed.is_error())
        return "(parsed)"_string;
    return parsed.release_error();
}

static Request request(StringView request_url, ResourceType type = ResourceType::XMLHttpRequest, Optional<StringView> initiator = {})
{
    Request request { .url = url(request_url), .initiator = {}, .method = "get"_string, .type = type, .tab_id = -1 };
    if (initiator.has_value())
        request.initiator = url(*initiator).origin();
    return request;
}

static WebExtensions::MatchPatternSet all_urls()
{
    WebExtensions::MatchPatternSet set;
    set.add(MUST(WebExtensions::MatchPattern::parse("<all_urls>"sv)));
    return set;
}

static ExtensionRules extension(StringView id, Vector<Rule> rules, bool host_access = true)
{
    return { .extension_id = MUST(String::from_utf8(id)), .rules = move(rules), .all_actions_need_host_access = false, .host_permissions = host_access ? all_urls() : WebExtensions::MatchPatternSet {} };
}

TEST_CASE(url_filter_syntax)
{
    EXPECT(url_filter_matches("abc"sv, url("https://example.com/xabcx"sv), false));
    EXPECT(!url_filter_matches("abd"sv, url("https://example.com/xabcx"sv), false));
    EXPECT(url_filter_matches("abc*def"sv, url("https://example.com/abc123def"sv), false));
    EXPECT(!url_filter_matches("abc*def"sv, url("https://example.com/abc123de"sv), false));

    // | anchors at the start and the end.
    EXPECT(url_filter_matches("|https://example.com"sv, url("https://example.com/a"sv), false));
    EXPECT(!url_filter_matches("|example.com"sv, url("https://example.com/a"sv), false));
    EXPECT(url_filter_matches(".png|"sv, url("https://example.com/a.png"sv), false));
    EXPECT(!url_filter_matches(".png|"sv, url("https://example.com/a.png?x"sv), false));

    // || anchors at the host or a subdomain of it.
    EXPECT(url_filter_matches("||example.com"sv, url("https://example.com/a"sv), false));
    EXPECT(url_filter_matches("||example.com"sv, url("https://www.example.com/a"sv), false));
    EXPECT(!url_filter_matches("||example.com"sv, url("https://notexample.com/a"sv), false));
    EXPECT(!url_filter_matches("||example.com"sv, url("https://other.org/?example.com"sv), false));
    EXPECT(url_filter_matches("||example.com/"sv, url("https://a.b.example.com/x/y"sv), false));

    // ^ is a separator, or the end.
    EXPECT(url_filter_matches("||example.com^"sv, url("https://example.com/"sv), false));
    EXPECT(url_filter_matches("||example.com^"sv, url("https://example.com:8000/"sv), false));
    EXPECT(!url_filter_matches("||example.com^"sv, url("https://example.company/"sv), false));
    EXPECT(url_filter_matches("/a^"sv, url("https://example.com/a?b"sv), false));
    EXPECT(!url_filter_matches("/a^"sv, url("https://example.com/ab"sv), false));
    EXPECT(url_filter_matches("example.com/a^"sv, url("https://example.com/a"sv), false));

    // Case: insensitive unless asked.
    EXPECT(url_filter_matches("/ABC"sv, url("https://example.com/abc"sv), false));
    EXPECT(!url_filter_matches("/ABC"sv, url("https://example.com/abc"sv), true));
    EXPECT(url_filter_matches("/abc"sv, url("https://example.com/abc"sv), true));

    // 1Password's.
    EXPECT(url_filter_matches("https://*/dns-query"sv, url("https://cloudflare-dns.com/dns-query?dns=abc"sv), false));
    EXPECT(!url_filter_matches("https://*/dns-query"sv, url("http://cloudflare-dns.com/dns-query"sv), false));
    EXPECT(url_filter_matches("/.well-known/webauthn"sv, url("https://example.com/.well-known/webauthn"sv), false));

    // The fragment isn't part of what's matched.
    EXPECT(!url_filter_matches("frag"sv, url("https://example.com/#frag"sv), false));
}

TEST_CASE(rules_parse_with_chromes_defaults)
{
    auto parsed = rule(R"({"id":1,"action":{"type":"block"},"condition":{"urlFilter":"ads"}})"sv);
    EXPECT_EQ(parsed.id(), 1);
    EXPECT_EQ(parsed.priority(), 1);
    EXPECT_EQ(parsed.action_type(), ActionType::Block);
    EXPECT(parsed.is_safe());
    EXPECT(parsed.json().has("condition"sv));

    auto headers = rule(R"({"id":2,"priority":3,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"User-Agent","operation":"remove"},{"header":"origin","operation":"set","value":"null"}],"responseHeaders":[{"header":"x-a","operation":"append","value":"1"}]},"condition":{}})"sv);
    EXPECT_EQ(headers.priority(), 3);
    EXPECT(!headers.is_safe());
    EXPECT_EQ(headers.request_headers().size(), 2u);
    EXPECT_EQ(headers.request_headers()[0].header, "user-agent"sv);
    EXPECT_EQ(headers.request_headers()[0].operation, HeaderInfo::Operation::Remove);
    EXPECT_EQ(headers.request_headers()[1].value, "null"_string);
    EXPECT_EQ(headers.response_headers()[0].operation, HeaderInfo::Operation::Append);
}

TEST_CASE(invalid_rules_are_refused)
{
    EXPECT(parse_error(R"("rule")"sv).contains("must be an object"sv));
    EXPECT(parse_error(R"({"action":{"type":"block"},"condition":{}})"sv).contains("id"sv));
    EXPECT(parse_error(R"({"id":0,"action":{"type":"block"},"condition":{}})"sv).contains("id"sv));
    EXPECT(parse_error(R"({"id":1.5,"action":{"type":"block"},"condition":{}})"sv).contains("id"sv));
    EXPECT(parse_error(R"({"id":1,"priority":0,"action":{"type":"block"},"condition":{}})"sv).contains("priority"sv));
    EXPECT(parse_error(R"({"id":1,"condition":{}})"sv).contains("action"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"explode"},"condition":{}})"sv).contains("action.type"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"}})"sv).contains("condition"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"urlFilter":""}})"sv).contains("urlFilter"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"urlFilter":"||*x"}})"sv).contains("urlFilter"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"urlFilter":"é"}})"sv).contains("urlFilter"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"urlFilter":"a","regexFilter":"b"}})"sv).contains("regexFilter"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"regexFilter":"("}})"sv).contains("regexFilter"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"requestDomains":[]}})"sv).contains("requestDomains"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"initiatorDomains":[]}})"sv).contains("initiatorDomains"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"resourceTypes":[]}})"sv).contains("resourceTypes"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"resourceTypes":["video"]}})"sv).contains("resourceTypes"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"resourceTypes":["image"],"excludedResourceTypes":["image"]}})"sv).contains("excludedResourceTypes"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"requestMethods":["fly"]}})"sv).contains("requestMethods"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"domainType":"mine"}})"sv).contains("domainType"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"requestDomains":["a.com",5]}})"sv).contains("requestDomains"sv));
    // tabIds are only for session rules.
    EXPECT(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"tabIds":[1]}})"sv).contains("tabIds"sv));
    EXPECT_EQ(parse_error(R"({"id":1,"action":{"type":"block"},"condition":{"tabIds":[1]}})"sv, RuleSource::Session), "(parsed)"sv);
    // allowAllRequests is only for frames.
    EXPECT(parse_error(R"({"id":1,"action":{"type":"allowAllRequests"},"condition":{}})"sv).contains("resourceTypes"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"allowAllRequests"},"condition":{"resourceTypes":["image"]}})"sv).contains("resourceTypes"sv));
    // redirect needs somewhere to go.
    EXPECT(parse_error(R"({"id":1,"action":{"type":"redirect"},"condition":{}})"sv).contains("redirect"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"redirect","redirect":{"url":"javascript:void 0"}},"condition":{}})"sv).contains("redirect"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"redirect","redirect":{"url":"not a url"}},"condition":{}})"sv).contains("redirect"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"redirect","redirect":{"extensionPath":"x.html"}},"condition":{}})"sv).contains("extensionPath"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"redirect","redirect":{"regexSubstitution":"x"}},"condition":{"urlFilter":"a"}})"sv).contains("regexSubstitution"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"redirect","redirect":{"transform":{"scheme":"javascript"}}},"condition":{}})"sv).contains("scheme"sv));
    // modifyHeaders needs headers, valid operations and values where they belong.
    EXPECT(parse_error(R"({"id":1,"action":{"type":"modifyHeaders"},"condition":{}})"sv).contains("requestHeaders"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[]},"condition":{}})"sv).contains("requestHeaders"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"a","operation":"set"}]},"condition":{}})"sv).contains("value"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"a","operation":"remove","value":"x"}]},"condition":{}})"sv).contains("value"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"a b","operation":"remove"}]},"condition":{}})"sv).contains("header"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"a","operation":"twist"}]},"condition":{}})"sv).contains("operation"sv));
    // Only some request headers may be appended to.
    EXPECT(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"x-custom","operation":"append","value":"1"}]},"condition":{}})"sv).contains("append"sv));
    EXPECT_EQ(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"accept-language","operation":"append","value":"fr"}]},"condition":{}})"sv), "(parsed)"sv);
}

TEST_CASE(conditions)
{
    auto domains = rule(R"({"id":1,"action":{"type":"block"},"condition":{"requestDomains":["example.com"],"excludedRequestDomains":["skip.example.com"]}})"sv);
    EXPECT(domains.matches(request("https://example.com/a"sv)));
    EXPECT(domains.matches(request("https://www.example.com/a"sv)));
    EXPECT(!domains.matches(request("https://skip.example.com/a"sv)));
    EXPECT(!domains.matches(request("https://a.skip.example.com/a"sv)));
    EXPECT(!domains.matches(request("https://notexample.com/a"sv)));
    EXPECT(!domains.matches(request("https://example.com.evil/a"sv)));

    auto ip = rule(R"({"id":1,"action":{"type":"block"},"condition":{"requestDomains":["9.9.9.10"]}})"sv);
    EXPECT(ip.matches(request("https://9.9.9.10/dns-query"sv)));

    auto initiators = rule(R"({"id":1,"action":{"type":"block"},"condition":{"initiatorDomains":["site.com"],"excludedInitiatorDomains":["no.site.com"]}})"sv);
    EXPECT(initiators.matches(request("https://cdn.com/x"sv, ResourceType::Script, "https://www.site.com/"sv)));
    EXPECT(!initiators.matches(request("https://cdn.com/x"sv, ResourceType::Script, "https://no.site.com/"sv)));
    EXPECT(!initiators.matches(request("https://cdn.com/x"sv, ResourceType::Script, "https://other.com/"sv)));
    EXPECT(!initiators.matches(request("https://cdn.com/x"sv, ResourceType::Script)));

    // Without resourceTypes, everything but main frames.
    auto any = rule(R"({"id":1,"action":{"type":"block"},"condition":{}})"sv);
    EXPECT(any.matches(request("https://a.com/"sv, ResourceType::Image)));
    EXPECT(any.matches(request("https://a.com/"sv, ResourceType::SubFrame)));
    EXPECT(!any.matches(request("https://a.com/"sv, ResourceType::MainFrame)));
    auto excluded = rule(R"({"id":1,"action":{"type":"block"},"condition":{"excludedResourceTypes":["image"]}})"sv);
    EXPECT(!excluded.matches(request("https://a.com/"sv, ResourceType::Image)));
    EXPECT(excluded.matches(request("https://a.com/"sv, ResourceType::MainFrame)));
    auto types = rule(R"({"id":1,"action":{"type":"block"},"condition":{"resourceTypes":["main_frame","xmlhttprequest"]}})"sv);
    EXPECT(types.matches(request("https://a.com/"sv, ResourceType::MainFrame)));
    EXPECT(types.matches(request("https://a.com/"sv, ResourceType::XMLHttpRequest)));
    EXPECT(!types.matches(request("https://a.com/"sv, ResourceType::Script)));

    auto methods = rule(R"({"id":1,"action":{"type":"block"},"condition":{"requestMethods":["post"]}})"sv);
    auto post = request("https://a.com/"sv);
    post.method = "post"_string;
    EXPECT(methods.matches(post));
    EXPECT(!methods.matches(request("https://a.com/"sv)));
    auto not_get = rule(R"({"id":1,"action":{"type":"block"},"condition":{"excludedRequestMethods":["get"]}})"sv);
    EXPECT(not_get.matches(post));
    EXPECT(!not_get.matches(request("https://a.com/"sv)));

    auto third_party = rule(R"({"id":1,"action":{"type":"block"},"condition":{"domainType":"thirdParty"}})"sv);
    EXPECT(third_party.matches(request("https://tracker.com/x"sv, ResourceType::Script, "https://site.com/"sv)));
    EXPECT(!third_party.matches(request("https://cdn.site.com/x"sv, ResourceType::Script, "https://www.site.com/"sv)));
    auto first_party = rule(R"({"id":1,"action":{"type":"block"},"condition":{"domainType":"firstParty"}})"sv);
    EXPECT(first_party.matches(request("https://cdn.site.com/x"sv, ResourceType::Script, "https://www.site.com/"sv)));
    EXPECT(!first_party.matches(request("https://tracker.com/x"sv, ResourceType::Script, "https://site.com/"sv)));

    auto tabs = rule(R"({"id":1,"action":{"type":"block"},"condition":{"tabIds":[5]}})"sv, RuleSource::Session);
    auto in_tab = request("https://a.com/"sv);
    in_tab.tab_id = 5;
    EXPECT(tabs.matches(in_tab));
    EXPECT(!tabs.matches(request("https://a.com/"sv)));
    auto not_tab = rule(R"({"id":1,"action":{"type":"block"},"condition":{"excludedTabIds":[-1]}})"sv, RuleSource::Session);
    EXPECT(not_tab.matches(in_tab));
    EXPECT(!not_tab.matches(request("https://a.com/"sv)));

    // The rules from 1Password's static ruleset and its session rule.
    auto doh = rule(R"({"id":1,"priority":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"user-agent","operation":"remove"}]},"condition":{"requestDomains":["dns.mullvad.net","cloudflare-dns.com","9.9.9.10"],"isUrlFilterCaseSensitive":false,"urlFilter":"https://*/dns-query","resourceTypes":["xmlhttprequest"]}})"sv, RuleSource::Static);
    EXPECT(doh.matches(request("https://cloudflare-dns.com/dns-query?dns=x"sv)));
    EXPECT(!doh.matches(request("https://cloudflare-dns.com/dns-query?dns=x"sv, ResourceType::Script)));
    EXPECT(!doh.matches(request("https://example.com/dns-query"sv)));
}

TEST_CASE(regex_filters)
{
    auto regex = rule(R"({"id":1,"action":{"type":"block"},"condition":{"regexFilter":"^https://[a-z]+\\.com/ads?/"}})"sv);
    EXPECT(regex.uses_regex());
    EXPECT(regex.matches(request("https://example.com/ad/1"sv)));
    EXPECT(regex.matches(request("https://EXAMPLE.com/ads/1"sv)));
    EXPECT(!regex.matches(request("https://example.org/ad/1"sv)));
    auto sensitive = rule(R"({"id":1,"action":{"type":"block"},"condition":{"regexFilter":"/ADS/","isUrlFilterCaseSensitive":true}})"sv);
    EXPECT(!sensitive.matches(request("https://example.com/ads/"sv)));

    EXPECT(!regex_unsupported_reason("^a+$"sv, false).has_value());
    EXPECT(regex_unsupported_reason("(a"sv, false).has_value());
}

TEST_CASE(redirects)
{
    auto to_url = rule(R"({"id":1,"action":{"type":"redirect","redirect":{"url":"https://other.com/x"}},"condition":{"urlFilter":"old"}})"sv);
    EXPECT_EQ(to_url.redirect_url_for(request("https://a.com/old"sv), "ext"sv)->serialize(), "https://other.com/x"sv);

    auto to_extension = rule(R"({"id":1,"action":{"type":"redirect","redirect":{"extensionPath":"/page.html?a"}},"condition":{"urlFilter":"old"}})"sv);
    EXPECT_EQ(to_extension.redirect_url_for(request("https://a.com/old"sv), "abcd"sv)->serialize(), "chrome-extension://abcd/page.html?a"sv);

    auto transform = rule(R"({"id":1,"action":{"type":"redirect","redirect":{"transform":{"host":"new.com","path":"/p","queryTransform":{"removeParams":["drop"],"addOrReplaceParams":[{"key":"k","value":"v"},{"key":"only","value":"x","replaceOnly":true}]}}}},"condition":{"urlFilter":"a.com"}})"sv);
    EXPECT_EQ(transform.redirect_url_for(request("https://a.com/old?drop=1&k=0&keep=2#f"sv), "ext"sv)->serialize(), "https://new.com/p?k=v&keep=2#f"sv);
    auto replace_query = rule(R"({"id":1,"action":{"type":"redirect","redirect":{"transform":{"scheme":"https","port":"8443","query":"?q=1","fragment":"#top"}}},"condition":{"urlFilter":"a.com"}})"sv);
    EXPECT_EQ(replace_query.redirect_url_for(request("http://a.com/x?old"sv), "ext"sv)->serialize(), "https://a.com:8443/x?q=1#top"sv);
    auto clear_port = rule(R"({"id":1,"action":{"type":"redirect","redirect":{"transform":{"port":"","query":""}}},"condition":{"urlFilter":"a.com"}})"sv);
    EXPECT_EQ(clear_port.redirect_url_for(request("http://a.com:81/x?old"sv), "ext"sv)->serialize(), "http://a.com/x"sv);

    auto substitution = rule(R"({"id":1,"action":{"type":"redirect","redirect":{"regexSubstitution":"https://\\1.org/\\2"}},"condition":{"regexFilter":"^https://([a-z]+)\\.com/(.*)$"}})"sv);
    EXPECT_EQ(substitution.redirect_url_for(request("https://example.com/path/x"sv), "ext"sv)->serialize(), "https://example.org/path/x"sv);

    auto upgrade = rule(R"({"id":1,"action":{"type":"upgradeScheme"},"condition":{}})"sv);
    EXPECT_EQ(upgrade.redirect_url_for(request("http://a.com/x"sv), "ext"sv)->serialize(), "https://a.com/x"sv);
    EXPECT(!upgrade.redirect_url_for(request("https://a.com/x"sv), "ext"sv).has_value());

    // A redirect that goes nowhere new doesn't count.
    auto same = rule(R"({"id":1,"action":{"type":"redirect","redirect":{"url":"https://a.com/x"}},"condition":{}})"sv);
    EXPECT(!same.redirect_url_for(request("https://a.com/x"sv), "ext"sv).has_value());
}

TEST_CASE(priorities_and_actions)
{
    auto block = rule(R"({"id":1,"action":{"type":"block"},"condition":{"urlFilter":"x"}})"sv);
    auto allow = rule(R"({"id":2,"action":{"type":"allow"},"condition":{"urlFilter":"x"}})"sv);
    auto redirect = rule(R"({"id":3,"action":{"type":"redirect","redirect":{"url":"https://r.com/"}},"condition":{"urlFilter":"x"}})"sv);
    auto upgrade = rule(R"({"id":4,"action":{"type":"upgradeScheme"},"condition":{"urlFilter":"x"}})"sv);
    auto high_redirect = rule(R"({"id":5,"priority":5,"action":{"type":"redirect","redirect":{"url":"https://r.com/"}},"condition":{"urlFilter":"x"}})"sv);

    auto outcome_of = [](Vector<Rule> rules, StringView request_url = "http://a.com/x"sv) {
        Vector<ExtensionRules> extensions;
        extensions.append(extension("e"sv, move(rules)));
        return evaluate(extensions, request(request_url));
    };

    // At one priority: allow beats block beats upgradeScheme beats redirect.
    EXPECT(!outcome_of({ block, allow, redirect }).blocked);
    EXPECT(outcome_of({ block, redirect, upgrade }).blocked);
    EXPECT_EQ(outcome_of({ redirect, upgrade }).redirect_url->serialize(), "https://a.com/x"sv);
    EXPECT_EQ(outcome_of({ redirect }).redirect_url->serialize(), "https://r.com/"sv);
    // A higher priority wins.
    auto outcome = outcome_of({ block, allow, high_redirect });
    EXPECT(!outcome.blocked);
    EXPECT_EQ(outcome.redirect_url->serialize(), "https://r.com/"sv);
    EXPECT_EQ(outcome.extension_id, "e"_string);
    EXPECT(!outcome_of({ block }, "http://a.com/y"sv).changes_anything());

    // Across extensions: block beats redirect beats allow; between redirects the most recently installed (first) wins.
    auto other_redirect = rule(R"({"id":1,"action":{"type":"redirect","redirect":{"url":"https://other.com/"}},"condition":{"urlFilter":"x"}})"sv);
    Vector<ExtensionRules> extensions;
    extensions.append(extension("new"sv, { other_redirect }));
    extensions.append(extension("old"sv, { high_redirect }));
    EXPECT_EQ(evaluate(extensions, request("http://a.com/x"sv)).redirect_url->serialize(), "https://other.com/"sv);
    extensions.append(extension("blocker"sv, { block }));
    outcome = evaluate(extensions, request("http://a.com/x"sv));
    EXPECT(outcome.blocked);
    EXPECT_EQ(outcome.extension_id, "blocker"_string);
    extensions.clear();
    extensions.append(extension("allower"sv, { allow }));
    extensions.append(extension("redirecter"sv, { redirect }));
    EXPECT(evaluate(extensions, request("http://a.com/x"sv)).redirect_url.has_value());
}

TEST_CASE(allow_all_requests_covers_a_frame)
{
    auto allow_frame = rule(R"({"id":1,"priority":2,"action":{"type":"allowAllRequests"},"condition":{"urlFilter":"||safe.com","resourceTypes":["main_frame"]}})"sv);
    auto block = rule(R"({"id":2,"action":{"type":"block"},"condition":{"urlFilter":"ads"}})"sv);
    auto high_block = rule(R"({"id":3,"priority":3,"action":{"type":"block"},"condition":{"urlFilter":"evil"}})"sv);
    Vector<ExtensionRules> extensions;
    extensions.append(extension("e"sv, { allow_frame, block, high_block }));

    Vector<FrameDocument> safe { { .url = url("https://safe.com/"sv), .initiator = {}, .type = ResourceType::MainFrame } };
    Vector<FrameDocument> unsafe { { .url = url("https://other.com/"sv), .initiator = {}, .type = ResourceType::MainFrame } };
    EXPECT(!evaluate(extensions, request("https://cdn.com/ads.js"sv, ResourceType::Script, "https://safe.com/"sv), safe).blocked);
    EXPECT(evaluate(extensions, request("https://cdn.com/ads.js"sv, ResourceType::Script, "https://other.com/"sv), unsafe).blocked);
    EXPECT(evaluate(extensions, request("https://cdn.com/evil.js"sv, ResourceType::Script, "https://safe.com/"sv), safe).blocked);
}

TEST_CASE(modify_headers_combine_as_chrome_does)
{
    auto set_a = rule(R"({"id":1,"priority":2,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"x-a","operation":"set","value":"high"}]},"condition":{}})"sv);
    auto set_a_low = rule(R"({"id":2,"priority":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"x-a","operation":"set","value":"low"},{"header":"user-agent","operation":"append","value":"same-extension"}]},"condition":{}})"sv);
    auto remove_ua = rule(R"({"id":1,"priority":9,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"user-agent","operation":"remove"}]},"condition":{}})"sv);
    auto append_ua = rule(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"accept-language","operation":"append","value":"fr"}]},"condition":{}})"sv);
    auto response = rule(R"({"id":3,"action":{"type":"modifyHeaders","responseHeaders":[{"header":"access-control-allow-origin","operation":"set","value":"*"},{"header":"x-extra","operation":"append","value":"2"}]},"condition":{}})"sv);

    Vector<ExtensionRules> extensions;
    extensions.append(extension("first"sv, { set_a_low, set_a, response }));
    extensions.append(extension("second"sv, { remove_ua, append_ua }));
    auto outcome = evaluate(extensions, request("https://a.com/"sv));
    EXPECT(!outcome.blocked);

    Vector<Header> headers {
        { "User-Agent"_string, "Browser"_string },
        { "X-A"_string, "original"_string },
        { "Accept-Language"_string, "en"_string },
    };
    apply_header_modifications(headers, outcome.request_headers, HeaderStage::Request);
    auto value_of = [&](StringView name) -> Optional<String> {
        for (auto const& header : headers) {
            if (header.name.equals_ignoring_ascii_case(name))
                return header.value;
        }
        return {};
    };
    // The first extension's higher-priority set wins; its lower one can't change it.
    EXPECT_EQ(value_of("x-a"sv), "high"_string);
    // The first extension (the more recent) appends to user-agent; after an append, the second's remove can't act.
    EXPECT_EQ(value_of("user-agent"sv), "Browser, same-extension"_string);
    EXPECT_EQ(value_of("accept-language"sv), "en, fr"_string);

    Vector<Header> response_headers { { "Access-Control-Allow-Origin"_string, "https://x.com"_string }, { "X-Extra"_string, "1"_string } };
    apply_header_modifications(response_headers, outcome.response_headers, HeaderStage::Response);
    EXPECT_EQ(response_headers.size(), 3u);
    EXPECT_EQ(response_headers[0].name.to_ascii_lowercase(), "x-extra"sv);
    size_t extras = 0;
    for (auto const& header : response_headers) {
        if (header.name.equals_ignoring_ascii_case("access-control-allow-origin"sv))
            EXPECT_EQ(header.value, "*"sv);
        if (header.name.equals_ignoring_ascii_case("x-extra"sv))
            ++extras;
    }
    EXPECT_EQ(extras, 2u);

    // A remove first stops anything later.
    Vector<ExtensionRules> remover_first;
    remover_first.append(extension("second"sv, { remove_ua }));
    remover_first.append(extension("first"sv, { set_a_low }));
    outcome = evaluate(remover_first, request("https://a.com/"sv));
    Vector<Header> more { { "User-Agent"_string, "Browser"_string } };
    apply_header_modifications(more, outcome.request_headers, HeaderStage::Request);
    EXPECT(more.size() == 1 && more[0].name.equals_ignoring_ascii_case("x-a"sv));

    // A set by one extension: another's append can't add to it.
    auto append_x_ua = rule(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"user-agent","operation":"append","value":"other"}]},"condition":{}})"sv);
    auto set_ua = rule(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"user-agent","operation":"set","value":"Mine"}]},"condition":{}})"sv);
    Vector<ExtensionRules> setter_first;
    setter_first.append(extension("setter"sv, { set_ua }));
    setter_first.append(extension("appender"sv, { append_x_ua }));
    outcome = evaluate(setter_first, request("https://a.com/"sv));
    Vector<Header> ua { { "User-Agent"_string, "Browser"_string } };
    apply_header_modifications(ua, outcome.request_headers, HeaderStage::Request);
    EXPECT_EQ(ua[0].value, "Mine"sv);

    // Only modifyHeaders rules above an allow count.
    auto allow = rule(R"({"id":9,"priority":2,"action":{"type":"allow"},"condition":{}})"sv);
    Vector<ExtensionRules> allowed;
    allowed.append(extension("first"sv, { set_a_low, set_a, allow }));
    outcome = evaluate(allowed, request("https://a.com/"sv));
    EXPECT(outcome.request_headers.is_empty());

    // A blocked request has no headers to change.
    auto block = rule(R"({"id":9,"action":{"type":"block"},"condition":{}})"sv);
    Vector<ExtensionRules> blocked;
    blocked.append(extension("first"sv, { set_a }));
    blocked.append(extension("blocker"sv, { block }));
    outcome = evaluate(blocked, request("https://a.com/"sv));
    EXPECT(outcome.blocked);
    EXPECT(outcome.request_headers.is_empty());
}

TEST_CASE(host_access_gates_actions)
{
    auto block = rule(R"({"id":1,"action":{"type":"block"},"condition":{"urlFilter":"b"}})"sv);
    auto redirect = rule(R"({"id":2,"action":{"type":"redirect","redirect":{"url":"https://r.com/"}},"condition":{"urlFilter":"r"}})"sv);
    auto headers = rule(R"({"id":3,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"x-a","operation":"set","value":"1"}]},"condition":{"urlFilter":"h"}})"sv);

    // declarativeNetRequest without host permissions: block only.
    Vector<ExtensionRules> without_hosts;
    without_hosts.append(extension("e"sv, { block, redirect, headers }, false));
    EXPECT(evaluate(without_hosts, request("https://a.com/b"sv)).blocked);
    EXPECT(!evaluate(without_hosts, request("https://a.com/r"sv)).changes_anything());
    EXPECT(!evaluate(without_hosts, request("https://a.com/h"sv)).changes_anything());

    // declarativeNetRequestWithHostAccess: everything needs host access, for the URL and the initiator.
    WebExtensions::MatchPatternSet a_com;
    a_com.add(MUST(WebExtensions::MatchPattern::parse("https://a.com/*"sv)));
    Vector<ExtensionRules> with_host_access;
    with_host_access.append({ .extension_id = "e"_string, .rules = { block, redirect, headers }, .all_actions_need_host_access = true, .host_permissions = a_com });
    EXPECT(evaluate(with_host_access, request("https://a.com/b"sv)).blocked);
    EXPECT(!evaluate(with_host_access, request("https://b.com/b"sv)).blocked);
    EXPECT(evaluate(with_host_access, request("https://a.com/r"sv)).redirect_url.has_value());
    EXPECT(!evaluate(with_host_access, request("https://a.com/r"sv, ResourceType::XMLHttpRequest, "https://other.com/"sv)).redirect_url.has_value());
    EXPECT(evaluate(with_host_access, request("https://a.com/r"sv, ResourceType::XMLHttpRequest, "https://a.com/"sv)).redirect_url.has_value());
    EXPECT_EQ(evaluate(with_host_access, request("https://a.com/h"sv)).request_headers.size(), 1u);

    // Never what another extension asks for, nor the Web Store.
    Vector<ExtensionRules> all;
    all.append(extension("aaaabbbbccccddddaaaabbbbccccdddd"sv, { block }));
    EXPECT(!evaluate(all, request("https://a.com/b"sv, ResourceType::XMLHttpRequest, "chrome-extension://ppppoooonnnnmmmmppppoooonnnnmmmm/"sv)).blocked);
    EXPECT(evaluate(all, request("https://a.com/b"sv, ResourceType::XMLHttpRequest, "chrome-extension://aaaabbbbccccddddaaaabbbbccccdddd/"sv)).blocked);
    EXPECT(!evaluate(all, request("https://chromewebstore.google.com/b"sv)).blocked);
}

TEST_CASE(rulesets_and_updates)
{
    Vector<String> warnings;
    auto rules = parse_ruleset(R"([{"id":1,"action":{"type":"block"},"condition":{"urlFilter":"a"}},{"id":2,"action":{"type":"nope"},"condition":{}},{"id":1,"action":{"type":"block"},"condition":{}},"x"])"sv, warnings);
    EXPECT_EQ(rules.size(), 1u);
    EXPECT_EQ(warnings.size(), 3u);
    warnings.clear();
    EXPECT(parse_ruleset("{}"sv, warnings).is_empty());
    EXPECT_EQ(warnings.size(), 1u);

    Vector<Rule> current;
    auto updated = update_rules(current, json(R"({"addRules":[{"id":1,"action":{"type":"block"},"condition":{}},{"id":2,"action":{"type":"allow"},"condition":{}}]})"sv), RuleSource::Dynamic);
    EXPECT(!updated.is_error());
    current = updated.release_value();
    EXPECT_EQ(current.size(), 2u);

    // Removals come first, so an ID can be replaced.
    updated = update_rules(current, json(R"({"removeRuleIds":[1,99],"addRules":[{"id":1,"priority":4,"action":{"type":"block"},"condition":{}}]})"sv), RuleSource::Dynamic);
    EXPECT(!updated.is_error());
    EXPECT_EQ(updated.value().size(), 2u);
    bool found = false;
    for (auto const& rule : updated.value()) {
        if (rule.id() == 1)
            found = rule.priority() == 4;
    }
    EXPECT(found);

    // Errors change nothing.
    EXPECT(update_rules(current, json(R"({"addRules":[{"id":2,"action":{"type":"block"},"condition":{}}]})"sv), RuleSource::Dynamic).is_error());
    EXPECT(update_rules(current, json(R"({"addRules":[{"id":3,"action":{"type":"block"},"condition":{}},{"id":3,"action":{"type":"block"},"condition":{}}]})"sv), RuleSource::Dynamic).is_error());
    EXPECT(update_rules(current, json(R"({"addRules":"x"})"sv), RuleSource::Dynamic).is_error());
    EXPECT(update_rules(current, json(R"({"removeRuleIds":["1"]})"sv), RuleSource::Dynamic).is_error());
    EXPECT(update_rules(current, json(R"("x")"sv), RuleSource::Dynamic).is_error());
    EXPECT(update_rules(current, json(R"({"addRules":[{"id":5,"action":{"type":"block"},"condition":{"tabIds":[1]}}]})"sv), RuleSource::Dynamic).is_error());
    EXPECT(!update_rules(current, json(R"({})"sv), RuleSource::Dynamic).is_error());

    // Limits.
    StringBuilder many;
    many.append(R"({"addRules":[)"sv);
    for (size_t i = 1; i <= max_number_of_unsafe_session_rules + 1; ++i)
        many.appendff(R"({}{{"id":{},"action":{{"type":"redirect","redirect":{{"url":"https://r.com/"}}}},"condition":{{}}}})", i == 1 ? "" : ",", i);
    many.append("]}"sv);
    auto too_many = update_rules({}, json(many.string_view()), RuleSource::Session);
    EXPECT(too_many.is_error());
}

TEST_CASE(url_filter_matching_takes_linear_time)
{
    // Each star once, left to right: a filter like this took seconds per request when each star tried every split.
    auto long_url = url(MUST(String::formatted("https://example.com/{}", MUST(String::repeated('a', 2000)))));
    auto start = MonotonicTime::now();
    EXPECT(!url_filter_matches("*a*a*a*a*a*a*a*a*a*a*a*a*b"sv, long_url, false));
    EXPECT(!url_filter_matches("||example.com^*a*a*a*a*a*a*a*a*a*a*b|"sv, long_url, false));
    EXPECT(!url_filter_matches("|https://*a^*a*a*a*a*a*a*a*a*a*b"sv, long_url, false));
    EXPECT(url_filter_matches("*a*a*a*a*a*a*a*a*a*a*a*a*a"sv, long_url, false));
    EXPECT(MonotonicTime::now() - start < AK::Duration::from_seconds(1));

    // What the stars and anchors still mean.
    EXPECT(url_filter_matches("a*b*c"sv, url("https://x.com/abc"sv), false));
    EXPECT(url_filter_matches("a*b*c"sv, url("https://x.com/a-b-c"sv), false));
    EXPECT(!url_filter_matches("a*b*c"sv, url("https://x.com/a-c-b"sv), false));
    EXPECT(url_filter_matches("|https://x.com/*.js|"sv, url("https://x.com/a.js.js"sv), false));
    EXPECT(!url_filter_matches("|https://x.com/*.js|"sv, url("https://x.com/a.jsx"sv), false));
    EXPECT(url_filter_matches("/a*/b|"sv, url("https://x.com/a/b/c/b"sv), false));
    EXPECT(url_filter_matches("||b.example.com^*x"sv, url("https://a.b.example.com/x"sv), false));
    EXPECT(!url_filter_matches("||b.example.com^*x"sv, url("https://ab.example.com/x"sv), false));
    EXPECT(url_filter_matches("||example.com"sv, url("https://example.example.com/"sv), false));
    EXPECT(url_filter_matches("com^"sv, url("https://example.com"sv), false));
    EXPECT(url_filter_matches("/x^^"sv, url("https://example.com/x"sv), false));
    EXPECT(url_filter_matches("*"sv, url("https://example.com/"sv), false));
    EXPECT(url_filter_matches("|*x|"sv, url("https://example.com/x"sv), false));
    EXPECT(!url_filter_matches("|*x|"sv, url("https://example.com/xy"sv), false));
    EXPECT(url_filter_matches("^x^"sv, url("https://example.com/x/"sv), false));

    // Chrome's limit on the filter's length.
    auto long_filter = MUST(String::formatted(R"({{"id":1,"action":{{"type":"block"}},"condition":{{"urlFilter":"{}"}}}})", MUST(String::repeated('a', 2049))));
    EXPECT(parse_error(long_filter).contains("urlFilter"sv));
}

TEST_CASE(header_values_must_be_valid)
{
    // Chrome refuses a value that isn't a header value; RequestServer refuses requests with one.
    EXPECT(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"x-a","operation":"set","value":"a\r\nx-b: 1"}]},"condition":{}})"sv).contains("value"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"x-a","operation":"set","value":"a\nb"}]},"condition":{}})"sv).contains("value"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","responseHeaders":[{"header":"x-a","operation":"append","value":" a"}]},"condition":{}})"sv).contains("value"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"x-a","operation":"set","value":"a\t"}]},"condition":{}})"sv).contains("value"sv));
    EXPECT(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"x-a","operation":"set","value":"a\u0000b"}]},"condition":{}})"sv).contains("value"sv));
    EXPECT_EQ(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"x-a","operation":"set","value":"a b"}]},"condition":{}})"sv), "(parsed)"sv);
    EXPECT_EQ(parse_error(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"x-a","operation":"set","value":""}]},"condition":{}})"sv), "(parsed)"sv);

    // Appending nothing to a request header leaves it as it was, rather than ending it in ", ".
    auto append_nothing = rule(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"accept","operation":"append","value":""}]},"condition":{}})"sv);
    Vector<ExtensionRules> extensions;
    extensions.append(extension("e"sv, { append_nothing }));
    auto outcome = evaluate(extensions, request("https://a.com/"sv));
    Vector<Header> headers { { "Accept"_string, "text/html"_string } };
    apply_header_modifications(headers, outcome.request_headers, HeaderStage::Request);
    EXPECT_EQ(headers.size(), 1u);
    EXPECT_EQ(headers[0].value, "text/html"sv);
}

TEST_CASE(redirects_go_only_where_chrome_lets_them)
{
    for (auto target : { "file:///etc/passwd"sv, "about:blank"sv, "chrome://settings"sv, "ladybird://about"sv, "blob:https://a.com/x"sv, "view-source:https://a.com/"sv })
        EXPECT(parse_error(MUST(String::formatted(R"({{"id":1,"action":{{"type":"redirect","redirect":{{"url":"{}"}}}},"condition":{{}}}})", target))).contains("redirect.url"sv));
    EXPECT_EQ(parse_error(R"({"id":1,"action":{"type":"redirect","redirect":{"url":"data:text/plain,x"}},"condition":{}})"sv), "(parsed)"sv);
    EXPECT_EQ(parse_error(R"({"id":1,"action":{"type":"redirect","redirect":{"url":"http://a.com/"}},"condition":{}})"sv), "(parsed)"sv);

    // A regexSubstitution is checked when it's made.
    auto to_file = rule(R"({"id":1,"action":{"type":"redirect","redirect":{"regexSubstitution":"file:///\\1"}},"condition":{"regexFilter":"^https://a\\.com/(.*)$"}})"sv);
    EXPECT(!to_file.redirect_url_for(request("https://a.com/etc/passwd"sv), "ext"sv).has_value());
    // Another extension's pages are never a target.
    auto to_other = rule(R"({"id":1,"action":{"type":"redirect","redirect":{"transform":{"scheme":"chrome-extension","host":"ppppoooonnnnmmmmppppoooonnnnmmmm"}}},"condition":{}})"sv);
    EXPECT(!to_other.redirect_url_for(request("https://a.com/x"sv), "aaaabbbbccccddddaaaabbbbccccdddd"sv).has_value());
    auto to_own = rule(R"({"id":1,"action":{"type":"redirect","redirect":{"transform":{"scheme":"chrome-extension","host":"aaaabbbbccccddddaaaabbbbccccdddd"}}},"condition":{}})"sv);
    EXPECT(to_own.redirect_url_for(request("https://a.com/x"sv), "aaaabbbbccccddddaaaabbbbccccdddd"sv).has_value());
}

TEST_CASE(opaque_initiators_need_access_to_every_host)
{
    auto headers = rule(R"({"id":1,"action":{"type":"modifyHeaders","requestHeaders":[{"header":"x-a","operation":"set","value":"1"}]},"condition":{}})"sv);
    auto redirect = rule(R"({"id":2,"action":{"type":"redirect","redirect":{"url":"https://r.com/"}},"condition":{"urlFilter":"r"}})"sv);
    auto opaque_request = [](StringView request_url) {
        auto made = request(request_url);
        made.initiator = URL::Origin::create_opaque();
        return made;
    };

    WebExtensions::MatchPatternSet a_com;
    a_com.add(MUST(WebExtensions::MatchPattern::parse("https://a.com/*"sv)));
    Vector<ExtensionRules> narrow;
    narrow.append({ .extension_id = "e"_string, .rules = { headers, redirect }, .all_actions_need_host_access = false, .host_permissions = a_com });
    EXPECT(evaluate(narrow, request("https://a.com/h"sv, ResourceType::XMLHttpRequest, "https://a.com/"sv)).request_headers.size() == 1);
    // A sandboxed frame's requests could come from any site.
    EXPECT(evaluate(narrow, opaque_request("https://a.com/h"sv)).request_headers.is_empty());
    EXPECT(!evaluate(narrow, opaque_request("https://a.com/r"sv)).redirect_url.has_value());

    Vector<ExtensionRules> broad;
    broad.append(extension("e"sv, { headers, redirect }));
    EXPECT_EQ(evaluate(broad, opaque_request("https://a.com/h"sv)).request_headers.size(), 1u);
    EXPECT(evaluate(broad, opaque_request("https://a.com/r"sv)).redirect_url.has_value());
}
