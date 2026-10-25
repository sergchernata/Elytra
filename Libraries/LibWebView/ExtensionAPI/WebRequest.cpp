/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibURL/Parser.h>
#include <LibWebExtensions/DeclarativeNetRequest.h>
#include <LibWebExtensions/MatchPattern.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionAPI/Management.h>
#include <LibWebView/ExtensionAPI/TabsDelegate.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

namespace DNR = WebExtensions::DNR;
using Context = ExtensionRuntime::Context;
using Reply = ExtensionRuntime::Reply;

// chrome.webRequest and chrome.declarativeNetRequest.
//
// The page processes do the work (LibWeb/Extensions/WebRequests): this tells each page which webRequest events
// extensions listen to and what their rules are, and the pages apply the rules and report the events, which go from
// here to the listeners that may see them. onAuthRequired's blocking listeners are asked here, and their answer goes
// back to the page.
//
// An extension's enabled static rulesets and dynamic rules are kept in <storage directory>/<id>/declarative_net_request.json.
class WebRequestAPI final : public ExtensionAPI {
public:
    explicit WebRequestAPI(ExtensionRuntime&);

    virtual void context_destroyed(u64 context_id) override;
    virtual void listeners_changed(u64 context_id, StringView event) override;
    virtual void extensions_changed() override;
    virtual void extension_uninstalled(StringView extension_id) override;
    virtual void page_created(WebContentPage&) override;
    virtual void page_gone(WebContentPage&) override;

    String config_for(WebContentPage&);

private:
    struct State {
        AK_ALLOC_WITH_KMALLOC;

        String version;
        Vector<String> enabled_rulesets;
        Vector<DNR::Rule> dynamic_rules;
        Vector<DNR::Rule> session_rules;
        // The manifest's rulesets, in its order, as read.
        Vector<String> ruleset_ids;
        HashMap<String, Vector<DNR::Rule>> static_rulesets;
    };
    State& state(Extension const&);
    void save(Extension const&);
    size_t static_rule_count(State const&, Vector<String> const& rulesets) const;

    ErrorOr<Extension const*, String> extension_for_rules(Context const&) const;
    void register_rule_functions();

    // What the pages know. An extension's rules for every tab are kept serialized until they change, and a page is
    // told again only when what it would be told changes.
    struct SerializedRules {
        bool has_plain { false };
        Vector<String> host_permissions;
        // As JSON, comma-separated: the rules that can act (those needing host access only with some).
        String rules;
        bool needs_host_access { false };
        bool needs_id { false };
    };
    SerializedRules const& serialized_rules(Extension const&);
    void rules_changed(Extension const&);
    HashMap<String, SerializedRules> m_serialized_rules;
    struct Told {
        u32 hash { 0 };
        size_t length { 0 };
        i64 tab_id { -1 };
    };
    HashMap<WebContentPage const*, Told> m_told;
    void tell_page(WebContentPage&);
    void tell_pages();
    HashTable<String> listened_events() const;
    void events_may_have_changed();
    HashTable<String> m_told_events;

    // webRequest. A listener's filter, read once.
    struct Filter {
        bool is_valid { false };
        Vector<WebExtensions::MatchPattern> urls;
        Optional<Vector<String>> types;
        bool has_tab_id { false };
        Optional<i64> tab_id;
        bool has_window_id { false };
        Optional<i64> window_id;
        bool request_headers { false };
        bool response_headers { false };
        bool blocking { false };
        bool async_blocking { false };
    };
    static Filter filter_from(JsonArray const& extra);
    Filter const& filter_of(Context const&, String const& event, i64 listener, JsonArray const& extra);
    HashMap<u64, HashMap<String, HashMap<i64, Filter>>> m_filters;

    // The requests pages reported, by the renderer's ID: the browser's ID for each (renderers' IDs are only their
    // process's), and the redirects of a navigation's.
    struct Request {
        String id;
        Vector<URL::URL> redirects;
    };
    Request& request_of(WebContentPage const&, String const& renderer_id);
    HashMap<WebContentPage const*, HashMap<String, Request>> m_requests;
    u64 m_next_request_id { 1 };

    enum class Purpose : u8 {
        Event,
        Authentication,
    };
    bool may_see(Extension const&, URL::URL const&, Optional<URL::URL> const& initiator) const;
    Optional<JsonObject> details_from(WebContentPage&, JsonObject const& message, Purpose, URL::URL& url, Optional<URL::URL>& initiator, ExtensionRuntime::TabsHooks::Frame& frame, String& renderer_id);
    static bool listener_wants(Filter const&, JsonObject const& details, ExtensionRuntime::TabsHooks::Frame const&, URL::URL const&);
    static JsonObject details_for(Filter const&, JsonObject details);
    void event_reported(WebContentPage&, JsonObject const&);

    // onAuthRequired's blocking listeners, asked for a page.
    void authentication_requested(WebContentPage&, JsonObject const&);
    void blocking_response(Context const&, JsonObject const&);
    void answer_authentication(u64 id);
    struct Answer {
        size_t order { 0 };
        JsonValue response;
    };
    struct PendingAuthentication {
        RefPtr<WebContentPage> page;
        u64 renderer_request { 0 };
        size_t remaining { 0 };
        Vector<Answer> answers;
        RefPtr<Core::Timer> timeout;
    };
    struct Asked {
        u64 authentication { 0 };
        u64 context_id { 0 };
        size_t order { 0 };
    };
    HashMap<u64, PendingAuthentication> m_pending_authentications;
    HashMap<u64, Asked> m_asked;
    u64 m_next_authentication { 1 };
    u64 m_next_blocking { 1 };

    // Extensions, most recently installed first.
    Vector<Extension const*> extensions_in_order() const;

    HashMap<String, NonnullOwnPtr<State>> m_states;
};

static WebRequestAPI* s_web_request_api;

NonnullOwnPtr<ExtensionAPI> create_web_request_api(ExtensionRuntime& runtime)
{
    auto api = make<WebRequestAPI>(runtime);
    s_web_request_api = api.ptr();
    return api;
}

String web_request_config_for_testing(WebContentPage& page)
{
    return s_web_request_api ? s_web_request_api->config_for(page) : String {};
}

static constexpr auto authentication_timeout = AK::Duration::from_seconds(60);
static constexpr size_t maximum_pending_authentications_per_page = 32;
// A listener's filter with more is refused, as are more requests at once than a page could have.
static constexpr size_t maximum_filter_urls = 1000;
static constexpr size_t maximum_filter_types = 32;
static constexpr size_t maximum_requests_per_page = 4096;
static constexpr size_t maximum_redirects = 20;

