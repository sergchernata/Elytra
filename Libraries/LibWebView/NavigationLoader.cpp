/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/CharacterTypes.h>
#include <LibCore/File.h>
#include <LibCore/MimeData.h>
#include <LibCore/System.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>
#include <LibWebCommon/HTML/BrowsingContext.h>
#include <LibWebCommon/HTML/CrossOrigin/OpenerPolicyEnforcement.h>
#include <LibWebCommon/HTML/EmbedderPolicy.h>
#include <LibWebCommon/HTML/NavigationParamsDescriptor.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/NavigationLoader.h>
#include <LibWebView/RequestServerManager.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#attempt-to-populate-the-history-entry's-document
// The document that the task queued by step 5 creates, if it creates one.
Optional<NavigationLoader::ResponseDocument> NavigationLoader::response_document() const
{
    auto const& navigation_params = result().navigation_params;

    // 3. If navigationParams is a non-fetch scheme navigation params:
    //    1. Set entry's document state's document to the result of running attempt to create a non-fetch scheme
    //       document given navigationParams.
    // NB: That hands the URL off to external software, which creates no document.
    if (navigation_params.has<Web::HTML::NonFetchSchemeNavigationParamsDescriptor>())
        return {};

    // 4. Otherwise, if any of the following are true:
    //    - navigationParams is null;
    //    [...]
    //    then:
    //    1. Set entry's document state's document to the result of creating a document for inline content that
    //       doesn't have a DOM, given navigable, null, navTimingType, and userInvolvement. The inline content
    //       should indicate to the user the sort of error that occurred.
    // NB: That document is created with the new opaque origin the result carries to the process creating it, a new
    //     opener policy enforcement result, and about:error as its URL.
    if (navigation_params.has<Web::HTML::NavigationParamsNullOrError>()) {
        auto origin = *result().inline_content_origin;
        return ResponseDocument {
            .is_inline_content = true,
            .coop_enforcement_result = { .url = URL::about_error(), .origin = origin, .opener_policy = {} },
            .response_url = URL::about_error(),
            .request_current_url = {},
            .origin = origin,
            .opener_policy = {},
            .embedder_policy = {},
            .final_sandboxing_flag_set = {},
            .environment_id = {},
            .document_id = {},
        };
    }

    auto const& fetched_navigation_params = navigation_params.get<Web::HTML::NavigationParamsDescriptor>();

    // FIXME: 5. Otherwise, if navigationParams's response has a `Content-Disposition` header specifying the
    //           attachment disposition type: [...]
    //        A download creates no document.

    // 6. Otherwise, if navigationParams's response's status is not 204 and is not 205, then set entry's
    //    document state's document to the result of loading a document given navigationParams,
    //    sourceSnapshotParams, and entry's document state's initiator origin.
    if (fetched_navigation_params.response.status == 204 || fetched_navigation_params.response.status == 205)
        return {};
    return ResponseDocument {
        .is_inline_content = false,
        .coop_enforcement_result = fetched_navigation_params.coop_enforcement_result,
        // The COOP enforcement result still describes the source document until an opener policy is enforced.
        // Placement uses the response's final URL, including any redirects.
        .response_url = fetched_navigation_params.response.url_list.is_empty() ? m_request.history_entry.url : fetched_navigation_params.response.url_list.last(),
        .request_current_url = fetched_navigation_params.request.has_value() && !fetched_navigation_params.request->url_list.is_empty()
            ? Optional<URL::URL> { fetched_navigation_params.request->url_list.last() }
            : Optional<URL::URL> {},
        .origin = fetched_navigation_params.origin,
        .opener_policy = fetched_navigation_params.opener_policy,
        .embedder_policy = fetched_navigation_params.policy_container.embedder_policy,
        .final_sandboxing_flag_set = fetched_navigation_params.final_sandboxing_flag_set,
        .environment_id = {},
        .document_id = {},
        .permissions_policy = fetched_navigation_params.permissions_policy,
    };
}

static Web::HTML::NavigationResponseBodyHandle* response_body_handle(Web::HTML::NavigationPopulationResult& result)
{
    if (!result.navigation_params.has<Web::HTML::NavigationParamsDescriptor>())
        return nullptr;
    auto& response = result.navigation_params.get<Web::HTML::NavigationParamsDescriptor>().response;
    return response.body.get_pointer<Web::HTML::NavigationResponseBodyHandle>();
}

// NB: The process populating a history entry's document echoes the entry in its request. Its document state names the
//     origins an about:blank or about:srcdoc document takes, and the requester an extension's file is read for, and its
//     resource is an about:srcdoc document's body: the UI process takes the entry it holds instead.
NonnullRefPtr<NavigationLoader> NavigationLoader::create_for_history_step(IsPrivate is_private, Web::HTML::NavigationPopulationRequest request, CanonicalSessionHistoryEntry const& target_entry)
{
    request.history_entry = Web::HTML::create_pending_session_history_entry_descriptor(target_entry.descriptor());
    return create(is_private, move(request));
}

