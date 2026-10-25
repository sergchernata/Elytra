/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// chrome.permissions, chrome.management, chrome.privacy and the rest of chrome.runtime.

#include "ExtensionTestHarness.h"
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibCore/Timer.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionAPI/Management.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/Settings.h>

namespace ExtensionTests {

namespace {

constexpr auto management_id = "epcdjeeimbldakmnfpideclgjeiflopp"sv;
constexpr auto other_id = "njaomanglkphpmpbpgpbeolgfhgafipk"sv;
constexpr auto permissions_id = "cdgfcddbilmookcknhbepmneijognhgb"sv;

WebView::ExtensionRuntime& runtime() { return WebView::Application::extension_runtime(); }

URL::URL page(StringView id, StringView step)
{
    return url(ByteString::formatted("chrome-extension://{}/page.html?step={}", id, step));
}

JsonObject parsed(JsonObject const& report, StringView key)
{
    auto json = JsonParser::parse(string_in(report, key));
    if (json.is_error() || !json.value().is_object())
        return {};
    return json.value().as_object();
}

ByteString storage_directory(Harness const& harness)
{
    return ByteString::formatted("{}/Storage", harness.directory);
}

void write_state(Harness const& harness, StringView id, StringView contents)
{
    auto directory = ByteString::formatted("{}/{}", storage_directory(harness), id);
    MUST(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    write_temporary_file(directory, "runtime.json"sv, contents);
}

// A first install hears onInstalled "install"; a known extension at a browser start hears onStartup; a new browser
// version gives "chrome_update" too.
void lifecycle_events(Harness& harness)
{
    write_state(harness, other_id, ByteString::formatted(R"({{"version":"3.1","browser_version":"{}"}})", BROWSER_VERSION));
    write_state(harness, permissions_id, R"({"version":"1.0","browser_version":"0.1"})"sv);

    MUST(harness.registry.install_unpacked(fixture("management"sv)));
    MUST(harness.registry.install_unpacked(fixture("management-other"sv)));
    MUST(harness.registry.install_unpacked(fixture("permissions"sv)));
    harness.registry.set_allows_file_access(management_id, true);

    // It also hears of the two installed after it, though its background was still starting: what happens meanwhile waits
    // for the background.
    auto report = report_of(harness.view, page(management_id, "lifecycle"sv));
    CHECK_EQ(string_in(report, "events"sv), MUST(String::formatted(R"([{{"onInstalled":{{"reason":"install"}}}},{{"management.onInstalled":"{}"}},{{"management.onInstalled":"{}"}}])", other_id, permissions_id)));

    report = report_of(harness.view, page(other_id, "lifecycle"sv));
    CHECK_EQ(string_in(report, "events"sv), R"([{"onStartup":{}}])"sv);

    report = report_of(harness.view, url(ByteString::formatted("chrome-extension://{}/page.html?step=lifecycle&count=2", permissions_id)));
    CHECK_EQ(string_in(report, "events"sv), R"([{"onInstalled":{"reason":"chrome_update"}},{"onStartup":{}}])"sv);

    // What was seen is kept, for the next start.
    auto state = MUST(Core::File::open(ByteString::formatted("{}/{}/runtime.json", storage_directory(harness), management_id), Core::File::OpenMode::Read));
    auto json = MUST(JsonParser::parse(MUST(state->read_until_eof())));
    CHECK_EQ(json.as_object().get_string("version"sv).value_or({}), "1.0"sv);
    CHECK_EQ(json.as_object().get_string("browser_version"sv).value_or({}), (StringView { BROWSER_VERSION, __builtin_strlen(BROWSER_VERSION) }));
}

// A new version of an installed extension hears "update", with the version it replaced.
void update_event(Harness& harness)
{
    auto directory = ByteString::formatted("{}/management-2", harness.directory);
    MUST(FileSystem::copy_file_or_directory(directory, fixture("management"sv), FileSystem::RecursionMode::Allowed));
    auto manifest_path = ByteString::formatted("{}/manifest.json", directory);
    auto manifest = MUST(MUST(Core::File::open(manifest_path, Core::File::OpenMode::Read))->read_until_eof());
    auto updated = StringView { manifest }.replace("\"version\": \"1.0\""sv, "\"version\": \"2.0\""sv, ReplaceMode::FirstOnly);
    MUST(FileSystem::remove(manifest_path, FileSystem::RecursionMode::Disallowed));
    write_temporary_file(directory, "manifest.json"sv, updated);
    MUST(harness.registry.install_unpacked(directory));

    auto report = report_of(harness.view, page(management_id, "lifecycle"sv));
    CHECK_EQ(string_in(report, "events"sv), R"([{"onInstalled":{"reason":"update","previousVersion":"1.0"}}])"sv);
}

struct RefCountedFunction : RefCounted<RefCountedFunction> {
    explicit RefCountedFunction(Function<void(bool)> f)
        : function(move(f))
    {
    }
    Function<void(bool)> function;
};

Vector<NonnullRefPtr<Core::Timer>>& pending_timers()
{
    static auto* timers = new Vector<NonnullRefPtr<Core::Timer>>;
    return *timers;
}

struct PermissionPrompt {
    String extension_id;
    Vector<String> permissions;
    Vector<String> origins;
};

void permissions(Harness& harness)
{
    Vector<PermissionPrompt> prompts;
    runtime().request_permissions_prompt = [&](WebView::Extension const& extension, Vector<String> permissions, Vector<String> origins, Function<void(bool)> answer) {
        bool deny = false;
        for (auto const& origin : origins)
            deny |= origin.contains("deny"sv);
        prompts.append({ extension.id(), move(permissions), move(origins) });
        answer(!deny);
    };

    auto report = report_of(harness.view, page(permissions_id, "permissions"sv));
    CHECK_EQ(string_in(report, "all"sv), R"({"permissions":["storage"],"origins":["https://granted.example/*","https://scripted.example/*"]})"sv);
    CHECK(report.get_bool("contains_required"sv).value_or(false));
    CHECK(!report.get_bool("contains_optional"sv).value_or(true));
    CHECK(report.get_bool("contains_origin"sv).value_or(false));
    CHECK(report.get_bool("contains_origin_path"sv).value_or(false));
    CHECK(report.get_bool("contains_scripted"sv).value_or(false));
    CHECK(!report.get_bool("contains_other_origin"sv).value_or(true));
    CHECK(!report.get_bool("contains_all_urls"sv).value_or(true));
    CHECK(string_in(report, "contains_bad_origin"sv).starts_with_bytes("error: Invalid value for origin pattern not a pattern"sv));
    CHECK(string_in(report, "contains_bad_type"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "contains_bad_list"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "contains_nothing"sv).starts_with_bytes("error:"sv));

    CHECK(report.get_bool("request"sv).value_or(false));
    CHECK(report.get_bool("contains_granted"sv).value_or(false));
    CHECK(report.get_bool("request_again"sv).value_or(false));
    CHECK_EQ(string_in(report, "request_undeclared"sv), "error: Only permissions specified in the manifest may be requested."sv);
    CHECK(report.get_bool("request_origin"sv).value_or(false));
    CHECK(report.get_bool("contains_granted_origin"sv).value_or(false));
    CHECK(!report.get_bool("contains_wider_origin"sv).value_or(true));
    CHECK(!report.get_bool("request_denied"sv).value_or(true));
    CHECK(!report.get_bool("contains_denied"sv).value_or(true));
    CHECK_EQ(string_in(report, "request_undeclared_origin"sv), "error: Only permissions specified in the manifest may be requested."sv);
    CHECK(string_in(report, "request_bad"sv).starts_with_bytes("error:"sv));
    CHECK_EQ(string_in(report, "all_granted"sv), R"({"permissions":["storage","notifications"],"origins":["https://granted.example/*","https://scripted.example/*","https://www.optional.example/*"]})"sv);

    CHECK_EQ(string_in(report, "remove_required"sv), "error: You cannot remove required permissions."sv);
    CHECK_EQ(string_in(report, "remove_required_origin"sv), "error: You cannot remove required permissions."sv);
    CHECK(report.get_bool("remove"sv).value_or(false));
    CHECK(!report.get_bool("contains_removed"sv).value_or(true));
    CHECK(report.get_bool("request_idle"sv).value_or(false));

    CHECK_EQ(string_in(report, "host_access_request"sv), "ok"sv);
    CHECK_EQ(string_in(report, "host_access_request_pattern"sv), "ok"sv);
    CHECK(string_in(report, "host_access_request_empty"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "host_access_request_both"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "host_access_request_bad_tab"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "host_access_request_bad_pattern"sv).starts_with_bytes("error:"sv));
    CHECK_EQ(string_in(report, "remove_host_access_request"sv), "ok"sv);

    CHECK_EQ(string_in(report, "events"sv), R"([{"permissions.onAdded":{"permissions":["notifications"],"origins":[]}},{"permissions.onAdded":{"permissions":[],"origins":["https://www.optional.example/*"]}},{"permissions.onRemoved":{"permissions":["notifications"],"origins":[]}},{"permissions.onAdded":{"permissions":["idle"],"origins":[]}}])"sv);

    // The user is asked only for what isn't granted yet.
    CHECK_EQ(prompts.size(), 4u);
    if (prompts.size() == 4) {
        CHECK_EQ(prompts[0].extension_id, permissions_id);
        CHECK_EQ(prompts[0].permissions, Vector<String> { "notifications"_string });
        CHECK_EQ(prompts[1].origins, Vector<String> { "https://www.optional.example/*"_string });
        CHECK_EQ(prompts[2].origins, Vector<String> { "https://deny.optional.example/*"_string });
        CHECK_EQ(prompts[3].permissions, Vector<String> { "idle"_string });
    }

    // Granted permissions count for the browser's own checks, and are kept.
    auto& management = runtime().management();
    CHECK(management.has_permission(permissions_id, "idle"sv));
    CHECK(management.has_permission(permissions_id, "storage"sv));
    CHECK(!management.has_permission(permissions_id, "notifications"sv));
    CHECK(!management.has_permission(other_id, "idle"sv));
    CHECK(management.has_host_permission(permissions_id, url("https://www.optional.example/a"sv)));
    CHECK(management.has_host_permission(permissions_id, url("https://granted.example/a"sv)));
    CHECK(!management.has_host_permission(permissions_id, url("https://deny.optional.example/a"sv)));
    auto saved = MUST(MUST(Core::File::open(ByteString::formatted("{}/{}/management.json", storage_directory(harness), permissions_id), Core::File::OpenMode::Read))->read_until_eof());
    CHECK(StringView { saved }.contains("\"idle\""sv));
    CHECK(StringView { saved }.contains("https://www.optional.example/*"sv));

    // Without anyone to ask, nothing more is granted.
    runtime().request_permissions_prompt = nullptr;
    report = report_of(harness.view, page(permissions_id, "deny"sv));
    CHECK(!report.get_bool("request"sv).value_or(true));
    CHECK(report.get_bool("still_granted"sv).value_or(false));
}

void management(Harness& harness)
{
    auto report = report_of(harness.view, page(management_id, "management"sv));
    auto self = parsed(report, "self"sv);
    CHECK_EQ(string_in(self, "id"sv), management_id);
    CHECK_EQ(string_in(self, "name"sv), "Management test extension"sv);
    CHECK_EQ(string_in(self, "shortName"sv), "Management"sv);
    CHECK_EQ(string_in(self, "description"sv), "Exercises chrome.management, chrome.privacy and chrome.runtime"sv);
    CHECK_EQ(string_in(self, "version"sv), "1.0"sv);
    CHECK_EQ(string_in(self, "versionName"sv), "1.0 beta"sv);
    CHECK(self.get_bool("mayDisable"sv).value_or(false));
    CHECK(self.get_bool("enabled"sv).value_or(false));
    CHECK_EQ(string_in(self, "installType"sv), "development"sv);
    CHECK_EQ(string_in(self, "type"sv), "extension"sv);
    CHECK_EQ(string_in(self, "homepageUrl"sv), "https://example.com/home"sv);
    CHECK_EQ(string_in(self, "optionsUrl"sv), "chrome-extension://epcdjeeimbldakmnfpideclgjeiflopp/options.html"sv);
    CHECK(self.get_bool("offlineEnabled"sv).value_or(false));
    CHECK(!self.has("disabledReason"sv));
    CHECK_EQ(self.get_array("icons"sv).value_or({}).serialized(), R"([{"size":16,"url":"chrome-extension://epcdjeeimbldakmnfpideclgjeiflopp/icon16.png"},{"size":128,"url":"chrome-extension://epcdjeeimbldakmnfpideclgjeiflopp/icon128.png"}])"sv);
    CHECK_EQ(self.get_array("permissions"sv).value_or({}).serialized(), R"(["management","privacy","storage"])"sv);
    CHECK_EQ(self.get_array("hostPermissions"sv).value_or({}).serialized(), R"(["https://granted.example/*"])"sv);

    CHECK_EQ(string_in(report, "all"sv), "cdgfcddbilmookcknhbepmneijognhgb,epcdjeeimbldakmnfpideclgjeiflopp,njaomanglkphpmpbpgpbeolgfhgafipk"sv);
    CHECK_EQ(string_in(report, "other"sv), "Other management test extension 3.1 true development"sv);
    CHECK_EQ(string_in(report, "unknown"sv), "error: Failed to find extension with id aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."sv);
    CHECK(string_in(report, "bad_id"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "disable_self"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "bad_set_id"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "bad_set_enabled"sv).starts_with_bytes("error:"sv));
    CHECK_EQ(string_in(report, "set_unknown"sv), "error: Failed to find extension with id aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."sv);
    CHECK_EQ(string_in(report, "disable"sv), "ok"sv);
    CHECK_EQ(string_in(report, "other_disabled"sv), "false unknown"sv);
    CHECK_EQ(string_in(report, "callback"sv), management_id);

    // A disabled extension's background stops.
    CHECK(!runtime().management().is_enabled(other_id));
    CHECK(runtime().background_view(other_id) == nullptr);
    CHECK(runtime().management().is_enabled(management_id));

    report = report_of(harness.view, page(management_id, "enable"sv));
    CHECK_EQ(string_in(report, "enable"sv), "ok"sv);
    CHECK(report.get_bool("other_enabled"sv).value_or(false));
    CHECK_EQ(string_in(report, "events"sv), R"([{"management.onDisabled":"njaomanglkphpmpbpgpbeolgfhgafipk"},{"management.onEnabled":"njaomanglkphpmpbpgpbeolgfhgafipk"}])"sv);
    CHECK(runtime().background_view(other_id) != nullptr);

    // Without the "management" permission, an extension sees only itself.
    report = report_of(harness.view, page(other_id, "management"sv));
    CHECK_EQ(string_in(report, "all"sv), other_id);
    CHECK_EQ(string_in(report, "self"sv), other_id);
    CHECK(string_in(report, "get_other"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "disable_other"sv).starts_with_bytes("error:"sv));
    CHECK_EQ(string_in(report, "options"sv), "error: Could not create an options page."sv);

    report = report_of(harness.view, page(permissions_id, "management"sv));
    CHECK_EQ(string_in(report, "self"sv), R"(Permissions test extension true true undefined false ["storage","idle"] ["https://granted.example/*","https://scripted.example/*","https://www.optional.example/*"])"sv);
    CHECK_EQ(string_in(report, "all"sv), permissions_id);
    CHECK(string_in(report, "uninstall_other"sv).starts_with_bytes("error:"sv));
}

void privacy(Harness& harness)
{
    auto& settings = WebView::Application::settings();
    auto search = settings.search_engine_settings();
    search.suggestions = false;
    settings.set_search_engine_settings(search);

    Vector<String> changed;
    runtime().management().on_privacy_setting_changed = [&](StringView name) { changed.append(MUST(String::from_utf8(name))); };

    auto report = report_of(harness.view, page(management_id, "privacy"sv));
    CHECK_EQ(string_in(report, "initial"sv), R"({"value":true,"levelOfControl":"controllable_by_this_extension"})"sv);
    CHECK_EQ(string_in(report, "set"sv), "ok"sv);
    CHECK_EQ(string_in(report, "after_set"sv), R"({"value":false,"levelOfControl":"controlled_by_this_extension"})"sv);
    CHECK(string_in(report, "wrong_type"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "no_value"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "no_details"sv).starts_with_bytes("error:"sv));
    CHECK_EQ(string_in(report, "incognito_scope"sv), "error: You do not have permission to access incognito preferences."sv);
    CHECK(string_in(report, "bad_scope"sv).starts_with_bytes("error:"sv));
    CHECK_EQ(string_in(report, "incognito_get"sv), "error: You do not have permission to access incognito preferences."sv);
    CHECK(string_in(report, "webrtc_bad"sv).starts_with_bytes("error:"sv));
    CHECK_EQ(string_in(report, "webrtc"sv), R"({"value":"disable_non_proxied_udp","levelOfControl":"controlled_by_this_extension"})"sv);
    CHECK_EQ(string_in(report, "defaults"sv), "alternateErrorPagesEnabled=true autofillAddressEnabled=true autofillCreditCardEnabled=true autofillEnabled=true safeBrowsingEnabled=true spellingServiceEnabled=false translationServiceEnabled=true thirdPartyCookiesAllowed=true hyperlinkAuditingEnabled=true referrersEnabled=true doNotTrackEnabled=false protectedContentEnabled=true networkPredictionEnabled=true"sv);
    CHECK_EQ(string_in(report, "suggest_off"sv), R"({"value":true,"levelOfControl":"controlled_by_this_extension"})"sv);
    CHECK_EQ(string_in(report, "on_change"sv), R"([{"privacy.onChange":{"value":false,"levelOfControl":"controlled_by_this_extension"}}])"sv);

    // What the extension sets is the browser's: Elytra has no password manager, so it's only kept for whoever asks;
    // search suggestions are Elytra's own.
    auto& management = runtime().management();
    CHECK_EQ(management.privacy_setting("services.passwordSavingEnabled"sv).serialized(), "false"sv);
    CHECK_EQ(management.privacy_setting("services.autofillEnabled"sv).serialized(), "true"sv);
    CHECK(settings.search_engine_settings().suggestions);
    CHECK(changed.contains_slow("services.passwordSavingEnabled"_string));

    // The last extension to set a setting controls it.
    report = report_of(harness.view, page(other_id, "privacy"sv));
    CHECK_EQ(string_in(report, "before"sv), R"({"value":false,"levelOfControl":"controlled_by_other_extensions"})"sv);
    CHECK_EQ(string_in(report, "set"sv), "ok"sv);
    CHECK_EQ(string_in(report, "after"sv), R"({"value":true,"levelOfControl":"controlled_by_this_extension"})"sv);

    report = report_of(harness.view, page(management_id, "privacy_after_other"sv));
    CHECK_EQ(string_in(report, "state"sv), R"({"value":true,"levelOfControl":"controlled_by_other_extensions"})"sv);
    // Cleared, the user's own value is back.
    CHECK_EQ(string_in(report, "suggest_clear"sv), R"({"value":false,"levelOfControl":"controllable_by_this_extension"})"sv);
    CHECK(!settings.search_engine_settings().suggestions);

    report = report_of(harness.view, page(other_id, "privacy_clear"sv));
    CHECK_EQ(string_in(report, "clear"sv), "ok"sv);
    CHECK(string_in(report, "bad_clear"sv).starts_with_bytes("error:"sv));
    CHECK_EQ(string_in(report, "after"sv), R"({"value":false,"levelOfControl":"controlled_by_other_extensions"})"sv);
    CHECK_EQ(management.privacy_setting("services.passwordSavingEnabled"sv).serialized(), "false"sv);

    // Without the "privacy" permission, an extension may look but not change.
    report = report_of(harness.view, page(permissions_id, "privacy"sv));
    CHECK_EQ(string_in(report, "get"sv), R"({"value":false,"levelOfControl":"not_controllable"})"sv);
    CHECK(string_in(report, "set"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "clear"sv).starts_with_bytes("error:"sv));

    runtime().management().on_privacy_setting_changed = nullptr;
}

void runtime_functions(Harness& harness)
{
    Optional<URL::URL> opened;
    runtime().open_options_page = [&](WebView::Extension const& extension, URL::URL const& url) {
        CHECK_EQ(extension.id(), management_id);
        opened = url;
    };

    auto report = report_of(harness.view, page(management_id, "runtime"sv));
    CHECK_EQ(string_in(report, "contexts"sv), "BACKGROUND -1 -1 false|TAB tab 0 chrome-extension://epcdjeeimbldakmnfpideclgjeiflopp/page.html?step=runtime false"sv);
    CHECK_EQ(string_in(report, "background_contexts"sv), "1 string"sv);
    CHECK_EQ(report.get_integer<i64>("tab_by_url"sv).value_or(0), 1);
    CHECK(string_in(report, "bad_filter"sv).starts_with_bytes("error:"sv));
    CHECK(string_in(report, "bad_filter_types"sv).starts_with_bytes("error:"sv));
    CHECK_EQ(string_in(report, "options"sv), "ok"sv);
    CHECK_EQ(opened.has_value() ? opened->serialize() : "(none)"_string, "chrome-extension://epcdjeeimbldakmnfpideclgjeiflopp/options.html"sv);
    CHECK_EQ(string_in(report, "uninstall_url"sv), "ok"sv);
    CHECK(string_in(report, "uninstall_url_javascript"sv).starts_with_bytes("error: Invalid URL"sv));
    CHECK_EQ(string_in(report, "uninstall_url_long"sv), "error: URL is too long."sv);
    CHECK(string_in(report, "uninstall_url_type"sv).starts_with_bytes("error:"sv));
    CHECK_EQ(string_in(report, "update_check"sv), R"({"status":"no_update"})"sv);
    CHECK_EQ(string_in(report, "restart"sv), "error: Function available only for ChromeOS kiosk mode."sv);
    CHECK(!report.get_bool("incognito_access"sv).value_or(true));
    CHECK(report.get_bool("file_access"sv).value_or(false));
    CHECK_EQ(string_in(report, "managed"sv), "{}"sv);
    CHECK_EQ(string_in(report, "managed_keys"sv), "[]"sv);
    CHECK_EQ(report.get_integer<i64>("managed_bytes"sv).value_or(-1), 0);

    auto uninstall_url = runtime().management().uninstall_url(management_id);
    CHECK_EQ(uninstall_url.has_value() ? uninstall_url->serialize() : "(none)"_string, "https://example.com/bye"sv);
    CHECK(!runtime().management().uninstall_url(other_id).has_value());

    runtime().open_options_page = nullptr;
}

// runtime.reload restarts the background and reloads the extension's pages.
void reload(Harness& harness)
{
    auto report = report_of(harness.view, page(management_id, "reload"sv));
    CHECK(report.get_bool("reloaded"sv).value_or(false));
    CHECK_EQ(report.get_integer<i64>("starts"sv).value_or(0), 2);
}

void uninstall_self(Harness& harness)
{
    // Asking the user needs someone to ask.
    auto report = report_of(harness.view, page(management_id, "uninstall_without_hook"sv));
    CHECK_EQ(string_in(report, "uninstall"sv), "error: User cancelled uninstall of extension."sv);
    CHECK(harness.registry.find(management_id));

    Vector<String> asked;
    Vector<String> opened;
    bool confirm = false;
    runtime().confirm_uninstall = [&](WebView::Extension const& extension, WebView::Extension const*, Function<void(bool)> done) {
        asked.append(extension.id());
        done(confirm);
        confirm = true;
    };
    runtime().management().open_url_in_new_tab = [&](URL::URL const& url) { opened.append(url.serialize()); };

    report = report_of(harness.view, page(management_id, "uninstall"sv));
    CHECK(string_in(report, "bad_options"sv).starts_with_bytes("error:"sv));
    CHECK_EQ(string_in(report, "cancelled"sv), "error: User cancelled uninstall of extension."sv);
    CHECK_EQ(string_in(report, "uninstall"sv), "ok"sv);
    CHECK_EQ(asked.size(), 2u);

    // Gone, its uninstall URL opens, and those that manage extensions hear of it.
    CHECK(!harness.registry.find(management_id));
    CHECK(!harness.registry.installed_extension(management_id).has_value());
    CHECK_EQ(opened, Vector<String> { "https://example.com/bye"_string });
    CHECK(runtime().background_view(management_id) == nullptr);

    runtime().confirm_uninstall = nullptr;
    runtime().management().open_url_in_new_tab = nullptr;
}

// What asks the user needs the user's gesture in the extension's page, and doesn't pile up questions.
void prompts_need_a_gesture(Harness& harness)
{
    size_t prompts = 0;
    // The user answers a little later, saying no.
    auto answer_later = [](Function<void(bool)> answer) {
        auto shared = make_ref_counted<RefCountedFunction>(move(answer));
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(400), [shared] { shared->function(false); });
        timer->start();
        pending_timers().append(move(timer));
    };
    runtime().request_permissions_prompt = [&](WebView::Extension const&, Vector<String>, Vector<String>, Function<void(bool)> answer) {
        ++prompts;
        answer_later(move(answer));
    };

    WebView::set_user_activation_for_testing([] { return false; });
    auto report = report_of(harness.view, page(permissions_id, "gesture"sv));
    CHECK_EQ(string_in(report, "request"sv), "error: This function must be called during a user gesture"sv);
    CHECK_EQ(prompts, 0u);

    WebView::set_user_activation_for_testing([] { return true; });
    report = report_of(harness.view, page(permissions_id, "prompts"sv));
    CHECK_EQ(string_in(report, "while_asking"sv), "error: A permission request is already showing"sv);
    CHECK_EQ(string_in(report, "first"sv), "false"sv);
    // Asking again for what the user just refused doesn't ask them.
    CHECK(!report.get_bool("after_denial"sv).value_or(true));
    CHECK(!report.get_bool("other_after_denial"sv).value_or(true));
    CHECK_EQ(prompts, 2u);
    runtime().request_permissions_prompt = nullptr;

    // management.uninstall of another extension.
    size_t asked = 0;
    runtime().confirm_uninstall = [&](WebView::Extension const&, WebView::Extension const*, Function<void(bool)> done) {
        ++asked;
        answer_later(move(done));
    };
    WebView::set_user_activation_for_testing([] { return false; });
    report = report_of(harness.view, page(management_id, "uninstall_other"sv));
    CHECK_EQ(string_in(report, "first"sv), "error: chrome.management.uninstall requires a user gesture."sv);
    CHECK_EQ(asked, 0u);
    WebView::set_user_activation_for_testing([] { return true; });
    report = report_of(harness.view, page(management_id, "uninstall_other"sv));
    CHECK_EQ(string_in(report, "second"sv), "error: The user is already being asked to remove an extension."sv);
    CHECK_EQ(string_in(report, "first"sv), "error: User cancelled uninstall of extension."sv);
    CHECK_EQ(asked, 1u);
    CHECK(harness.registry.find(other_id));
    runtime().confirm_uninstall = nullptr;
    WebView::set_user_activation_for_testing({});
}

// Uninstalling forgets what the extension stored and was granted: an extension installed later with the same ID
// (the same one again, or one copying its key) starts afresh.
void uninstalling_forgets_storage_and_grants(Harness& harness)
{
    constexpr auto id = "ofhkpcbnbblidbdoohghmanghnlljfip"sv;
    MUST(harness.registry.install_unpacked(fixture("uninstall-forgets"sv)));
    runtime().request_permissions_prompt = [](auto const&, auto, auto, Function<void(bool)> on_answer) { on_answer(true); };
    auto page = [&](StringView step) { return url(MUST(String::formatted("chrome-extension://{}/page.html?step={}", id, step))); };

    auto report = report_of(harness.view, page("set"sv));
    CHECK(report.get_bool("granted"sv).value_or(false));
    CHECK_EQ(report.get_integer<i64>("stored"sv).value_or(0), 1);
    runtime().request_permissions_prompt = nullptr;

    (void)report_of(harness.view, url("data:text/html,<script>document.title = JSON.stringify({ away: true });</script>"sv));
    MUST(harness.registry.uninstall(id));
    CHECK(!FileSystem::exists(ByteString::formatted("{}/{}", storage_directory(harness), id)));

    MUST(harness.registry.install_unpacked(fixture("uninstall-forgets"sv)));
    report = report_of(harness.view, page("check"sv));
    CHECK(report.get("stored"sv).has_value() && report.get("stored"sv)->is_null());
    CHECK(!report.get_bool("alarms"sv).value_or(true));
    (void)harness.registry.uninstall(id);
}

}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return ExtensionTests::run_extension_tests(arguments, [](ExtensionTests::Harness& harness) {
        using namespace ExtensionTests;
        runtime().set_storage_directory(storage_directory(harness));
        harness.registry.set_extensions_directory(ByteString::formatted("{}/Extensions", harness.directory));
        lifecycle_events(harness);
        permissions(harness);
        management(harness);
        privacy(harness);
        runtime_functions(harness);
        reload(harness);
        prompts_need_a_gesture(harness);
        update_event(harness);
        uninstall_self(harness);
        uninstalling_forgets_storage_and_grants(harness);
    });
}
