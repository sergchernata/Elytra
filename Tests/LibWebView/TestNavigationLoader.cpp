/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/HTML/BrowsingContext.h>
#include <LibWebView/CanonicalSessionHistoryEntry.h>
#include <LibWebView/NavigationLoader.h>

static Web::HTML::NavigationParamsDescriptor navigation_params_for(URL::URL const& url)
{
    return {
        .id = {},
        .navigable_id = {},
        .request = {},
        .response = {},
        .fetch_timing_info = {},
        .coop_enforcement_result = { .url = url, .origin = url.origin(), .opener_policy = {} },
        .reserved_environment = {},
        .origin = url.origin(),
        .policy_container = {},
        .opener_policy = {},
        .about_base_url = {},
        .agent_cluster_id = {},
        .new_browsing_context_group_id = {},
    };
}

TEST_CASE(response_document_uses_history_entry_url_when_response_url_list_is_empty)
{
    auto url = URL::Parser::basic_parse("https://ladybird.org/"sv).release_value();
    Web::HTML::NavigationPopulationRequest request {};
    request.history_entry.url = url;
    auto loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, move(request));
    loader->did_finish_navigation_params_creation({
        .navigation_params = navigation_params_for(url),
        .redirected_url = {},
        .classic_history_api_state = {},
        .replacement_document_state = {},
    });

    auto document = loader->response_document();
    EXPECT(document.has_value());
    if (document.has_value())
        EXPECT_EQ(document->response_url, url);
}

TEST_CASE(response_document_uses_last_response_url_after_redirects)
{
    auto initial_url = URL::Parser::basic_parse("https://ladybird.org/"sv).release_value();
    auto final_url = URL::Parser::basic_parse("https://example.com/"sv).release_value();
    Web::HTML::NavigationPopulationRequest request {};
    request.history_entry.url = initial_url;
    auto loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, move(request));
    auto params = navigation_params_for(initial_url);
    params.response.url_list = { initial_url, final_url };
    loader->did_finish_navigation_params_creation({
        .navigation_params = move(params),
        .redirected_url = {},
        .classic_history_api_state = {},
        .replacement_document_state = {},
    });

    auto document = loader->response_document();
    EXPECT(document.has_value());
    if (document.has_value())
        EXPECT_EQ(document->response_url, final_url);
}

// A process reports the URLs its fetch reached. One naming an extension's file it can't have reached would give the
// renderer's own document an extension's origin, with its process and its API; such a navigation gets an error document.
static void expect_error_document_for(URL::URL const& admitted_url, Vector<URL::URL> response_url_list, Optional<URL::URL> redirected_url = {})
{
    Web::HTML::NavigationPopulationRequest request {};
    request.history_entry.url = admitted_url;
    auto loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, move(request));
    auto params = navigation_params_for(response_url_list.last());
    params.response.url_list = move(response_url_list);
    params.response.body = MUST(ByteBuffer::copy("<script>chrome.storage.local.get()</script>"sv.bytes()));
    loader->did_finish_navigation_params_creation({
        .navigation_params = move(params),
        .redirected_url = move(redirected_url),
        .classic_history_api_state = {},
        .replacement_document_state = {},
    });

    auto document = loader->response_document();
    EXPECT(document.has_value());
    if (!document.has_value())
        return;
    EXPECT(document->is_inline_content);
    EXPECT(document->origin.is_opaque());
}

TEST_CASE(response_document_refuses_a_made_up_extension_url)
{
    auto site = URL::Parser::basic_parse("https://evil.example/"sv).release_value();
    auto extension_page = URL::Parser::basic_parse("chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/x.html"sv).release_value();
    auto other_extension_page = URL::Parser::basic_parse("chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/y.html"sv).release_value();

    // A site's navigation that the process says reached an extension's page.
    expect_error_document_for(site, { extension_page });
    // Through a redirect, which the browser checks against the extension, and without one here, refuses.
    expect_error_document_for(site, { site, extension_page }, extension_page);
    // An extension's URL anywhere but at the end: an extension's file never redirects.
    expect_error_document_for(site, { extension_page, site });
    // A navigation to one extension page that ends at another.
    expect_error_document_for(extension_page, { other_extension_page });
    // Even the admitted URL: only the browser reads an extension's file, and this one is no installed extension's.
    expect_error_document_for(extension_page, { extension_page });
}

// A policy container's referrer policy is never empty: one a renderer says is would crash the process that hosts the
// document, as the policy is applied.
TEST_CASE(a_made_up_empty_referrer_policy_becomes_the_default)
{
    auto url = URL::Parser::basic_parse("https://ladybird.org/"sv).release_value();
    Web::HTML::NavigationPopulationRequest request {};
    request.history_entry.url = url;
    auto loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, move(request));
    auto params = navigation_params_for(url);
    params.policy_container.referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::EmptyString;
    loader->did_finish_navigation_params_creation({
        .navigation_params = move(params),
        .redirected_url = {},
        .classic_history_api_state = {},
        .replacement_document_state = {},
    });

    auto const& result = loader->result();
    auto const* navigation_params = result.navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    EXPECT(navigation_params);
    if (navigation_params)
        EXPECT(navigation_params->policy_container.referrer_policy == Web::ReferrerPolicy::DEFAULT_REFERRER_POLICY);
}