NavigationLoader::~NavigationLoader()
{
    release_response_body();
}

static bool is_extension_url(URL::URL const& url)
{
    return url.scheme() == "chrome-extension"sv;
}

// The policies an extension's pages run under, as a response's Content-Security-Policy headers would give them.
// https://w3c.github.io/webappsec-csp/#parse-serialized-policy
static Web::ContentSecurityPolicy::SerializedPolicy serialized_policy_for_extension(StringView policy, URL::Origin const& self_origin)
{
    Vector<Web::ContentSecurityPolicy::Directives::SerializedDirective> directives;
    for (auto token : policy.split_view(';')) {
        token = token.trim_whitespace();
        if (token.is_empty() || !all_of(token, is_ascii))
            continue;
        auto parts = token.split_view_if(is_ascii_space);
        auto name = Utf16FlyString::from_utf8(parts.first().to_ascii_lowercase_string());
        if (any_of(directives, [&](auto const& directive) { return directive.name == name; }))
            continue;
        Vector<Utf16String> value;
        for (size_t i = 1; i < parts.size(); ++i)
            value.append(Utf16String::from_utf8(parts[i]));
        directives.append({ .name = move(name), .value = move(value) });
    }
    return {
        .directives = move(directives),
        .disposition = Web::Bindings::SecurityPolicyViolationEventDisposition::Enforce,
        .source = Web::ContentSecurityPolicy::PolicySource::Header,
        .self_origin = self_origin,
        .pre_parsed_policy_string = MUST(String::from_utf8(policy)),
    };
}