static constexpr Array web_request_events {
    "onBeforeRequest"sv, "onBeforeSendHeaders"sv, "onSendHeaders"sv, "onHeadersReceived"sv, "onAuthRequired"sv,
    "onResponseStarted"sv, "onBeforeRedirect"sv, "onCompleted"sv, "onErrorOccurred"sv
};

static JsonArray json_strings(Vector<String> const& strings)
{
    JsonArray array;
    for (auto const& string : strings)
        array.must_append(string);
    return array;
}

static i64 tab_id_of(ExtensionRuntime const& runtime, WebContentPage& page)
{
    if (!runtime.tabs_delegate() || !page.is_open() || runtime.is_background_page(page))
        return -1;
    for (auto const& tab : runtime.tabs_delegate()->tabs()) {
        if (tab.view == &page.view())
            return tab.id;
    }
    return -1;
}

WebRequestAPI::WebRequestAPI(ExtensionRuntime& extension_runtime)
    : ExtensionAPI(extension_runtime)
{
    runtime().register_page_message("web_request_event"sv, [this](WebContentPage& page, JsonObject const& message) { event_reported(page, message); });
    runtime().register_page_message("web_request_auth"sv, [this](WebContentPage& page, JsonObject const& message) { authentication_requested(page, message); });
    runtime().register_context_message("blocking_response"sv, [this](Context const& context, JsonObject const& message) { blocking_response(context, message); });

    // Session rules for some tabs only go to those tabs' pages, which a tab may only just have become.
    auto tabs_changed = move(runtime().tabs_hooks.tabs_changed);
    runtime().tabs_hooks.tabs_changed = [this, tabs_changed = move(tabs_changed)] {
        if (tabs_changed)
            tabs_changed();
        bool has_tab_rules = false;
        for (auto const& [id, state] : m_states) {
            if (any_of(state->session_rules, [](auto const& rule) { return rule.condition().tab_ids.has_value() || !rule.condition().excluded_tab_ids.is_empty(); }))
                has_tab_rules = true;
        }
        if (!has_tab_rules)
            return;
        WebContentClient::for_each_client([&](WebContentClient& client) {
            client.for_each_page([&](WebContentPage& page) {
                if (auto told = m_told.get(&page); !told.has_value() || told->tab_id != tab_id_of(runtime(), page))
                    tell_page(page);
                return IterationDecision::Continue;
            });
            return IterationDecision::Continue;
        });
    };

    runtime().register_function("webRequest.handlerBehaviorChanged"sv, [](Context const&, JsonArray const&, Reply reply) {
        reply(JsonValue {});
    });

    register_rule_functions();
}

Vector<Extension const*> WebRequestAPI::extensions_in_order() const
{
    Vector<Extension const*> extensions;
    runtime().registry().for_each_extension([&](Extension const& extension) { extensions.append(&extension); });
    auto installed = [&](Extension const* extension) {
        auto record = runtime().registry().installed_extension(extension->id());
        return record.has_value() ? record->install_time : UnixDateTime::epoch();
    };
    quick_sort(extensions, [&](auto const* a, auto const* b) {
        auto a_time = installed(a);
        auto b_time = installed(b);
        if (a_time != b_time)
            return a_time > b_time;
        return a->id() < b->id();
    });
    return extensions;
}

// Rules.
WebRequestAPI::State& WebRequestAPI::state(Extension const& extension)
{
    if (auto existing = m_states.find(extension.id()); existing != m_states.end()) {
        if (existing->value->version == extension.manifest().version)
            return *existing->value;
        // An update: the manifest's rulesets again, keeping dynamic and session rules.
        auto old = m_states.take(extension.id()).release_value();
        auto& updated = state(extension);
        updated.session_rules = move(old->session_rules);
        return updated;
    }

    auto state = make<State>();
    state->version = extension.manifest().version;
    if (extension.manifest().declarative_net_request.has_value()) {
        for (auto const& resource : extension.manifest().declarative_net_request->rule_resources) {
            if (state->ruleset_ids.size() >= DNR::max_number_of_static_rulesets || state->ruleset_ids.contains_slow(resource.id))
                continue;
            state->ruleset_ids.append(resource.id);
            Vector<DNR::Rule> rules;
            if (auto contents = extension.read_file(resource.path); contents.has_value()) {
                Vector<String> warnings;
                rules = DNR::parse_ruleset(*contents, warnings);
                for (auto const& warning : warnings)
                    dbgln("Extensions: {}'s ruleset {}: {}", extension.id(), resource.id, warning);
            } else {
                dbgln("Extensions: {} has no ruleset file {}", extension.id(), resource.path);
            }
            state->static_rulesets.set(resource.id, move(rules));
            if (resource.enabled && state->enabled_rulesets.size() < DNR::max_number_of_enabled_static_rulesets
                && static_rule_count(*state, state->enabled_rulesets) + state->static_rulesets.get(resource.id)->size() <= DNR::guaranteed_minimum_static_rules)
                state->enabled_rulesets.append(resource.id);
        }
    }

    if (runtime().storage_directory().has_value()) {
        auto path = LexicalPath::join(*runtime().storage_directory(), extension.id(), "declarative_net_request.json"sv).string();
        if (auto file = Core::File::open(path, Core::File::OpenMode::Read); !file.is_error()) {
            auto contents = file.value()->read_until_eof();
            auto json = contents.is_error() ? ErrorOr<JsonValue> { Error::from_string_literal("unreadable") } : JsonParser::parse(StringView { contents.value() });
            if (!json.is_error() && json.value().is_object()) {
                auto const& saved = json.value().as_object();
                if (auto rules = saved.get("dynamicRules"sv); rules.has_value()) {
                    JsonObject update;
                    update.set("addRules"sv, *rules);
                    if (auto parsed = DNR::update_rules({}, update, DNR::RuleSource::Dynamic); !parsed.is_error())
                        state->dynamic_rules = parsed.release_value();
                    else
                        dbgln("Extensions: {}'s saved dynamic rules: {}", extension.id(), parsed.error());
                }
                // Chrome keeps the enabled rulesets until the extension is updated.
                auto enabled = saved.get_array("enabledRulesets"sv);
                if (enabled.has_value() && saved.get_string("version"sv) == extension.manifest().version) {
                    Vector<String> ids;
                    enabled->for_each([&](JsonValue const& id) {
                        if (id.is_string() && state->ruleset_ids.contains_slow(id.as_string()) && !ids.contains_slow(id.as_string()))
                            ids.append(id.as_string());
                    });
                    if (ids.size() <= DNR::max_number_of_enabled_static_rulesets && static_rule_count(*state, ids) <= DNR::guaranteed_minimum_static_rules)
                        state->enabled_rulesets = move(ids);
                }
            }
        }
    }

    auto& result = *state;
    m_states.set(extension.id(), move(state));
    return result;
}