// A process reports the URLs its fetch reached, and an about:blank or about:srcdoc document takes the origin of the
// navigation's initiator or of its entry. One the UI process did not admit to such a URL would give the renderer's own
// document any origin it named, so it gets an error document; one it did admit gets the body the browser makes.
static URL::URL parse(StringView string)
{
    return URL::Parser::basic_parse(string).release_value();
}

static constexpr auto forged_body = "<script>alert(document.domain)</script>"sv;

static Web::HTML::NavigationPopulationResult result_reaching(Vector<URL::URL> url_list, Optional<URL::URL> redirected_url = {}, Optional<Web::HTML::SessionHistoryDocumentStateDescriptor> replacement_document_state = {})
{
    auto params = navigation_params_for(url_list.last());
    params.response.url_list = move(url_list);
    params.response.headers.append({ "Content-Type"sv, "text/html"sv });
    params.response.body = MUST(ByteBuffer::copy(forged_body.bytes()));
    return {
        .navigation_params = move(params),
        .redirected_url = move(redirected_url),
        .classic_history_api_state = {},
        .replacement_document_state = move(replacement_document_state),
    };
}

static Web::HTML::NavigationPopulationRequest request_for(URL::URL const& url, Optional<URL::Origin> initiator_origin, Optional<URL::Origin> origin = {}, Web::HTML::DocumentResource resource = {})
{
    Web::HTML::NavigationPopulationRequest request {};
    request.history_entry.url = url;
    request.history_entry.document_state.initiator_origin = move(initiator_origin);
    request.history_entry.document_state.origin = move(origin);
    request.history_entry.document_state.resource = move(resource);
    return request;
}

static bool is_error_document(WebView::NavigationLoader const& loader)
{
    auto document = loader.response_document();
    return document.has_value() && document->is_inline_content && document->origin.is_opaque();
}

static Optional<ByteString> body_of(WebView::NavigationLoader const& loader)
{
    auto const* params = loader.result().navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    if (!params || !params->response.body.has<ByteBuffer>())
        return {};
    return ByteString { params->response.body.get<ByteBuffer>().bytes() };
}

TEST_CASE(an_about_blank_response_needs_an_about_blank_navigation)
{
    auto victim = parse("https://victim.example/"sv).origin();
    auto extension = parse("chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/"sv).origin();
    for (auto const& initiator : { victim, extension }) {
        auto loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, request_for(parse("https://evil.example/b"sv), initiator));
        loader->did_finish_navigation_params_creation(result_reaching({ URL::about_blank() }));
        EXPECT(is_error_document(*loader));

        auto srcdoc_loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, request_for(parse("https://evil.example/b"sv), initiator, initiator));
        srcdoc_loader->did_finish_navigation_params_creation(result_reaching({ URL::about_srcdoc() }));
        EXPECT(is_error_document(*srcdoc_loader));
    }
}

TEST_CASE(an_about_blank_document_has_the_browsers_body)
{
    auto victim = parse("https://victim.example/"sv).origin();
    auto loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, request_for(URL::about_blank(), victim, victim));
    loader->did_finish_navigation_params_creation(result_reaching({ URL::about_blank() }));
    auto document = loader->response_document();
    EXPECT(document.has_value() && !document->is_inline_content);
    if (document.has_value())
        EXPECT(document->origin.is_same_origin(victim));
    EXPECT_EQ(body_of(*loader), ByteString {});
}

TEST_CASE(an_about_srcdoc_document_has_its_entrys_resource)
{
    auto victim = parse("https://victim.example/"sv).origin();
    auto loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, request_for(URL::about_srcdoc(), victim, victim, Utf16String::from_utf8("<p>parent's</p>"sv)));
    loader->did_finish_navigation_params_creation(result_reaching({ URL::about_srcdoc() }));
    auto document = loader->response_document();
    EXPECT(document.has_value() && !document->is_inline_content);
    EXPECT_EQ(body_of(*loader), "<p>parent's</p>"sv);
}

// An extension's origin goes only to a document of an installed extension's, which this test has none of.
TEST_CASE(an_about_blank_document_of_an_unknown_extensions_origin_is_refused)
{
    auto extension = parse("chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/"sv).origin();
    auto loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, request_for(URL::about_blank(), extension, extension));
    loader->did_finish_navigation_params_creation(result_reaching({ URL::about_blank() }));
    EXPECT(is_error_document(*loader));
}