// NB: The process that fetched a navigation's response reports the URLs it reached, its headers, its policies and its
//     body, and the response's URL gives the document its origin. A document of an extension's origin runs in the
//     extension's process with the extension's API, so no process may make one up: a response at an extension's URL is
//     one the browser reads from the extension's files itself, with the policies the browser gives the extension's
//     pages. An extension's file never redirects, so its URL can only end the response's URL list, and a navigation the
//     UI process admitted to one ends at that same URL. A navigation the UI process admitted elsewhere that redirects
//     to an extension's file reaches what the extension makes web accessible to both the redirecting site and the
//     navigation's requester, as Chrome allows.
//     Returns false when the result names an extension's URL it can't have reached, or a file the browser won't serve.
static bool take_extension_response_from_the_browser(IsPrivate is_private, Web::HTML::NavigationPopulationResult& result, URL::URL const& admitted_url, Optional<URL::Origin> const& requester, bool& read_extension_file)
{
    auto* navigation_params = result.navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    auto names_an_extension_url = is_extension_url(admitted_url) || (result.redirected_url.has_value() && is_extension_url(*result.redirected_url));
    if (navigation_params) {
        names_an_extension_url |= any_of(navigation_params->response.url_list, is_extension_url);
        if (navigation_params->request.has_value())
            names_an_extension_url |= any_of(navigation_params->request->url_list, is_extension_url);
    }
    if (!names_an_extension_url)
        return true;
    // Extensions don't run in private windows, nor do their pages open there.
    if (is_private == IsPrivate::Yes)
        return !navigation_params && (!result.redirected_url.has_value() || !is_extension_url(*result.redirected_url));

    // An error document has an opaque origin of the UI process's making.
    if (!navigation_params)
        return !result.redirected_url.has_value() || !is_extension_url(*result.redirected_url);

    auto const& response_url_list = navigation_params->response.url_list;
    auto final_url = response_url_list.is_empty() ? admitted_url : response_url_list.last();
    if (!is_extension_url(final_url))
        return false;
    auto ends_at_final_url = [&](Vector<URL::URL> const& url_list) {
        if (url_list.is_empty())
            return true;
        for (size_t i = 0; i + 1 < url_list.size(); ++i) {
            if (is_extension_url(url_list[i]))
                return false;
        }
        return url_list.last().equals(final_url, URL::ExcludeFragment::Yes);
    };
    if (!ends_at_final_url(response_url_list))
        return false;
    if (navigation_params->request.has_value() && !ends_at_final_url(navigation_params->request->url_list))
        return false;
    if (result.redirected_url.has_value() && !result.redirected_url->equals(final_url, URL::ExcludeFragment::Yes))
        return false;
    auto is_redirect = !admitted_url.equals(final_url, URL::ExcludeFragment::Yes);
    if (is_redirect && is_extension_url(admitted_url))
        return false;

    if (!Application::is_initialized())
        return false;
    auto const& registry = Application::extension_registry();
    auto extension_id = final_url.serialized_host();
    auto path = final_url.file_path();
    auto const* extension = registry.find(extension_id);
    if (!extension)
        return false;
    if (is_redirect && (!extension->may_be_loaded_by(path, admitted_url.origin()) || (requester.has_value() && !extension->may_be_loaded_by(path, requester))))
        return false;

    auto file = registry.open_file(extension_id, path, is_redirect ? Optional<URL::Origin> { admitted_url.origin() } : requester);
    if (file.is_error())
        return false;
    auto stream = Core::File::adopt_fd(file.value().take_fd(), Core::File::OpenMode::Read);
    if (stream.is_error())
        return false;
    static constexpr size_t maximum_size = 64 * MiB;
    auto stat = Core::System::fstat(stream.value()->fd());
    if (stat.is_error() || !S_ISREG(stat.value().st_mode) || static_cast<size_t>(stat.value().st_size) > maximum_size)
        return false;
    auto body = stream.value()->read_until_eof();
    if (body.is_error() || body.value().size() > maximum_size)
        return false;

    NavigationLoader::discard(is_private, result);
    auto& response = navigation_params->response;
    response.status = 200;
    response.status_message = "OK"sv;
    response.network_error_message.clear();
    response.body = body.release_value();

    // The headers ResourceLoader gives an extension's file, and the policies they make.
    response.headers.clear();
    response.headers.append({ "Access-Control-Allow-Origin"sv, "*"sv });
    response.headers.append({ "Content-Type"sv, Core::guess_mime_type_based_on_filename(path) });
    // https://html.spec.whatwg.org/multipage/browsers.html#creating-a-policy-container-from-a-fetch-response
    // NB: The UI process obtains the embedder policy from these headers (obtain_the_response_policies()).
    navigation_params->policy_container = {
        .csp_list = {},
        .embedder_policy = {},
        .referrer_policy = Web::ReferrerPolicy::DEFAULT_REFERRER_POLICY,
        .force_load_at_top = false,
    };
    // The sandboxing flags are the navigable's (determine_the_origin_of_the_response()) and the CSP's, not the
    // renderer's. A page the manifest lists as sandboxed runs under the sandbox CSP, with an opaque origin, as in Chrome.
    navigation_params->final_sandboxing_flag_set = {};
    auto is_sandboxed_page = extension->is_sandboxed_page(path);
    auto policies = is_sandboxed_page ? Vector<String> { extension->manifest().sandbox_csp() } : extension->content_security_policies();
    for (auto const& policy : policies) {
        response.headers.append({ "Content-Security-Policy"sv, policy.to_byte_string() });
        auto serialized_policy = serialized_policy_for_extension(policy, final_url.origin());
        for (auto const& directive : serialized_policy.directives) {
            if (directive.name == "sandbox"sv)
                navigation_params->final_sandboxing_flag_set |= Web::HTML::parse_a_sandboxing_directive(directive.value.span());
        }
        navigation_params->policy_container.csp_list.append(move(serialized_policy));
    }
    if (is_sandboxed_page)
        navigation_params->final_sandboxing_flag_set |= Web::HTML::SandboxingFlagSet::SandboxedOrigin;

    // The referrer is the requester's origin, which the UI process holds, rather than what the renderer says.
    if (navigation_params->request.has_value()) {
        auto referrer = requester.has_value() && !requester->is_opaque() ? URL::Parser::basic_parse(requester->serialize()) : Optional<URL::URL> {};
        navigation_params->request->referrer = referrer.has_value() ? Web::Fetch::Infrastructure::RequestReferrerType { referrer.release_value() } : Web::Fetch::Infrastructure::RequestReferrer::NoReferrer;
    }
    read_extension_file = true;
    return true;
}