size_t WebRequestAPI::static_rule_count(State const& state, Vector<String> const& rulesets) const
{
    size_t count = 0;
    for (auto const& id : rulesets) {
        if (auto rules = state.static_rulesets.get(id); rules.has_value())
            count += rules->size();
    }
    return count;
}

void WebRequestAPI::save(Extension const& extension)
{
    if (!runtime().storage_directory().has_value())
        return;
    auto& state = this->state(extension);
    auto directory = LexicalPath::join(*runtime().storage_directory(), extension.id()).string();
    if (auto result = Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes); result.is_error()) {
        dbgln("Extensions: Can't make {}: {}", directory, result.error());
        return;
    }
    JsonArray rules;
    for (auto const& rule : state.dynamic_rules)
        rules.must_append(rule.json());
    JsonObject saved;
    saved.set("version"sv, state.version);
    saved.set("enabledRulesets"sv, json_strings(state.enabled_rulesets));
    saved.set("dynamicRules"sv, move(rules));

    auto path = LexicalPath::join(directory, "declarative_net_request.json"sv).string();
    auto serialized = saved.serialized();
    auto file = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error() || file.value()->write_until_depleted(serialized.bytes()).is_error())
        dbgln("Extensions: Can't save {}'s rules", extension.id());
}

ErrorOr<Extension const*, String> WebRequestAPI::extension_for_rules(Context const& context) const
{
    if (context.type == "content_script"sv)
        return "declarativeNetRequest isn't available to content scripts."_string;
    auto const* extension = runtime().registry().find(context.extension_id);
    if (!extension)
        return "The extension isn't installed."_string;
    auto& management = runtime().management();
    if (!management.has_permission(extension->id(), "declarativeNetRequest"sv) && !management.has_permission(extension->id(), "declarativeNetRequestWithHostAccess"sv))
        return "You do not have permission to use declarativeNetRequest."_string;
    return extension;
}

static ErrorOr<Vector<String>, String> ruleset_ids_in(JsonObject const& options, StringView key)
{
    Vector<String> ids;
    auto value = options.get(key);
    if (!value.has_value())
        return ids;
    if (!value->is_array())
        return MUST(String::formatted("Invalid value for '{}'.", key));
    for (auto const& id : value->as_array().values()) {
        if (!id.is_string())
            return MUST(String::formatted("Invalid value for '{}'.", key));
        ids.append(id.as_string());
    }
    return ids;
}

void WebRequestAPI::register_rule_functions()
{
#define EXTENSION_OR_REPLY(context)                     \
    ({                                                  \
        auto _extension = extension_for_rules(context); \
        if (_extension.is_error())                      \
            return reply(_extension.release_error());   \
        _extension.release_value();                     \
    })

    for (auto source : { DNR::RuleSource::Dynamic, DNR::RuleSource::Session }) {
        auto kind = source == DNR::RuleSource::Dynamic ? "Dynamic"sv : "Session"sv;

        runtime().register_function(MUST(String::formatted("declarativeNetRequest.update{}Rules", kind)), [this, source, kind](Context const& context, JsonArray const& arguments, Reply reply) {
            auto const* extension = EXTENSION_OR_REPLY(context);
            if (arguments.is_empty() || !arguments.at(0).is_object())
                return reply(MUST(String::formatted("Error in invocation of declarativeNetRequest.update{}Rules: the options must be an object.", kind)));
            auto& state = this->state(*extension);
            auto& rules = source == DNR::RuleSource::Dynamic ? state.dynamic_rules : state.session_rules;
            auto updated = DNR::update_rules(rules, arguments.at(0), source);
            if (updated.is_error())
                return reply(updated.release_error());
            rules = updated.release_value();
            if (source == DNR::RuleSource::Dynamic)
                save(*extension);
            rules_changed(*extension);
            reply(JsonValue {});
        });

        runtime().register_function(MUST(String::formatted("declarativeNetRequest.get{}Rules", kind)), [this, source](Context const& context, JsonArray const& arguments, Reply reply) {
            auto const* extension = EXTENSION_OR_REPLY(context);
            Optional<Vector<i64>> wanted;
            if (!arguments.is_empty() && arguments.at(0).is_object()) {
                if (auto ids = arguments.at(0).as_object().get_array("ruleIds"sv); ids.has_value()) {
                    wanted = Vector<i64> {};
                    ids->for_each([&](JsonValue const& id) {
                        if (auto value = id.get_integer<i64>(); value.has_value())
                            wanted->append(*value);
                    });
                }
            } else if (!arguments.is_empty() && !arguments.at(0).is_null()) {
                return reply("Invalid value for 'filter'."_string);
            }
            auto& state = this->state(*extension);
            JsonArray result;
            for (auto const& rule : source == DNR::RuleSource::Dynamic ? state.dynamic_rules : state.session_rules) {
                if (!wanted.has_value() || wanted->contains_slow(rule.id()))
                    result.must_append(rule.json());
            }
            reply(JsonValue { move(result) });
        });
    }

    runtime().register_function("declarativeNetRequest.getEnabledRulesets"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        auto const* extension = EXTENSION_OR_REPLY(context);
        reply(JsonValue { json_strings(state(*extension).enabled_rulesets) });
    });

    runtime().register_function("declarativeNetRequest.updateEnabledRulesets"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        auto const* extension = EXTENSION_OR_REPLY(context);
        if (arguments.is_empty() || !arguments.at(0).is_object())
            return reply("Error in invocation of declarativeNetRequest.updateEnabledRulesets: the options must be an object."_string);
        auto const& options = arguments.at(0).as_object();
        auto disable = ruleset_ids_in(options, "disableRulesetIds"sv);
        if (disable.is_error())
            return reply(disable.release_error());
        auto enable = ruleset_ids_in(options, "enableRulesetIds"sv);
        if (enable.is_error())
            return reply(enable.release_error());
        auto& state = this->state(*extension);
        for (auto const& id : disable.value()) {
            if (!state.ruleset_ids.contains_slow(id))
                return reply(MUST(String::formatted("Invalid ruleset id: {}.", id)));
        }
        for (auto const& id : enable.value()) {
            if (!state.ruleset_ids.contains_slow(id))
                return reply(MUST(String::formatted("Invalid ruleset id: {}.", id)));
        }
        // Disabling comes first; the order stays the manifest's.
        Vector<String> enabled;
        for (auto const& id : state.ruleset_ids) {
            bool was_enabled = state.enabled_rulesets.contains_slow(id) && !disable.value().contains_slow(id);
            if (was_enabled || enable.value().contains_slow(id))
                enabled.append(id);
        }
        if (enabled.size() > DNR::max_number_of_enabled_static_rulesets)
            return reply("The number of enabled static rulesets exceeds the limit."_string);
        if (static_rule_count(state, enabled) > DNR::guaranteed_minimum_static_rules)
            return reply("The number of enabled static rules exceeds the rule count limit."_string);
        state.enabled_rulesets = move(enabled);
        save(*extension);
        rules_changed(*extension);
        reply(JsonValue {});
    });

    runtime().register_function("declarativeNetRequest.getAvailableStaticRuleCount"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        auto const* extension = EXTENSION_OR_REPLY(context);
        auto& state = this->state(*extension);
        auto used = static_rule_count(state, state.enabled_rulesets);
        reply(JsonValue { static_cast<i64>(DNR::guaranteed_minimum_static_rules) - static_cast<i64>(used) });
    });

    runtime().register_function("declarativeNetRequest.isRegexSupported"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        (void)EXTENSION_OR_REPLY(context);
        auto regex = arguments.is_empty() || !arguments.at(0).is_object() ? Optional<String> {} : arguments.at(0).as_object().get_string("regex"sv);
        if (!regex.has_value())
            return reply("Error in invocation of declarativeNetRequest.isRegexSupported: 'regex' is required."_string);
        auto case_sensitive = arguments.at(0).as_object().get_bool("isCaseSensitive"sv).value_or(false);
        JsonObject result;
        auto reason = DNR::regex_unsupported_reason(*regex, case_sensitive);
        result.set("isSupported"sv, !reason.has_value());
        if (reason.has_value())
            result.set("reason"sv, *reason);
        reply(JsonValue { move(result) });
    });

    runtime().register_function("declarativeNetRequest.getMatchedRules"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        auto const* extension = EXTENSION_OR_REPLY(context);
        if (!runtime().management().has_permission(extension->id(), "declarativeNetRequestFeedback"sv))
            return reply("The extension doesn't have the declarativeNetRequestFeedback permission."_string);
        // FIXME: Have the pages report the rules that matched.
        JsonObject result;
        result.set("rulesMatchedInfo"sv, JsonArray {});
        reply(JsonValue { move(result) });
    });

    runtime().register_function("declarativeNetRequest.setExtensionActionOptions"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        (void)EXTENSION_OR_REPLY(context);
        reply(JsonValue {});
    });

    runtime().register_function("declarativeNetRequest.testMatchOutcome"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        (void)EXTENSION_OR_REPLY(context);
        reply("testMatchOutcome isn't supported."_string);
    });