// A redirect's new document state has no initiator origin, and the old one's origin: a process reports it, but doesn't
// choose its origins.
TEST_CASE(a_redirects_document_state_has_no_initiator_of_the_renderers_choosing)
{
    auto extension = parse("chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/"sv).origin();
    auto victim = parse("https://victim.example/"sv).origin();
    Web::HTML::SessionHistoryDocumentStateDescriptor forged {};
    forged.id = Web::HTML::CrossProcessId { .namespace_id = 9, .local_id = 4242 };
    forged.initiator_origin = extension;
    forged.origin = victim;

    auto admitted = parse("https://evil.example/"sv);
    auto redirected = parse("https://evil.example/c"sv);
    auto loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, request_for(admitted, URL::Origin::create_opaque()));
    loader->did_finish_navigation_params_creation(result_reaching({ admitted, redirected }, redirected, forged));
    auto const& document_state = loader->request().history_entry.document_state;
    EXPECT(!document_state.initiator_origin.has_value());
    EXPECT(!document_state.origin.has_value());

    // Nor does the response then become an about:blank document of the forged initiator's.
    loader = WebView::NavigationLoader::create(WebView::IsPrivate::No, request_for(admitted, URL::Origin::create_opaque()));
    loader->did_finish_navigation_params_creation(result_reaching({ admitted, URL::about_blank() }, URL::about_blank(), forged));
    EXPECT(is_error_document(*loader));
}

// A history step's population is the entry the UI process holds, whatever the process populating it echoes.
static NonnullRefPtr<WebView::CanonicalSessionHistoryEntry> entry_for(URL::URL const& url, Optional<URL::Origin> initiator_origin, Optional<URL::Origin> origin)
{
    Web::HTML::SessionHistoryEntryDescriptor descriptor {};
    descriptor.url = url;
    descriptor.document_state.id = Web::HTML::CrossProcessId { .namespace_id = 9, .local_id = 77 };
    descriptor.document_state.initiator_origin = move(initiator_origin);
    descriptor.document_state.origin = move(origin);
    return MUST(WebView::CanonicalSessionHistoryEntry::create_from_descriptor(descriptor));
}

TEST_CASE(a_history_step_takes_the_entrys_document_state)
{
    auto extension = parse("chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/"sv).origin();
    auto victim = parse("https://victim.example/"sv).origin();
    auto evil = parse("https://evil.example/"sv).origin();

    // The entry of a site's page, which the process says an extension's page or another site's initiated.
    auto entry = entry_for(parse("https://evil.example/a"sv), evil, evil);
    for (auto const& forged_initiator : { extension, victim }) {
        auto loader = WebView::NavigationLoader::create_for_history_step(WebView::IsPrivate::No, request_for(parse("https://evil.example/a"sv), forged_initiator, forged_initiator), *entry);
        EXPECT(loader->request().history_entry.document_state.initiator_origin->is_same_origin(evil));
        loader->did_finish_navigation_params_creation(result_reaching({ URL::about_blank() }));
        EXPECT(is_error_document(*loader));
    }

    // An about:blank entry of the site's: its document is the site's, and empty.
    auto blank_entry = entry_for(URL::about_blank(), evil, evil);
    auto loader = WebView::NavigationLoader::create_for_history_step(WebView::IsPrivate::No, request_for(URL::about_blank(), victim, victim), *blank_entry);
    loader->did_finish_navigation_params_creation(result_reaching({ URL::about_blank() }));
    auto document = loader->response_document();
    EXPECT(document.has_value() && document->origin.is_same_origin(evil));
    EXPECT_EQ(body_of(*loader), ByteString {});

    // An about:srcdoc entry's document is its resource.
    Web::HTML::SessionHistoryEntryDescriptor srcdoc_descriptor {};
    srcdoc_descriptor.url = URL::about_srcdoc();
    srcdoc_descriptor.document_state.id = Web::HTML::CrossProcessId { .namespace_id = 9, .local_id = 78 };
    srcdoc_descriptor.document_state.initiator_origin = evil;
    srcdoc_descriptor.document_state.origin = evil;
    srcdoc_descriptor.document_state.resource = Utf16String::from_utf8("<p>kept</p>"sv);
    auto srcdoc_entry = MUST(WebView::CanonicalSessionHistoryEntry::create_from_descriptor(srcdoc_descriptor));
    loader = WebView::NavigationLoader::create_for_history_step(WebView::IsPrivate::No, request_for(URL::about_srcdoc(), extension, extension, Utf16String::from_utf8("<script>x</script>"sv)), *srcdoc_entry);
    loader->did_finish_navigation_params_creation(result_reaching({ URL::about_srcdoc() }));
    document = loader->response_document();
    EXPECT(document.has_value() && document->origin.is_same_origin(evil));
    EXPECT_EQ(body_of(*loader), "<p>kept</p>"sv);
}