// NB: An about:blank document takes the origin of the navigation's initiator, and an about:srcdoc one its entry's, so a
//     process may report only a response at such a URL the UI process admitted a navigation to. Neither redirects. The
//     body is the browser's too: about:blank's is empty, and about:srcdoc's is the entry's resource.
//     Returns false when the result names such a URL the navigation wasn't to.
static bool take_about_response_from_the_browser(IsPrivate is_private, Web::HTML::NavigationPopulationResult& result, Web::HTML::PendingSessionHistoryEntryDescriptor const& admitted_entry)
{
    auto* navigation_params = result.navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    if (!navigation_params)
        return true;
    auto const& response_url_list = navigation_params->response.url_list;
    auto final_url = response_url_list.is_empty() ? admitted_entry.url : response_url_list.last();
    auto is_blank = Web::HTML::url_matches_about_blank(final_url);
    auto is_srcdoc = Web::HTML::url_matches_about_srcdoc(final_url);
    if (!is_blank && !is_srcdoc)
        return true;

    auto is_admitted_url = [&](URL::URL const& url) {
        return is_blank ? Web::HTML::url_matches_about_blank(url) : Web::HTML::url_matches_about_srcdoc(url);
    };
    if (!is_admitted_url(admitted_entry.url) || response_url_list.size() > 1 || result.redirected_url.has_value() || result.replacement_document_state.has_value())
        return false;
    if (navigation_params->request.has_value() && (navigation_params->request->url_list.size() > 1 || !all_of(navigation_params->request->url_list, is_admitted_url)))
        return false;

    NavigationLoader::discard(is_private, result);
    auto& response = navigation_params->response;
    response.status = 200;
    response.status_message = "OK"sv;
    response.network_error_message.clear();
    response.headers.clear();
    if (is_blank) {
        response.headers.append({ "Content-Type"sv, "text/html;charset=utf-8"sv });
        response.body = ByteBuffer {};
    } else {
        // https://html.spec.whatwg.org/multipage/browsing-the-web.html#create-navigation-params-from-a-srcdoc-resource
        response.headers.append({ "Content-Type"sv, "text/html"sv });
        auto const* srcdoc = admitted_entry.document_state.resource.get_pointer<Utf16String>();
        if (srcdoc) {
            auto utf8 = srcdoc->to_utf8();
            response.body = MUST(ByteBuffer::copy(utf8.bytes()));
        } else {
            response.body = ByteBuffer {};
        }
    }
    return true;
}

void NavigationLoader::did_finish_navigation_params_creation(Web::HTML::NavigationPopulationResult result)
{
    VERIFY(!m_result.has_value());
    VERIFY(!m_response_body_request);
    VERIFY(!m_completion_steps);

    // NB: The process that fetched the response reports the URLs it reached. Fetching never redirects a request to a
    //     file: URL, so one among them on a navigation the UI process did not admit to a file: URL is made up: the
    //     document it would create could read local files. Such a navigation creates an error document instead.
    if (m_request.history_entry.url.scheme() != "file"sv) {
        auto is_file_url = [](URL::URL const& url) { return url.scheme() == "file"sv; };
        auto names_a_local_file = result.redirected_url.has_value() && is_file_url(*result.redirected_url);
        if (auto const* navigation_params = result.navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>()) {
            names_a_local_file |= any_of(navigation_params->response.url_list, is_file_url);
            if (navigation_params->request.has_value())
                names_a_local_file |= any_of(navigation_params->request->url_list, is_file_url);
        }
        if (names_a_local_file) {
            discard(m_is_private, result);
            result.navigation_params = Web::HTML::NavigationParamsNullOrError {};
            result.redirected_url.clear();
        }
    }

    // NB: A redirect replaces the entry's document state with a new one of the old one's fields but for its initiator
    //     origin and about base URL, which it has none of, and its resource, which it may clear (create navigation params
    //     by fetching, steps 21.19-20). The process reports that it did, and the UI process makes the new document state.
    if (result.replacement_document_state.has_value()) {
        if (!result.redirected_url.has_value()) {
            result.replacement_document_state.clear();
        } else {
            auto replacement = m_request.history_entry.document_state;
            replacement.id = result.replacement_document_state->id;
            replacement.initiator_origin.clear();
            replacement.about_base_url.clear();
            replacement.reload_pending = false;
            replacement.nested_histories.clear();
            if (result.replacement_document_state->resource.has<Empty>())
                replacement.resource = Empty {};
            result.replacement_document_state = move(replacement);
        }
    }

    // NB: The URL and requester the UI process admitted, before the result's replacement document state.
    if (!take_extension_response_from_the_browser(m_is_private, result, m_request.history_entry.url, extension_file_requester(), m_response_is_an_extension_file)
        || !take_about_response_from_the_browser(m_is_private, result, m_request.history_entry)) {
        discard(m_is_private, result);
        result.navigation_params = Web::HTML::NavigationParamsNullOrError {};
        result.redirected_url.clear();
        result.replacement_document_state.clear();
        m_response_is_an_extension_file = false;
    }

    // NB: A policy container's referrer policy is never the empty string; one a renderer says is would crash the process
    //     hosting the document as the policy applies (determine_requests_referrer() verifies it isn't).
    if (auto* navigation_params = result.navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>()) {
        if (navigation_params->policy_container.referrer_policy == Web::ReferrerPolicy::ReferrerPolicy::EmptyString)
            navigation_params->policy_container.referrer_policy = Web::ReferrerPolicy::DEFAULT_REFERRER_POLICY;
    }

    Web::HTML::apply_navigation_population_result(m_request, result);
    m_result = move(result);
    m_result->inline_content_origin = URL::Origin::create_opaque();
    determine_the_origin_of_the_response();
    refuse_an_extension_origin_the_browser_did_not_give();
}