#undef EXTENSION_OR_REPLY
}

// What the pages know.
HashTable<String> WebRequestAPI::listened_events() const
{
    HashTable<String> events;
    runtime().for_each_context([&](Context const& context) {
        if (context.type == "content_script"sv || !runtime().management().has_permission(context.extension_id, "webRequest"sv))
            return;
        for (auto const& [event, listeners] : context.listeners) {
            if (!listeners.is_empty() && event.starts_with_bytes("webRequest."sv))
                events.set(event);
        }
    });
    return events;
}

// Whether a redirect rule may go to the extension's own pages, which the page then needs the extension's ID for.
static bool may_redirect_to_extension_pages(DNR::Rule const& rule)
{
    if (rule.action_type() != DNR::ActionType::Redirect || !rule.redirect().has_value())
        return false;
    auto const& redirect = *rule.redirect();
    return redirect.extension_path.has_value()
        || (redirect.url.has_value() && redirect.url->starts_with_bytes("chrome-extension:"sv))
        || (redirect.transform.has_value() && redirect.transform->scheme == "chrome-extension"sv)
        || (redirect.regex_substitution.has_value() && redirect.regex_substitution->contains("chrome-extension"sv));
}

static bool needs_host_access(DNR::Rule const& rule, bool has_plain)
{
    return !has_plain || rule.action_type() == DNR::ActionType::Redirect || rule.action_type() == DNR::ActionType::ModifyHeaders;
}

WebRequestAPI::SerializedRules const& WebRequestAPI::serialized_rules(Extension const& extension)
{
    auto const& id = extension.id();
    auto& management = runtime().management();
    bool has_plain = management.has_permission(id, "declarativeNetRequest"sv);
    auto host_permissions = management.host_permission_patterns(id);
    if (auto cached = m_serialized_rules.find(id); cached != m_serialized_rules.end() && cached->value.has_plain == has_plain && cached->value.host_permissions == host_permissions)
        return cached->value;

    auto& state = this->state(extension);
    SerializedRules serialized;
    serialized.has_plain = has_plain;
    serialized.host_permissions = move(host_permissions);
    StringBuilder builder;
    auto add = [&](DNR::Rule const& rule) {
        bool needs_hosts = needs_host_access(rule, has_plain);
        // A rule needing host access does nothing for an extension without any.
        if (needs_hosts && serialized.host_permissions.is_empty())
            return;
        serialized.needs_host_access |= needs_hosts;
        serialized.needs_id |= may_redirect_to_extension_pages(rule);
        if (!builder.is_empty())
            builder.append(',');
        builder.append(rule.json().serialized());
    };
    for (auto const& ruleset : state.enabled_rulesets) {
        for (auto const& rule : *state.static_rulesets.get(ruleset))
            add(rule);
    }
    for (auto const& rule : state.dynamic_rules)
        add(rule);
    for (auto const& rule : state.session_rules) {
        if (!rule.condition().tab_ids.has_value() && rule.condition().excluded_tab_ids.is_empty())
            add(rule);
    }
    serialized.rules = builder.to_string_without_validation();
    m_serialized_rules.set(id, move(serialized));
    return *m_serialized_rules.get(id);
}

void WebRequestAPI::rules_changed(Extension const& extension)
{
    m_serialized_rules.remove(extension.id());
    tell_pages();
}

// What the page needs of the rules: those that can act, the host permissions only of extensions whose rules need them,
// and in place of an extension's ID a name of the page's own, unless the page is the extension's or its rules redirect
// to its pages. A private page has none.
String WebRequestAPI::config_for(WebContentPage& page)
{
    if (!page.is_open() || ExtensionRuntime::is_private_page(page))
        return R"({"type":"web_request_config","events":[],"extensions":[]})"_string;

    auto tab_id = tab_id_of(runtime(), page);
    JsonArray events;
    for (auto const& event : listened_events())
        events.must_append(event);

    StringBuilder builder;
    builder.appendff(R"({{"type":"web_request_config","events":{},"extensions":[)", events.serialized());
    auto& management = runtime().management();
    size_t index = 0;
    for (auto const* extension : extensions_in_order()) {
        auto const& id = extension->id();
        bool has_plain = management.has_permission(id, "declarativeNetRequest"sv);
        if (!has_plain && !management.has_permission(id, "declarativeNetRequestWithHostAccess"sv))
            continue;
        auto const& serialized = serialized_rules(*extension);
        bool extension_needs_hosts = serialized.needs_host_access;
        bool needs_id = serialized.needs_id;

        // Session rules for some tabs go to those tabs' pages only, without the tabs.
        StringBuilder tab_rules;
        for (auto const& rule : state(*extension).session_rules) {
            auto const& condition = rule.condition();
            if (!condition.tab_ids.has_value() && condition.excluded_tab_ids.is_empty())
                continue;
            if (condition.tab_ids.has_value() && !condition.tab_ids->contains_slow(tab_id))
                continue;
            if (condition.excluded_tab_ids.contains_slow(tab_id))
                continue;
            bool needs_hosts = needs_host_access(rule, has_plain);
            if (needs_hosts && serialized.host_permissions.is_empty())
                continue;
            extension_needs_hosts |= needs_hosts;
            needs_id |= may_redirect_to_extension_pages(rule);
            auto json = rule.json();
            if (auto rule_condition = json.get_object("condition"sv); rule_condition.has_value()) {
                auto without_tabs = *rule_condition;
                without_tabs.remove("tabIds"sv);
                without_tabs.remove("excludedTabIds"sv);
                json.set("condition"sv, move(without_tabs));
            }
            tab_rules.append(',');
            tab_rules.append(json.serialized());
        }
        if (serialized.rules.is_empty() && tab_rules.is_empty())
            continue;

        if (index != 0)
            builder.append(',');
        auto name = page.client().may_speak_for_extension(id) || needs_id ? id : MUST(String::formatted("extension-{}", index));
        ++index;
        builder.appendff(R"({{"id":"{}","allActionsNeedHostAccess":{})", name, !has_plain);
        if (extension_needs_hosts)
            builder.appendff(R"(,"hostPermissions":{})", json_strings(serialized.host_permissions).serialized());
        builder.append(R"(,"rules":[)"sv);
        builder.append(serialized.rules);
        auto tab_rules_view = tab_rules.string_view();
        if (serialized.rules.is_empty() && !tab_rules_view.is_empty())
            tab_rules_view = tab_rules_view.substring_view(1);
        builder.append(tab_rules_view);
        builder.append("]}"sv);
    }
    builder.append("]}"sv);
    return builder.to_string_without_validation();
}

void WebRequestAPI::tell_page(WebContentPage& page)
{
    if (!page.is_open())
        return;
    auto config = config_for(page);
    Told told { config.hash(), config.bytes().size(), tab_id_of(runtime(), page) };
    if (auto last = m_told.get(&page); last.has_value() && last->hash == told.hash && last->length == told.length) {
        m_told.set(&page, told);
        return;
    }
    m_told.set(&page, told);
    page.async_deliver_extension_message(0, config);
}

void WebRequestAPI::tell_pages()
{
    m_told_events = listened_events();
    WebContentClient::for_each_client([&](WebContentClient& client) {
        client.for_each_page([&](WebContentPage& page) {
            tell_page(page);
            return IterationDecision::Continue;
        });
        return IterationDecision::Continue;
    });
}

void WebRequestAPI::events_may_have_changed()
{
    auto events = listened_events();
    if (events.size() == m_told_events.size() && all_of(events, [&](auto const& event) { return m_told_events.contains(event); }))
        return;
    tell_pages();
}

void WebRequestAPI::page_created(WebContentPage& page)
{
    // A new page, or a new process for it, knows nothing yet.
    m_told.remove(&page);
    tell_page(page);
}

void WebRequestAPI::page_gone(WebContentPage& page)
{
    m_told.remove(&page);
    m_requests.remove(&page);
}

void WebRequestAPI::listeners_changed(u64 context_id, StringView event)
{
    if (!event.starts_with("webRequest."sv))
        return;
    if (auto filters = m_filters.find(context_id); filters != m_filters.end())
        filters->value.remove(event);
    events_may_have_changed();
}

void WebRequestAPI::context_destroyed(u64 context_id)
{
    m_filters.remove(context_id);
    Vector<u64> unanswered;
    for (auto const& [blocking, asked] : m_asked) {
        if (asked.context_id == context_id)
            unanswered.append(blocking);
    }
    for (auto blocking : unanswered) {
        auto asked = m_asked.take(blocking).release_value();
        if (auto pending = m_pending_authentications.find(asked.authentication); pending != m_pending_authentications.end() && --pending->value.remaining == 0)
            answer_authentication(asked.authentication);
    }
    events_may_have_changed();
}

void WebRequestAPI::extensions_changed()
{
    // Forget what's kept of extensions that are gone; an updated one reads its rulesets again (state()).
    Vector<String> gone;
    for (auto const& [id, state] : m_states) {
        if (!runtime().registry().find(id))
            gone.append(id);
    }
    for (auto const& id : gone)
        m_states.remove(id);
    m_serialized_rules.clear();
    tell_pages();
}

void WebRequestAPI::extension_uninstalled(StringView extension_id)
{
    m_states.remove(extension_id);
    m_serialized_rules.remove(extension_id);
}