// NB: A document of an extension's origin runs in the extension's process, with the extension's API. It is one of the
//     extension's files the browser read, or an about:blank or about:srcdoc document of the browser's making that takes
//     the extension's origin from the navigation's initiator or its entry, both of which the UI process holds. Those
//     run under the extension's CSP. A document of an extension the browser doesn't have, or any other, is refused.
void NavigationLoader::refuse_an_extension_origin_the_browser_did_not_give()
{
    auto* navigation_params = m_result->navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    if (!navigation_params)
        return;
    auto const& origin = navigation_params->origin;
    if (origin.is_opaque() || origin.scheme() != "chrome-extension"sv)
        return;
    auto const& response_url = navigation_params->response.url_list.is_empty() ? m_request.history_entry.url : navigation_params->response.url_list.last();
    if (m_response_is_an_extension_file && is_extension_url(response_url) && response_url.origin().is_same_origin(origin))
        return;

    Extension const* extension = nullptr;
    if (Application::is_initialized())
        extension = Application::extension_registry().find(origin.host().serialize());
    if (extension && (Web::HTML::url_matches_about_blank(response_url) || Web::HTML::url_matches_about_srcdoc(response_url))) {
        navigation_params->policy_container.csp_list.clear();
        for (auto const& policy : extension->content_security_policies())
            navigation_params->policy_container.csp_list.append(serialized_policy_for_extension(policy, origin));
        return;
    }

    discard(m_is_private, *m_result);
    m_result->navigation_params = Web::HTML::NavigationParamsNullOrError {};
    m_response_is_an_extension_file = false;
}

Optional<URL::Origin> NavigationLoader::extension_file_requester() const
{
    if (m_extension_file_requester.has_value())
        return *m_extension_file_requester;
    // NB: Only the UI process says the browser itself is the requester; a navigation without an initiator has the least.
    return m_request.history_entry.document_state.initiator_origin.value_or(URL::Origin::create_opaque());
}