// webRequest.
bool WebRequestAPI::may_see(Extension const& extension, URL::URL const& url, Optional<URL::URL> const& initiator) const
{
    auto& management = runtime().management();
    if (!management.has_permission(extension.id(), "webRequest"sv))
        return false;
    if (WebExtensions::is_restricted_url(url, false, extension.id().bytes_as_string_view()))
        return false;
    if (!management.has_host_permission(extension.id(), url))
        return false;
    if (initiator.has_value()) {
        // Never what another extension asks for; the initiator's host access too, as Chrome asks.
        if (initiator->scheme() == "chrome-extension"sv)
            return initiator->serialized_host() == extension.id().bytes_as_string_view();
        if (initiator->scheme().is_one_of("http"sv, "https"sv) && !management.has_host_permission(extension.id(), *initiator))
            return false;
    }
    return true;
}

WebRequestAPI::Request& WebRequestAPI::request_of(WebContentPage const& page, String const& renderer_id)
{
    auto& requests = m_requests.ensure(&page);
    if (auto request = requests.find(renderer_id); request != requests.end())
        return request->value;
    if (requests.size() >= maximum_requests_per_page)
        requests.clear();
    return requests.ensure(renderer_id, [&] { return Request { String::number(m_next_request_id++), {} }; });
}

// The details a page reported, checked: only what Chrome's details have, with the tab and frame the browser knows, for
// a request a document of the page could have made. The browser can't see a page's subresource requests (RequestServer
// makes them for the renderer), so their URLs are the renderer's word; but their initiator must be a document the page
// hosts, and a navigation's URL the one the browser started it at (or a redirect of it the page reported). Credentials
// are asked for only for a site of the page's documents or navigations, or for an extension's own requests, in its own
// process, to a host it has access to.
Optional<JsonObject> WebRequestAPI::details_from(WebContentPage& page, JsonObject const& message, Purpose purpose, URL::URL& url, Optional<URL::URL>& initiator, ExtensionRuntime::TabsHooks::Frame& frame, String& renderer_id)
{
    if (ExtensionRuntime::is_private_page(page))
        return {};
    auto reported = message.get_object("details"sv);
    if (!reported.has_value())
        return {};
    auto url_string = reported->get_string("url"sv);
    auto request_id = reported->get_string("requestId"sv);
    auto method = reported->get_string("method"sv);
    auto type = reported->get_string("type"sv);
    if (!url_string.has_value() || !request_id.has_value() || request_id->bytes().size() > 32 || !method.has_value() || !type.has_value() || !DNR::resource_type_from_string(*type).has_value())
        return {};
    auto parsed = URL::Parser::basic_parse(*url_string);
    if (!parsed.has_value() || !parsed->scheme().is_one_of("http"sv, "https"sv))
        return {};
    url = parsed.release_value();
    bool is_navigation = *type == "main_frame"sv || *type == "sub_frame"sv;

    Optional<Web::HTML::CrossProcessId> navigable_id;
    if (auto navigable = message.get_object("navigable"sv); navigable.has_value()) {
        auto namespace_id = navigable->get_u64("ns"sv);
        auto local_id = navigable->get_u64("id"sv);
        if (namespace_id.has_value() && local_id.has_value())
            navigable_id = Web::HTML::CrossProcessId { .namespace_id = *namespace_id, .local_id = *local_id };
    }

    // The initiator: a document the page hosts (an opaque one is "null"). Only a navigation the browser started has none.
    initiator = {};
    auto initiator_string = reported->get_string("initiator"sv);
    bool for_own_extension = false;
    if (initiator_string.has_value() && *initiator_string != "null"sv) {
        auto initiator_url = URL::Parser::basic_parse(*initiator_string);
        if (!initiator_url.has_value())
            return {};
        if (initiator_url->scheme() == "chrome-extension"sv) {
            // A page may only speak for an extension whose own process it is.
            if (!page.client().may_speak_for_extension(initiator_url->serialized_host()))
                return {};
            for_own_extension = runtime().management().has_host_permission(initiator_url->serialized_host(), url);
        } else if (!page.hosts_a_document_with_origin(initiator_url->origin())) {
            return {};
        }
        initiator = initiator_url;
    } else if (!initiator_string.has_value() && !is_navigation) {
        return {};
    }

    auto* known_request = [&]() -> Request* {
        auto requests = m_requests.find(&page);
        if (requests == m_requests.end())
            return nullptr;
        auto request = requests->value.find(*request_id);
        return request == requests->value.end() ? nullptr : &request->value;
    }();

    if (is_navigation) {
        // The navigation's URL, or where it redirected.
        if (!navigable_id.has_value())
            return {};
        auto navigation_url = page.url_of_navigation_populated_in(*navigable_id);
        auto hosted = page.hosted_navigable(*navigable_id);
        bool is_known = (navigation_url.has_value() && navigation_url->equals(url, URL::ExcludeFragment::Yes))
            || (hosted.has_value() && hosted->active_document_url().equals(url, URL::ExcludeFragment::Yes));
        bool is_redirect = !is_known && known_request && any_of(known_request->redirects, [&](auto const& redirect) { return redirect.equals(url, URL::ExcludeFragment::Yes); });
        if (!is_known && !is_redirect)
            return {};
        // Where the page says a navigation redirected: credentials only for a site of the navigation's or the page's.
        if (purpose == Purpose::Authentication && is_redirect
            && !(navigation_url.has_value() && navigation_url->origin().is_same_site(url.origin()))
            && !page.hosts_a_document_same_site_with(url.origin()))
            return {};
    } else if (purpose == Purpose::Authentication) {
        if (!navigable_id.has_value() || !page.hosted_navigable(*navigable_id).has_value())
            return {};
        if (!for_own_extension && !page.hosts_a_document_same_site_with(url.origin()))
            return {};
    }

    renderer_id = *request_id;
    JsonObject details;
    details.set("requestId"sv, request_of(page, *request_id).id);
    details.set("url"sv, url.serialize());
    details.set("method"sv, *method);
    details.set("type"sv, *type);
    details.set("timeStamp"sv, reported->get_double_with_precision_loss("timeStamp"sv).value_or(0));
    if (initiator_string.has_value())
        details.set("initiator"sv, initiator.has_value() ? initiator->origin().serialize() : "null"_string);

    for (auto key : { "statusLine"sv, "redirectUrl"sv, "error"sv, "scheme"sv, "realm"sv }) {
        if (auto value = reported->get_string(key); value.has_value())
            details.set(key, *value);
    }
    if (auto status = reported->get_integer<i64>("statusCode"sv); status.has_value() && *status >= 0 && *status < 1000)
        details.set("statusCode"sv, *status);
    for (auto key : { "fromCache"sv, "isProxy"sv }) {
        if (auto value = reported->get_bool(key); value.has_value())
            details.set(key, *value);
    }
    if (purpose == Purpose::Authentication) {
        // The challenger is the URL's host, as the browser knows it.
        JsonObject challenger;
        challenger.set("host"sv, url.serialized_host());
        challenger.set("port"sv, url.port_or_default());
        details.set("challenger"sv, move(challenger));
    }
    for (auto key : { "requestHeaders"sv, "responseHeaders"sv }) {
        auto headers = reported->get_array(key);
        if (!headers.has_value())
            continue;
        JsonArray checked;
        headers->for_each([&](JsonValue const& header) {
            if (!header.is_object())
                return;
            auto name = header.as_object().get_string("name"sv);
            auto value = header.as_object().get_string("value"sv);
            if (!name.has_value() || !value.has_value())
                return;
            JsonObject entry;
            entry.set("name"sv, *name);
            entry.set("value"sv, *value);
            checked.must_append(move(entry));
        });
        details.set(key, move(checked));
    }

    // The frame, as the browser knows it.
    frame = {};
    if (navigable_id.has_value() && runtime().tabs_hooks.describe_frame) {
        if (auto described = runtime().tabs_hooks.describe_frame(page, *navigable_id); described.has_value())
            frame = described.release_value();
    }
    details.set("tabId"sv, frame.tab_id);
    details.set("frameId"sv, frame.frame_id);
    details.set("parentFrameId"sv, frame.parent_frame_id);
    details.set("frameType"sv, frame.is_top_level ? "outermost_frame"sv : "sub_frame"sv);
    details.set("documentLifecycle"sv, "active"sv);
    if (!is_navigation && frame.document_id.has_value())
        details.set("documentId"sv, *frame.document_id);
    if (frame.parent_document_id.has_value())
        details.set("parentDocumentId"sv, *frame.parent_document_id);
    return details;
}

WebRequestAPI::Filter WebRequestAPI::filter_from(JsonArray const& extra)
{
    Filter filter;
    if (extra.is_empty() || !extra.at(0).is_object())
        return filter;
    auto const& object = extra.at(0).as_object();
    auto urls = object.get_array("urls"sv);
    if (!urls.has_value() || urls->size() > maximum_filter_urls)
        return filter;
    urls->for_each([&](JsonValue const& pattern) {
        if (!pattern.is_string())
            return;
        if (auto parsed = WebExtensions::MatchPattern::parse(pattern.as_string()); !parsed.is_error())
            filter.urls.append(parsed.release_value());
    });
    if (auto types = object.get_array("types"sv); types.has_value()) {
        if (types->size() > maximum_filter_types)
            return filter;
        filter.types = Vector<String> {};
        types->for_each([&](JsonValue const& value) {
            if (value.is_string())
                filter.types->append(value.as_string());
        });
    }
    if (auto tab = object.get("tabId"sv); tab.has_value()) {
        filter.has_tab_id = true;
        filter.tab_id = tab->get_integer<i64>();
    }
    if (auto window = object.get("windowId"sv); window.has_value()) {
        filter.has_window_id = true;
        filter.window_id = window->get_integer<i64>();
    }
    if (extra.size() >= 2 && extra.at(1).is_array()) {
        extra.at(1).as_array().for_each([&](JsonValue const& value) {
            if (!value.is_string())
                return;
            auto const& spec = value.as_string();
            filter.request_headers |= spec == "requestHeaders"sv;
            filter.response_headers |= spec == "responseHeaders"sv;
            filter.blocking |= spec == "blocking"sv;
            filter.async_blocking |= spec == "asyncBlocking"sv;
        });
    }
    filter.is_valid = true;
    return filter;
}

WebRequestAPI::Filter const& WebRequestAPI::filter_of(Context const& context, String const& event, i64 listener, JsonArray const& extra)
{
    auto& listeners = m_filters.ensure(context.id).ensure(event);
    return listeners.ensure(listener, [&] { return filter_from(extra); });
}

bool WebRequestAPI::listener_wants(Filter const& filter, JsonObject const& details, ExtensionRuntime::TabsHooks::Frame const& frame, URL::URL const& url)
{
    if (!filter.is_valid || !any_of(filter.urls, [&](auto const& pattern) { return pattern.matches(url); }))
        return false;
    if (filter.types.has_value() && !filter.types->contains_slow(details.get_string("type"sv).value_or({})))
        return false;
    if (filter.has_tab_id && filter.tab_id != frame.tab_id)
        return false;
    if (filter.has_window_id && filter.window_id != frame.window_id)
        return false;
    return true;
}

// What a listener gets: headers only when it asked for them.
JsonObject WebRequestAPI::details_for(Filter const& filter, JsonObject details)
{
    if (!filter.response_headers)
        details.remove("responseHeaders"sv);
    if (!filter.request_headers)
        details.remove("requestHeaders"sv);
    return details;
}

void WebRequestAPI::event_reported(WebContentPage& page, JsonObject const& message)
{
    auto event = message.get_string("event"sv);
    if (!event.has_value() || !event->starts_with_bytes("webRequest."sv))
        return;
    auto short_name = event->bytes_as_string_view().substring_view(11);
    if (!web_request_events.contains_slow(short_name) || short_name == "onAuthRequired"sv)
        return;

    URL::URL url;
    Optional<URL::URL> initiator;
    ExtensionRuntime::TabsHooks::Frame frame;
    String renderer_id;
    auto details = details_from(page, message, Purpose::Event, url, initiator, frame, renderer_id);
    if (!details.has_value())
        return;

    // A navigation's redirect is where its next events may be for; a request that ended is forgotten.
    auto type = details->get_string("type"sv).value_or({});
    if (short_name == "onBeforeRedirect"sv && type.is_one_of("main_frame"sv, "sub_frame"sv)) {
        auto& request = request_of(page, renderer_id);
        auto redirect_url = URL::Parser::basic_parse(details->get_string("redirectUrl"sv).value_or({}));
        if (redirect_url.has_value() && request.redirects.size() < maximum_redirects)
            request.redirects.append(redirect_url.release_value());
    }
    if (short_name.is_one_of("onCompleted"sv, "onErrorOccurred"sv)) {
        if (auto requests = m_requests.find(&page); requests != m_requests.end())
            requests->value.remove(renderer_id);
    }

    runtime().for_each_context([&](Context const& context) {
        if (context.type == "content_script"sv)
            return;
        auto listeners = context.listeners.find(*event);
        if (listeners == context.listeners.end() || listeners->value.is_empty())
            return;
        auto const* extension = runtime().registry().find(context.extension_id);
        if (!extension || !may_see(*extension, url, initiator))
            return;
        for (auto const& [listener, extra] : listeners->value) {
            auto const& filter = filter_of(context, *event, listener, extra);
            if (!listener_wants(filter, *details, frame, url))
                continue;
            JsonObject send;
            send.set("type"sv, "event"sv);
            send.set("event"sv, *event);
            send.set("args"sv, JsonArray { Vector<JsonValue> { details_for(filter, *details) } });
            send.set("listener"sv, listener);
            runtime().send(context, send);
        }
    });
}