// NB: The process that created the navigation params determined responseOrigin for its own checks. The UI process
//     runs the step again with what it holds, and the process creating the document takes the origin from here.
void NavigationLoader::determine_the_origin_of_the_response()
{
    auto* navigation_params = m_result->navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    if (!navigation_params)
        return;

    auto const& entry = m_request.history_entry;
    auto const& target_snapshot_params = m_request.target_snapshot_params;
    auto response_url = navigation_params->response.url_list.is_empty() ? entry.url : navigation_params->response.url_list.last();

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#create-navigation-params-from-a-srcdoc-resource
    if (Web::HTML::url_matches_about_srcdoc(response_url)) {
        // 3. Let responseOrigin be the result of determining the origin given response's URL, targetSnapshotParams's
        //    sandboxing flags, and entry's document state's origin.
        // AD-HOC: Determining the origin asserts that sourceOrigin is non-null for about:srcdoc, which the process that
        //         gave entry need not uphold. Such a document gets a new opaque origin.
        if (!entry.document_state.origin.has_value()) {
            navigation_params->origin = URL::Origin::create_opaque();
            return;
        }
        navigation_params->origin = Web::HTML::determine_the_origin(response_url, target_snapshot_params.sandboxing_flags, entry.document_state.origin);
        return;
    }

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#create-navigation-params-by-fetching
    // 10. Set finalSandboxFlags to the union of targetSnapshotParams's sandboxing flags and responsePolicyContainer's CSP
    //     list's CSP-derived sandboxing flags.
    // NB: The process that fetched the response created responsePolicyContainer, and its finalSandboxFlags hold the
    //     CSP-derived sandboxing flags. Sandboxing flags only ever make an origin opaque, so the UI process takes them.
    navigation_params->final_sandboxing_flag_set |= target_snapshot_params.sandboxing_flags;

    // 11. Set responseOrigin to the result of determining the origin given response's URL, finalSandboxFlags, and
    //     entry's document state's initiator origin.
    navigation_params->origin = Web::HTML::determine_the_origin(response_url, navigation_params->final_sandboxing_flag_set, entry.document_state.initiator_origin);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#create-navigation-params-by-fetching
// NB: The process that fetched the response can host another site, so the UI process obtains the response's opener and
//     embedder policies itself, from the headers that RequestServer gave it with the response.
void NavigationLoader::obtain_the_response_policies(CanonicalNavigable const& navigable)
{
    auto* navigation_params = m_result->navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    if (!navigation_params)
        return;

    auto const& response_url = navigation_params->response.url_list.is_empty() ? m_request.history_entry.url : navigation_params->response.url_list.last();

    // NB: A response that the fetching process read from its own HTTP memory cache has no RequestServer request, so
    //     its headers are the ones that process reported.
    auto header_list = m_response_header_list ? NonnullRefPtr { *m_response_header_list } : HTTP::HeaderList::create(navigation_params->response.headers);

    // 17. Let responseCOOP be a new opener policy.
    Web::HTML::OpenerPolicy response_coop;

    // 21. While true:
    //     2. If request's reserved client is null:
    //        1. Let topLevelCreationURL be currentURL.
    //        3. If navigable is not a top-level traversable:
    //           1. Let parentEnvironment be navigable's parent's active document's relevant settings object.
    //           2. Set topLevelCreationURL to parentEnvironment's top-level creation URL.
    // NB: The UI process runs these steps for the final response only.
    auto top_level_creation_url = response_url;
    if (!navigable.is_top_level_traversable())
        top_level_creation_url = navigable.parent()->active_document().relevant_global_object().relevant_settings_object().top_level_creation_url().value();

    // https://html.spec.whatwg.org/multipage/webappapis.html#secure-context
    // 2. If environment's top-level creation URL's scheme is "data", then return false.
    // 3. If the result of Is url potentially trustworthy? given environment's top-level creation URL is "Potentially
    //    Trustworthy", then return true.
    auto reserved_environment_is_a_non_secure_context = top_level_creation_url.scheme() == "data"sv
        || Web::SecureContexts::is_url_potentially_trustworthy(top_level_creation_url) == Web::SecureContexts::Trustworthiness::NotTrustworthy;

    //     9. Set responsePolicyContainer to the result of creating a policy container from a fetch response given
    //        response and request's reserved client.
    // NB: The document takes responsePolicyContainer unless the response's URL is local, when it inherits a policy
    //     container instead.
    if (!Web::Fetch::Infrastructure::is_local_url(response_url))
        navigation_params->policy_container.embedder_policy = Web::HTML::obtain_an_embedder_policy(header_list, reserved_environment_is_a_non_secure_context);

    //     12. If navigable is a top-level traversable:
    if (navigable.is_top_level_traversable()) {
        // 1. Set responseCOOP to the result of obtaining an opener policy given response and request's reserved client.
        response_coop = Web::HTML::obtain_an_opener_policy(header_list, reserved_environment_is_a_non_secure_context);

        // 2. Set coopEnforcementResult to the result of enforcing the response's opener policy given navigable's active
        //    browsing context, response's URL, responseOrigin, responseCOOP, coopEnforcementResult, and request's
        //    referrer.
        // NB: The fetching process enforced the opener policy of each response along the redirects, which the UI
        //     process does not see. Enforcing the final response's opener policy again, against navigable's active
        //     document, lets that process add a browsing context group switch but not withhold one. Only the switch
        //     flags are taken from this result.
        auto const& active_document = navigable.active_document();
        Web::HTML::OpenerPolicyEnforcementResult active_document_coop_enforcement_result {
            .needs_a_browsing_context_group_switch = false,
            .would_need_a_browsing_context_group_switch_due_to_report_only = false,
            .url = active_document.creation_url(),
            .origin = active_document.origin(),
            .opener_policy = active_document.opener_policy(),
            .current_context_is_navigation_source = false,
        };
        auto coop_enforcement_result = Web::HTML::enforce_a_responses_opener_policy(active_document.is_initial_about_blank(), response_url, navigation_params->origin, response_coop, active_document_coop_enforcement_result);
        navigation_params->coop_enforcement_result.opener_policy = response_coop;
        navigation_params->coop_enforcement_result.needs_a_browsing_context_group_switch |= coop_enforcement_result.needs_a_browsing_context_group_switch;
        navigation_params->coop_enforcement_result.would_need_a_browsing_context_group_switch_due_to_report_only |= coop_enforcement_result.would_need_a_browsing_context_group_switch_due_to_report_only;
    }

    navigation_params->opener_policy = response_coop;

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#initialise-the-document-object
    // 2. Let permissionsPolicy be the result of creating a permissions policy from a response given navigationParams's
    //    navigable's container, navigationParams's origin, and navigationParams's response.
    // NB: The UI process creates it here, from the response's headers, so that the process creating the document
    //     takes it with navigationParams.
    navigation_params->permissions_policy = navigable.create_a_permissions_policy_from_a_response(navigation_params->origin, header_list);
}

// https://html.spec.whatwg.org/multipage/browsers.html#check-a-navigation-response's-adherence-to-its-embedder-policy
// NB: response is only used for violation reports, which are not queued.
static bool check_a_navigation_responses_adherence_to_its_embedder_policy(CanonicalNavigable const& navigable, Web::HTML::EmbedderPolicy const& response_policy)
{
    // 1. If navigable is not a child navigable, then return true.
    auto const* container_document = navigable.container_document();
    if (!container_document)
        return true;

    // 2. Let parentPolicy be navigable's container document's policy container's embedder policy.
    auto const& parent_policy = container_document->embedder_policy();

    // FIXME: 3. If parentPolicy's report-only value is compatible with cross-origin isolation and responsePolicy's value
    //           is not, then queue a cross-origin embedder policy inheritance violation with response, "navigation",
    //           parentPolicy's report-only reporting endpoint, "reporting", and navigable's container document's
    //           relevant settings object.

    // 4. If parentPolicy's value is not compatible with cross-origin isolation or responsePolicy's value is compatible
    //    with cross-origin isolation, then return true.
    if (!Web::HTML::is_compatible_with_cross_origin_isolation(parent_policy.value) || Web::HTML::is_compatible_with_cross_origin_isolation(response_policy.value))
        return true;

    // FIXME: 5. Queue a cross-origin embedder policy inheritance violation with response, "navigation", parentPolicy's
    //           reporting endpoint, "enforce", and navigable's container document's relevant settings object.

    // 6. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#attempt-to-populate-the-history-entry's-document
// NB: The UI process checks the response's adherence to its embedder policy, as it holds navigable's container
//     document.
void NavigationLoader::check_the_responses_adherence_to_its_embedder_policy(CanonicalNavigable const& navigable)
{
    auto* navigation_params = m_result->navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    if (!navigation_params)
        return;

    // 5. Queue a global task on the navigation and traversal task source, given navigable's active window, to run these
    //    steps:
    //    4. Otherwise, if any of the following are true:
    //       [...]
    //       - navigationParams's reserved environment is non-null and the result of checking a navigation response's
    //         adherence to its embedder policy given navigationParams's response, navigable, and navigationParams's
    //         policy container's embedder policy is false; or
    //       [...]
    //       then:
    //       1. Set entry's document state's document to the result of creating a document for inline content that
    //          doesn't have a DOM, given navigable, null, navTimingType, and userInvolvement. The inline content
    //          should indicate to the user the sort of error that occurred.
    // NB: The UI process checks this before the page that hosts the document queues the task, which creates that
    //     document for navigation params that are null.
    if (!navigation_params->reserved_environment.has_value()
        || check_a_navigation_responses_adherence_to_its_embedder_policy(navigable, navigation_params->policy_container.embedder_policy)) {
        return;
    }
    release_response_body();
    m_result->navigation_params = Web::HTML::NavigationParamsNullOrError { "Blocked by Cross-Origin-Embedder-Policy"_utf16 };
}

void NavigationLoader::acquire_response_body(Function<void(bool)> completion_steps)
{
    VERIFY(m_result.has_value());
    VERIFY(!m_response_body_request);
    VERIFY(!m_completion_steps);

    m_completion_steps = move(completion_steps);

    auto* body_handle = response_body_handle(*m_result);
    if (!body_handle) {
        did_acquire(true);
        return;
    }

    // The response lives in the RequestServer of the client that fetched it.
    auto request_server = RequestServerManager::the().instance_for_client(body_handle->request_server_client_id);
    if (!request_server) {
        did_acquire(false);
        return;
    }
    auto request = request_server->ui_client().adopt_request(
        body_handle->request_server_client_id,
        body_handle->request_server_request_id,
        Requests::RequestClient::TransferLease::Yes);
    if (!request) {
        did_acquire(false);
        return;
    }
    m_response_body_request_server = request_server;
    m_response_body_request_server_client_id = body_handle->request_server_client_id;
    m_response_body_request_server_request_id = body_handle->request_server_request_id;

    request->set_body_delivery_paused(true);
    set_response_body_callbacks(*request);
    m_response_body_request = request;
}

void NavigationLoader::set_response_body_callbacks(Requests::Request& request)
{
    auto weak_this = make_weak_ptr();
    request.set_unbuffered_request_callbacks(
        [weak_this](NonnullRefPtr<HTTP::HeaderList> response_header_list, Optional<u32>, Optional<String> const&, Optional<Core::ImmutableBytes>, Optional<u64>, Requests::CacheState) {
            if (auto* loader = weak_this.ptr()) {
                loader->m_response_header_list = move(response_header_list);
                loader->did_acquire(true);
            }
        },
        [](Requests::ResponseData) { },
        [](Core::ImmutableBytes) { },
        [weak_this](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError>) {
            if (auto* loader = weak_this.ptr())
                loader->did_acquire(false);
        });
}

void NavigationLoader::move_response_body_to(RequestServerInstance& request_server, Function<void(bool)> completion_steps)
{
    VERIFY(!m_completion_steps);
    m_completion_steps = move(completion_steps);

    if (request_server.is_retired() || (m_response_body_request_server && m_response_body_request_server->is_retired())) {
        did_acquire(false);
        return;
    }
    if (!m_response_body_request || !m_response_body_request_server || m_response_body_request_server.ptr() == &request_server) {
        did_acquire(true);
        return;
    }

    auto* body_handle = response_body_handle(*m_result);
    if (!body_handle) {
        did_acquire(true);
        return;
    }

    auto exported = m_response_body_request_server->ui_client().export_request(*m_response_body_request);
    if (exported.is_error()) {
        warnln("Unable to move a navigation response between RequestServers: {}", exported.error());
        did_acquire(false);
        return;
    }

    auto request = request_server.ui_client().import_request(exported.release_value(), Requests::RequestClient::TransferLease::Yes);
    if (!request) {
        did_acquire(false);
        return;
    }
    request->set_body_delivery_paused(true);
    set_response_body_callbacks(*request);
    m_response_body_request = request;
    m_response_body_request_server = request_server;

    // The handle names the lease the host adopts, which is now the UI process's request in the host's RequestServer.
    body_handle->request_server_client_id = request_server.ui_client().request_server_client_id();
    body_handle->request_server_request_id = request->id();
    m_response_body_request_server_client_id = body_handle->request_server_client_id;
    m_response_body_request_server_request_id = body_handle->request_server_request_id;
}

Web::HTML::NavigationPopulationResult NavigationLoader::take_result()
{
    VERIFY(m_result.has_value());
    if (m_response_body_request)
        m_response_body_was_handed_off = true;
    return m_result.release_value();
}

bool NavigationLoader::response_body_matches(int request_server_client_id, u64 request_server_request_id) const
{
    return m_response_body_request_server_client_id == request_server_client_id
        && m_response_body_request_server_request_id == request_server_request_id;
}

void NavigationLoader::reclaim_response_body_after_failed_handoff()
{
    if (!m_response_body_was_handed_off)
        return;
    m_response_body_was_handed_off = false;
    release_response_body();
}

Web::HTML::NavigationPopulationResult const& NavigationLoader::result() const
{
    VERIFY(m_result.has_value());
    return *m_result;
}

// The document the result creates takes the UI process's id for it. Its window takes over the navigation params'
// reserved environment's id, which the UI process generates, and is in the agent cluster of the agent the UI process
// obtained for it. A navigation without a reserved environment gets one with the id.
void NavigationLoader::set_document(CanonicalDocument const& document, CanonicalNavigable const& navigable)
{
    VERIFY(m_result.has_value());
    m_result->document_id = document.id();

    auto* navigation_params = m_result->navigation_params.get_pointer<Web::HTML::NavigationParamsDescriptor>();
    if (!navigation_params)
        return;
    auto const& window = document.relevant_global_object();
    // NB: An extension's file the browser read gets the environment the UI process knows of, not the renderer's.
    if (m_response_is_an_extension_file) {
        auto const& response_url = navigation_params->response.url_list.is_empty() ? m_request.history_entry.url : navigation_params->response.url_list.last();
        auto const* top_level_document = navigable.is_top_level_traversable() ? nullptr : &navigable.top_level_traversable().active_document();
        navigation_params->reserved_environment = Web::HTML::NavigationEnvironmentDescriptor {
            .id = {},
            .creation_url = response_url,
            .top_level_creation_url = top_level_document ? top_level_document->creation_url() : response_url,
            .top_level_origin = top_level_document ? top_level_document->origin() : navigation_params->origin,
        };
    }
    if (!navigation_params->reserved_environment.has_value()) {
        navigation_params->reserved_environment = Web::HTML::NavigationEnvironmentDescriptor {
            .id = {},
            .creation_url = m_request.history_entry.url,
            .top_level_creation_url = {},
            .top_level_origin = {},
        };
    }
    navigation_params->reserved_environment->id = window.relevant_settings_object().id();
    navigation_params->agent_cluster_id = window.agent().agent_cluster_id();
    navigation_params->agent_cluster_cross_origin_isolation_mode = window.agent().agent_cluster_cross_origin_isolation_mode();

    if (&document.browsing_context() != &navigable.active_browsing_context())
        navigation_params->new_browsing_context_group_id = document.browsing_context().group()->id();
}

void NavigationLoader::discard(IsPrivate, Web::HTML::NavigationPopulationResult& result)
{
    auto* body_handle = response_body_handle(result);
    if (!body_handle)
        return;

    auto request_server = RequestServerManager::the().instance_for_client(body_handle->request_server_client_id);
    if (!request_server)
        return;
    auto request = request_server->ui_client().adopt_request(body_handle->request_server_client_id, body_handle->request_server_request_id);
    if (request)
        request->stop();
}

void NavigationLoader::did_acquire(bool succeeded)
{
    if (!m_completion_steps)
        return;

    auto completion_steps = move(m_completion_steps);
    completion_steps(succeeded);
}

void NavigationLoader::release_response_body()
{
    if (!m_response_body_request)
        return;

    // Once the descriptor has been sent, the destination is responsible for adopting the request. Dropping our
    // local reference before that IPC is handled must not release the lease or cancel the request being transferred.
    if (m_response_body_was_handed_off) {
        m_response_body_request = nullptr;
        return;
    }

    m_response_body_request->release_transfer_lease();
    m_response_body_request->stop();
    m_response_body_request = nullptr;
}

}