// onAuthRequired.
void WebRequestAPI::authentication_requested(WebContentPage& page, JsonObject const& message)
{
    auto renderer_request = message.get_u64("request"sv);
    if (!renderer_request.has_value())
        return;
    auto reply_nothing = [&] {
        JsonObject reply;
        reply.set("type"sv, "web_request_auth_reply"sv);
        reply.set("request"sv, *renderer_request);
        if (page.is_open())
            page.async_deliver_extension_message(0, reply.serialized());
    };

    size_t pending_for_page = 0;
    for (auto const& [id, pending] : m_pending_authentications) {
        if (pending.page.ptr() == &page)
            ++pending_for_page;
    }
    URL::URL url;
    Optional<URL::URL> initiator;
    ExtensionRuntime::TabsHooks::Frame frame;
    String renderer_id;
    auto details = pending_for_page < maximum_pending_authentications_per_page ? details_from(page, message, Purpose::Authentication, url, initiator, frame, renderer_id) : Optional<JsonObject> {};
    if (!details.has_value())
        return reply_nothing();

    auto id = m_next_authentication++;
    PendingAuthentication pending;
    pending.page = page;
    pending.renderer_request = *renderer_request;

    // Blocking listeners are asked, the most recently installed extension's first; the others just hear of it.
    auto order_of = [order = extensions_in_order()](StringView extension_id) -> size_t {
        for (size_t i = 0; i < order.size(); ++i) {
            if (order[i]->id() == extension_id)
                return i;
        }
        return order.size();
    };
    Vector<Function<void()>> sends;
    runtime().for_each_context([&](Context const& context) {
        if (context.type == "content_script"sv)
            return;
        auto listeners = context.listeners.find("webRequest.onAuthRequired"sv);
        if (listeners == context.listeners.end() || listeners->value.is_empty())
            return;
        auto const* extension = runtime().registry().find(context.extension_id);
        if (!extension || !may_see(*extension, url, initiator))
            return;
        bool may_answer = runtime().management().has_permission(extension->id(), "webRequestAuthProvider"sv);
        for (auto const& [listener, extra] : listeners->value) {
            auto const& filter = filter_of(context, "webRequest.onAuthRequired"_string, listener, extra);
            if (!listener_wants(filter, *details, frame, url))
                continue;
            JsonObject send;
            send.set("type"sv, "event"sv);
            send.set("event"sv, "webRequest.onAuthRequired"sv);
            send.set("args"sv, JsonArray { Vector<JsonValue> { details_for(filter, *details) } });
            send.set("listener"sv, listener);
            bool is_async = filter.async_blocking;
            if (may_answer && (is_async || filter.blocking)) {
                auto blocking = m_next_blocking++;
                send.set("blocking"sv, blocking);
                send.set("async"sv, is_async);
                m_asked.set(blocking, { id, context.id, order_of(extension->id()) });
                ++pending.remaining;
            }
            sends.append([this, context_id = context.id, send = move(send)] {
                if (auto const* context = runtime().find_context(context_id))
                    runtime().send(*context, send);
            });
        }
    });

    if (pending.remaining == 0) {
        for (auto& send : sends)
            send();
        return reply_nothing();
    }
    pending.timeout = Core::Timer::create_single_shot(authentication_timeout, [this, id] {
        if (!m_pending_authentications.contains(id))
            return;
        m_asked.remove_all_matching([&](auto const&, auto const& asked) { return asked.authentication == id; });
        answer_authentication(id);
    });
    pending.timeout->start();
    m_pending_authentications.set(id, move(pending));
    for (auto& send : sends)
        send();
}

void WebRequestAPI::blocking_response(Context const& context, JsonObject const& message)
{
    auto blocking = message.get_u64("blocking"sv);
    if (!blocking.has_value())
        return;
    auto asked = m_asked.get(*blocking);
    // Only the context asked may answer.
    if (!asked.has_value() || asked->context_id != context.id)
        return;
    m_asked.remove(*blocking);
    auto pending = m_pending_authentications.find(asked->authentication);
    if (pending == m_pending_authentications.end())
        return;
    pending->value.answers.append({ asked->order, message.get("response"sv).value_or(JsonValue {}) });
    if (--pending->value.remaining == 0)
        answer_authentication(asked->authentication);
}

void WebRequestAPI::answer_authentication(u64 id)
{
    auto pending = m_pending_authentications.take(id);
    if (!pending.has_value())
        return;
    if (pending->timeout)
        pending->timeout->stop();

    quick_sort(pending->answers, [](auto const& a, auto const& b) { return a.order < b.order; });
    JsonObject reply;
    reply.set("type"sv, "web_request_auth_reply"sv);
    reply.set("request"sv, pending->renderer_request);
    bool cancel = false;
    for (auto const& answer : pending->answers) {
        if (!answer.response.is_object())
            continue;
        auto const& response = answer.response.as_object();
        if (auto credentials = response.get_object("authCredentials"sv); credentials.has_value()) {
            auto username = credentials->get_string("username"sv);
            auto password = credentials->get_string("password"sv);
            if (username.has_value() && password.has_value()) {
                JsonObject given;
                given.set("username"sv, *username);
                given.set("password"sv, *password);
                reply.set("credentials"sv, move(given));
                break;
            }
        }
        if (response.get_bool("cancel"sv) == true)
            cancel = true;
    }
    if (!reply.has("credentials"sv) && cancel)
        reply.set("cancel"sv, true);
    if (pending->page && pending->page->is_open())
        pending->page->async_deliver_extension_message(0, reply.serialized());
}

}
