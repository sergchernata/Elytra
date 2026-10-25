/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebExtensions/Manifest.h>

using namespace WebExtensions;

static ErrorOr<Manifest> parse(StringView json)
{
    return Manifest::parse(json);
}

static bool fails(StringView json)
{
    return Manifest::parse(json).is_error();
}

// A trimmed copy of 1Password 8.12.41.1's manifest.json, keeping every key and shape it uses.
static constexpr auto onepassword_manifest = R"~~~({
   "action": {
      "default_popup": "popup/index.html",
      "default_title": "1Password"
   },
   "author": "AgileBits",
   "background": {
      "service_worker": "background/background.js",
      "type": "module"
   },
   "commands": {
      "_execute_action": {
         "suggested_key": {
            "default": "Ctrl+Shift+X",
            "mac": "Command+Shift+X"
         }
      },
      "lock": {
         "description": "Lock the extension",
         "name": "lock",
         "suggested_key": {
            "default": "Ctrl+Shift+L",
            "mac": "Command+Shift+L"
         }
      }
   },
   "content_scripts": [ {
      "all_frames": true,
      "js": [ "inline/inject-content-scripts.js" ],
      "matches": [ "<all_urls>" ],
      "run_at": "document_start"
   }, {
      "all_frames": true,
      "js": [ "inline/injected/webauthn-listeners.js" ],
      "matches": [ "https://*/*", "http://localhost/*" ],
      "run_at": "document_start",
      "world": "MAIN"
   }, {
      "css": [ "inline/injected/styles/b5integration.css" ],
      "js": [ "inline/injected/b5.js" ],
      "matches": [ "https://*.1password.ca/*", "https://*.1password.com/*" ],
      "run_at": "document_idle"
   }, {
      "all_frames": false,
      "js": [ "inline/injected/secure-remote-autofill-start-pairing.js" ],
      "matches": [ "https://www.director.ai/?*", "https://www.director.ai/" ],
      "run_at": "document_end"
   } ],
   "content_security_policy": {
      "extension_pages": "default-src 'none'; base-uri 'none'; img-src 'self' data: blob:; font-src 'self'; connect-src https://*.1password.com wss://b5n.1password.com http://127.0.0.1:12519; style-src 'self' 'unsafe-inline'; script-src 'self' 'wasm-unsafe-eval'"
   },
   "declarative_net_request": {
      "rule_resources": [ {
         "enabled": true,
         "id": "ruleset_1",
         "path": "rules_1.json"
      } ]
   },
   "default_locale": "en",
   "description": "__MSG_extDescription__",
   "homepage_url": "https://1password.com",
   "host_permissions": [ "<all_urls>" ],
   "icons": {
      "128": "images/icons/onepassword-128.png",
      "16": "images/icons/app_icon-light_bg-color-unlocked-32.png",
      "48": "images/icons/onepassword-48.png"
   },
   "key": "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAnHpaUll4uWujpAdbIXOQY2WE6hk8PllsYsnoUaj5qHXwv4IB6A9pONqGaTL2KL20u6E6XVhncY6Ae6SQSBQqiIkgjPsiG0NDNsDlju/kzBnfimKFC/bpzOrqFqbhswQHifnet5uHlpG97whTzLO3ka0M5aqB9V9mD/0qVXvNgAVVnSTULH254YqpeCcAhmsKiFZSL6OrOZmCp8kZ/OeOUK9iYWYylL7VcOXVrZf10EPrlaCNXzVk7K35dPuQ7svhA0Pgju3kngB4RLa5Iojhw3IT+B5+m8pisjOSd1oKMrRmhGs7rDhF5IEtAiVxqVp7uOOMPQj3vrbMDAzf7vqLtQIDAQAB",
   "manifest_version": 3,
   "minimum_chrome_version": "128",
   "name": "__MSG_extName__",
   "permissions": [ "alarms", "contextMenus", "downloads", "idle", "management", "nativeMessaging", "notifications", "privacy", "scripting", "storage", "tabs", "webNavigation", "webRequest", "webRequestAuthProvider", "declarativeNetRequestWithHostAccess" ],
   "short_name": "1Password",
   "update_url": "https://clients2.google.com/service/update2/crx",
   "version": "8.12.41.1",
   "web_accessible_resources": [ {
      "matches": [ "<all_urls>" ],
      "resources": [ "fonts/*.ttf", "inline/menu/menu.html", "*.js.map" ]
   } ]
})~~~"sv;

TEST_CASE(onepassword_manifest)
{
    auto manifest = TRY_OR_FAIL(parse(onepassword_manifest));
    EXPECT_EQ(manifest.manifest_version, 3);
    EXPECT_EQ(manifest.name, "__MSG_extName__"sv);
    EXPECT_EQ(manifest.short_name, "1Password"sv);
    EXPECT_EQ(manifest.version, "8.12.41.1"sv);
    EXPECT_EQ(manifest.description, "__MSG_extDescription__"sv);
    EXPECT_EQ(manifest.default_locale, "en"sv);
    EXPECT_EQ(manifest.author, "AgileBits"sv);
    EXPECT(manifest.key.has_value());
    EXPECT(manifest.key->starts_with_bytes("MIIBIjAN"sv));
    EXPECT_EQ(manifest.minimum_chrome_version, "128"sv);
    EXPECT_EQ(manifest.homepage_url, "https://1password.com"sv);

    EXPECT_EQ(manifest.icons.size(), 3u);
    EXPECT_EQ(manifest.icons.get(128), "images/icons/onepassword-128.png"sv);
    EXPECT_EQ(manifest.icons.get(16), "images/icons/app_icon-light_bg-color-unlocked-32.png"sv);

    EXPECT(manifest.action.has_value());
    EXPECT_EQ(manifest.action->default_popup, "popup/index.html"sv);
    EXPECT_EQ(manifest.action->default_title, "1Password"sv);
    EXPECT(manifest.action->default_icon.is_empty());

    EXPECT(manifest.background.has_value());
    EXPECT_EQ(manifest.background->service_worker, "background/background.js"sv);
    EXPECT_EQ(manifest.background->type, BackgroundInfo::Type::Module);

    EXPECT_EQ(manifest.commands.size(), 2u);
    auto execute_action = manifest.command("_execute_action"sv);
    EXPECT(execute_action.has_value());
    EXPECT(!execute_action->description.has_value());
    EXPECT_EQ(execute_action->suggested_key.default_key, "Ctrl+Shift+X"sv);
    EXPECT_EQ(execute_action->suggested_key.for_platform("mac"sv), "Command+Shift+X"sv);
    EXPECT_EQ(execute_action->suggested_key.for_platform("linux"sv), "Ctrl+Shift+X"sv);
    auto lock = manifest.command("lock"sv);
    EXPECT(lock.has_value());
    EXPECT_EQ(lock->description, "Lock the extension"sv);

    EXPECT_EQ(manifest.content_scripts.size(), 4u);
    auto const& first = manifest.content_scripts[0];
    EXPECT_EQ(first.matches, (Vector<String> { "<all_urls>"_string }));
    EXPECT_EQ(first.js, (Vector<String> { "inline/inject-content-scripts.js"_string }));
    EXPECT(first.all_frames);
    EXPECT_EQ(first.run_at, ContentScript::RunAt::DocumentStart);
    EXPECT_EQ(first.world, ContentScript::World::Isolated);
    EXPECT_EQ(manifest.content_scripts[1].world, ContentScript::World::Main);
    EXPECT_EQ(manifest.content_scripts[1].matches.size(), 2u);
    EXPECT_EQ(manifest.content_scripts[2].css, (Vector<String> { "inline/injected/styles/b5integration.css"_string }));
    EXPECT_EQ(manifest.content_scripts[2].run_at, ContentScript::RunAt::DocumentIdle);
    EXPECT(!manifest.content_scripts[2].all_frames);
    EXPECT_EQ(manifest.content_scripts[3].run_at, ContentScript::RunAt::DocumentEnd);

    EXPECT(manifest.content_security_policy.extension_pages.has_value());
    EXPECT(manifest.extension_pages_csp().starts_with_bytes("default-src 'none'"sv));
    EXPECT(!manifest.content_security_policy.sandbox.has_value());

    EXPECT(manifest.declarative_net_request.has_value());
    EXPECT_EQ(manifest.declarative_net_request->rule_resources.size(), 1u);
    EXPECT_EQ(manifest.declarative_net_request->rule_resources[0].id, "ruleset_1"sv);
    EXPECT(manifest.declarative_net_request->rule_resources[0].enabled);
    EXPECT_EQ(manifest.declarative_net_request->rule_resources[0].path, "rules_1.json"sv);

    EXPECT_EQ(manifest.host_permissions, (Vector<String> { "<all_urls>"_string }));
    EXPECT_EQ(manifest.permissions.size(), 15u);
    EXPECT(manifest.has_permission("nativeMessaging"sv));
    EXPECT(manifest.has_permission("declarativeNetRequestWithHostAccess"sv));
    EXPECT(!manifest.has_permission("cookies"sv));
    EXPECT(!manifest.has_permission("nativemessaging"sv));

    EXPECT_EQ(manifest.web_accessible_resources.size(), 1u);
    EXPECT_EQ(manifest.web_accessible_resources[0].resources.size(), 3u);
    EXPECT_EQ(manifest.web_accessible_resources[0].matches, (Vector<String> { "<all_urls>"_string }));
    EXPECT(!manifest.web_accessible_resources[0].use_dynamic_url);

    EXPECT_EQ(manifest.incognito, IncognitoMode::Spanning);
    // Unknown keys survive in raw.
    EXPECT_EQ(manifest.raw.get_string("update_url"sv), "https://clients2.google.com/service/update2/crx"sv);
}

TEST_CASE(minimal_manifest_defaults)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1"})"sv));
    EXPECT_EQ(manifest.name, "x"sv);
    EXPECT_EQ(manifest.version, "1"sv);
    EXPECT(!manifest.action.has_value());
    EXPECT(!manifest.background.has_value());
    EXPECT(manifest.content_scripts.is_empty());
    EXPECT(manifest.permissions.is_empty());
    EXPECT(!manifest.key.has_value());
    EXPECT_EQ(manifest.extension_pages_csp(), "script-src 'self';"sv);
    EXPECT_EQ(manifest.sandbox_csp(), Manifest::default_sandbox_csp());
    EXPECT(manifest.warnings.is_empty());
}

TEST_CASE(csp_defaults_match_chrome)
{
    EXPECT_EQ(Manifest::default_mv3_extension_pages_csp(), "script-src 'self';"sv);
    EXPECT_EQ(Manifest::minimum_mv3_extension_pages_csp(), "script-src 'self' 'wasm-unsafe-eval' 'inline-speculation-rules'; object-src 'self';"sv);
    EXPECT_EQ(Manifest::default_mv2_extension_pages_csp(), "script-src 'self' blob: filesystem:; object-src 'self' blob: filesystem:;"sv);
    EXPECT_EQ(Manifest::default_sandbox_csp(), "sandbox allow-scripts allow-forms allow-popups allow-modals; script-src 'self' 'unsafe-inline' 'unsafe-eval'; child-src 'self';"sv);
}

TEST_CASE(rejects_non_objects_and_bad_json)
{
    EXPECT(fails(""sv));
    EXPECT(fails("[]"sv));
    EXPECT(fails("\"x\""sv));
    EXPECT(fails("null"sv));
    EXPECT(fails("42"sv));
    EXPECT(fails("{"sv));
    EXPECT(fails(R"({"manifest_version": 3, "name": "x", "version": "1")"sv));
    EXPECT(fails(R"({"manifest_version": 3, "name": "x", "version": "1"} trailing)"sv));
    EXPECT(fails("\xff\xfe{}"sv));
}

TEST_CASE(manifest_version_validation)
{
    EXPECT(fails(R"({"name": "x", "version": "1"})"sv));
    EXPECT(fails(R"({"manifest_version": 1, "name": "x", "version": "1"})"sv));
    EXPECT(fails(R"({"manifest_version": 4, "name": "x", "version": "1"})"sv));
    EXPECT(fails(R"({"manifest_version": "3", "name": "x", "version": "1"})"sv));
    EXPECT(fails(R"({"manifest_version": 3.5, "name": "x", "version": "1"})"sv));
    EXPECT(fails(R"({"manifest_version": -3, "name": "x", "version": "1"})"sv));
    EXPECT(fails(R"({"manifest_version": null, "name": "x", "version": "1"})"sv));
    EXPECT(!fails(R"({"manifest_version": 2, "name": "x", "version": "1"})"sv));
    EXPECT(!fails(R"({"manifest_version": 3.0, "name": "x", "version": "1"})"sv));
}

TEST_CASE(name_validation)
{
    EXPECT(fails(R"({"manifest_version": 3, "version": "1"})"sv));
    EXPECT(fails(R"({"manifest_version": 3, "name": 5, "version": "1"})"sv));
    EXPECT(fails(R"({"manifest_version": 3, "name": "", "version": "1"})"sv));
    EXPECT(fails(R"({"manifest_version": 3, "name": "   ", "version": "1"})"sv));
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "  padded  ", "version": "1"})"sv));
    EXPECT_EQ(manifest.name, "padded"sv);
}

TEST_CASE(version_validation)
{
    auto version_ok = [](StringView version) {
        auto json = ByteString::formatted(R"({{"manifest_version": 3, "name": "x", "version": "{}"}})", version);
        return !Manifest::parse(json).is_error();
    };
    EXPECT(version_ok("1"sv));
    EXPECT(version_ok("1.0"sv));
    EXPECT(version_ok("1.2.3.4"sv));
    EXPECT(version_ok("0.0.0.0"sv));
    EXPECT(version_ok("65535.65535.65535.65535"sv));
    EXPECT(!version_ok(""sv));
    EXPECT(!version_ok("1.2.3.4.5"sv));
    EXPECT(!version_ok("1..2"sv));
    EXPECT(!version_ok(".1"sv));
    EXPECT(!version_ok("1."sv));
    EXPECT(!version_ok("a"sv));
    EXPECT(!version_ok("1.0b"sv));
    EXPECT(!version_ok("-1"sv));
    EXPECT(!version_ok("+1"sv));
    EXPECT(!version_ok(" 1"sv));
    EXPECT(!version_ok("01"sv));
    // base::Version only rejects leading zeros in the first component.
    EXPECT(version_ok("1.01"sv));
    EXPECT(!version_ok("65536"sv));
    EXPECT(!version_ok("99999999999999999999"sv));
    EXPECT(fails(R"({"manifest_version": 3, "name": "x"})"sv));
    EXPECT(fails(R"({"manifest_version": 3, "name": "x", "version": 1})"sv));
}

TEST_CASE(wrong_types_for_known_keys_are_errors)
{
    auto with = [](StringView extra) {
        auto json = ByteString::formatted(R"({{"manifest_version": 3, "name": "x", "version": "1", {}}})", extra);
        return Manifest::parse(json).is_error();
    };
    EXPECT(with(R"("description": 5)"sv));
    EXPECT(with(R"("short_name": [])"sv));
    EXPECT(with(R"("version_name": {})"sv));
    EXPECT(with(R"("default_locale": 1)"sv));
    EXPECT(with(R"("key": 1)"sv));
    EXPECT(with(R"("key": "not a key!")"sv));
    EXPECT(with(R"("key": "")"sv));
    EXPECT(with(R"("minimum_chrome_version": 128)"sv));
    EXPECT(with(R"("minimum_chrome_version": "abc")"sv));
    EXPECT(with(R"("icons": [])"sv));
    EXPECT(with(R"("icons": {"16": 5})"sv));
    EXPECT(with(R"("icons": {"big": "a.png"})"sv));
    EXPECT(with(R"("icons": {"-16": "a.png"})"sv));
    EXPECT(with(R"("action": "popup.html")"sv));
    EXPECT(with(R"("action": {"default_popup": 1})"sv));
    EXPECT(with(R"("action": {"default_title": []})"sv));
    EXPECT(with(R"("action": {"default_icon": 1})"sv));
    EXPECT(with(R"("action": {"default_icon": {"x": "a.png"}})"sv));
    EXPECT(with(R"("background": "bg.js")"sv));
    EXPECT(with(R"("background": {"service_worker": 1})"sv));
    EXPECT(with(R"("background": {"service_worker": "a.js", "type": "esm"})"sv));
    EXPECT(with(R"("content_scripts": {})"sv));
    EXPECT(with(R"("content_scripts": [1])"sv));
    EXPECT(with(R"("permissions": "tabs")"sv));
    EXPECT(with(R"("permissions": [1])"sv));
    EXPECT(with(R"("optional_permissions": {})"sv));
    EXPECT(with(R"("host_permissions": "<all_urls>")"sv));
    EXPECT(with(R"("optional_host_permissions": [null])"sv));
    EXPECT(with(R"("web_accessible_resources": {})"sv));
    EXPECT(with(R"("web_accessible_resources": ["a.png"])"sv));
    EXPECT(with(R"("web_accessible_resources": [{"resources": "a.png", "matches": ["<all_urls>"]}])"sv));
    EXPECT(with(R"("web_accessible_resources": [{"matches": ["<all_urls>"]}])"sv));
    EXPECT(with(R"("web_accessible_resources": [{"resources": ["a.png"], "use_dynamic_url": "yes", "matches": ["<all_urls>"]}])"sv));
    EXPECT(with(R"("commands": [])"sv));
    EXPECT(with(R"("commands": {"a": 1})"sv));
    EXPECT(with(R"("commands": {"a": {"description": 1}})"sv));
    EXPECT(with(R"("commands": {"a": {"suggested_key": 1}})"sv));
    EXPECT(with(R"("commands": {"a": {"suggested_key": {"mac": 1}}})"sv));
    EXPECT(with(R"("content_security_policy": "script-src 'self'")"sv));
    EXPECT(with(R"("content_security_policy": {"extension_pages": 1})"sv));
    EXPECT(with(R"("content_security_policy": {"sandbox": []})"sv));
    EXPECT(with(R"("externally_connectable": [])"sv));
    EXPECT(with(R"("externally_connectable": {"matches": "x"})"sv));
    EXPECT(with(R"("externally_connectable": {"ids": [1]})"sv));
    EXPECT(with(R"("devtools_page": {})"sv));
    EXPECT(with(R"("homepage_url": 1)"sv));
    EXPECT(with(R"("homepage_url": "not a url")"sv));
    EXPECT(with(R"("incognito": "sometimes")"sv));
    EXPECT(with(R"("incognito": true)"sv));
    EXPECT(with(R"("declarative_net_request": [])"sv));
    EXPECT(with(R"("declarative_net_request": {"rule_resources": {}})"sv));
    EXPECT(with(R"("declarative_net_request": {"rule_resources": [{"id": "a", "enabled": true}]})"sv));
    EXPECT(with(R"("declarative_net_request": {"rule_resources": [{"id": "a", "path": "r.json"}]})"sv));
    EXPECT(with(R"("declarative_net_request": {"rule_resources": [{"id": "_a", "enabled": true, "path": "r.json"}]})"sv));
    EXPECT(with(R"("declarative_net_request": {"rule_resources": [{"id": "a", "enabled": true, "path": "r.json"}, {"id": "a", "enabled": false, "path": "s.json"}]})"sv));
    EXPECT(with(R"("storage": [])"sv));
    EXPECT(with(R"("storage": {"managed_schema": 1})"sv));
    EXPECT(with(R"("offline_enabled": "yes")"sv));
}

TEST_CASE(bad_options_keys_are_warnings)
{
    // Chrome reports bad options_page/options_ui as install warnings, not load errors.
    auto with = [](StringView extra) -> ErrorOr<Manifest> {
        auto json = ByteString::formatted(R"({{"manifest_version": 3, "name": "x", "version": "1", {}}})", extra);
        return Manifest::parse(json);
    };
    for (auto extra : { R"("options_page": 1)"sv, R"("options_ui": "options.html")"sv, R"("options_ui": {"open_in_tab": true})"sv, R"("options_ui": {"page": "o.html", "open_in_tab": "yes"})"sv }) {
        auto manifest = TRY_OR_FAIL(with(extra));
        EXPECT(!manifest.options_page.has_value());
        EXPECT(!manifest.options_ui.has_value());
        EXPECT(!manifest.warnings.is_empty());
    }
}

TEST_CASE(null_values_for_optional_keys)
{
    // Chrome treats a present-but-wrong-typed key as an error, including null.
    EXPECT(fails(R"({"manifest_version": 3, "name": "x", "version": "1", "description": null})"sv));
}

TEST_CASE(icons)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "icons": {"16": "i16.png", "32": "i32.png", "128": "i128.png"}})"sv));
    EXPECT_EQ(best_icon_for_size(manifest.icons, 16), "i16.png"sv);
    EXPECT_EQ(best_icon_for_size(manifest.icons, 20), "i32.png"sv);
    EXPECT_EQ(best_icon_for_size(manifest.icons, 64), "i128.png"sv);
    EXPECT_EQ(best_icon_for_size(manifest.icons, 512), "i128.png"sv);
    EXPECT(!best_icon_for_size({}, 16).has_value());
}

TEST_CASE(action_default_icon_forms)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "action": {"default_icon": "icon.png"}})"sv));
    EXPECT_EQ(manifest.action->default_icon.get(16), "icon.png"sv);
    EXPECT(!manifest.action->default_popup.has_value());

    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "action": {"default_icon": {"16": "a.png", "32": "b.png"}}})"sv));
    EXPECT_EQ(manifest.action->default_icon.size(), 2u);
    EXPECT_EQ(manifest.action->default_icon.get(32), "b.png"sv);

    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1", "action": {}})"sv));
    EXPECT(manifest.action.has_value());
}

TEST_CASE(mv2_browser_action_and_page_action)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 2, "name": "x", "version": "1",
        "browser_action": {"default_popup": "p.html", "default_title": "T"}})"sv));
    EXPECT_EQ(manifest.manifest_version, 2);
    EXPECT_EQ(manifest.action->default_popup, "p.html"sv);
    EXPECT_EQ(manifest.action->default_title, "T"sv);

    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 2, "name": "x", "version": "1",
        "page_action": {"default_title": "P"}})"sv));
    EXPECT_EQ(manifest.action->default_title, "P"sv);

    // In MV3 the old keys are ignored with a warning, as Chrome does.
    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "browser_action": {"default_popup": "p.html"}})"sv));
    EXPECT(!manifest.action.has_value());
    EXPECT(!manifest.warnings.is_empty());

    // MV2 "action" is ignored with a warning.
    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 2, "name": "x", "version": "1",
        "action": {"default_popup": "p.html"}})"sv));
    EXPECT(!manifest.action.has_value());
    EXPECT(!manifest.warnings.is_empty());
}

TEST_CASE(mv2_background)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 2, "name": "x", "version": "1",
        "background": {"scripts": ["a.js", "b.js"], "persistent": false}})"sv));
    EXPECT_EQ(manifest.background->scripts, (Vector<String> { "a.js"_string, "b.js"_string }));
    EXPECT_EQ(manifest.background->persistent, false);
    EXPECT(!manifest.background->service_worker.has_value());

    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 2, "name": "x", "version": "1",
        "background": {"page": "bg.html"}})"sv));
    EXPECT_EQ(manifest.background->page, "bg.html"sv);

    EXPECT(fails(R"({"manifest_version": 2, "name": "x", "version": "1", "background": {"scripts": "a.js"}})"sv));
    EXPECT(fails(R"({"manifest_version": 2, "name": "x", "version": "1", "background": {"scripts": ["a.js"], "page": "bg.html"}})"sv));
    EXPECT(fails(R"({"manifest_version": 2, "name": "x", "version": "1", "background": {"persistent": "no"}})"sv));
}

TEST_CASE(mv3_background)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "background": {"service_worker": "sw.js"}})"sv));
    EXPECT_EQ(manifest.background->type, BackgroundInfo::Type::Classic);

    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "background": {"service_worker": "sw.js", "type": "classic"}})"sv));
    EXPECT_EQ(manifest.background->type, BackgroundInfo::Type::Classic);

    // Cross-browser manifests list "scripts" next to "service_worker"; Chrome warns and uses the worker.
    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "background": {"service_worker": "sw.js", "scripts": ["bg.js"]}})"sv));
    EXPECT_EQ(manifest.background->service_worker, "sw.js"sv);
    EXPECT(manifest.background->scripts.is_empty());
    EXPECT(!manifest.warnings.is_empty());

    EXPECT(fails(R"({"manifest_version": 3, "name": "x", "version": "1", "background": {"service_worker": ""}})"sv));
}

TEST_CASE(content_script_details)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "content_scripts": [{
            "matches": ["https://*.example.com/*"],
            "exclude_matches": ["https://bad.example.com/*"],
            "include_globs": ["*a*"],
            "exclude_globs": ["*b*"],
            "css": ["c.css"],
            "match_about_blank": true,
            "match_origin_as_fallback": true,
            "world": "ISOLATED"
        }]})"sv));
    auto const& script = manifest.content_scripts[0];
    EXPECT_EQ(script.exclude_matches, (Vector<String> { "https://bad.example.com/*"_string }));
    EXPECT_EQ(script.include_globs, (Vector<String> { "*a*"_string }));
    EXPECT_EQ(script.exclude_globs, (Vector<String> { "*b*"_string }));
    EXPECT(script.js.is_empty());
    EXPECT(script.match_about_blank);
    EXPECT(script.match_origin_as_fallback);
    EXPECT_EQ(script.world, ContentScript::World::Isolated);
    EXPECT_EQ(script.run_at, ContentScript::RunAt::DocumentIdle);
}

TEST_CASE(content_script_errors)
{
    auto script = [](StringView body) {
        auto json = ByteString::formatted(R"({{"manifest_version": 3, "name": "x", "version": "1", "content_scripts": [{}]}})", body);
        return Manifest::parse(json).is_error();
    };
    EXPECT(!script(R"({"matches": ["<all_urls>"], "js": ["a.js"]})"sv));
    EXPECT(script(R"({"js": ["a.js"]})"sv));
    EXPECT(script(R"({"matches": [], "js": ["a.js"]})"sv));
    EXPECT(script(R"({"matches": "<all_urls>", "js": ["a.js"]})"sv));
    EXPECT(script(R"({"matches": ["<all_urls>"]})"sv));
    EXPECT(script(R"({"matches": ["<all_urls>"], "js": []})"sv));
    EXPECT(script(R"({"matches": ["<all_urls>"], "js": "a.js"})"sv));
    EXPECT(script(R"({"matches": ["<all_urls>"], "js": [1]})"sv));
    EXPECT(script(R"({"matches": ["<all_urls>"], "js": ["a.js"], "run_at": "document_later"})"sv));
    EXPECT(script(R"({"matches": ["<all_urls>"], "js": ["a.js"], "run_at": 1})"sv));
    EXPECT(script(R"({"matches": ["<all_urls>"], "js": ["a.js"], "all_frames": "true"})"sv));
    EXPECT(script(R"({"matches": ["<all_urls>"], "js": ["a.js"], "world": "main"})"sv));
    EXPECT(script(R"({"matches": ["<all_urls>"], "js": ["a.js"], "exclude_globs": "x"})"sv));
}

TEST_CASE(mv2_host_permissions_live_in_permissions)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 2, "name": "x", "version": "1",
        "permissions": ["tabs", "<all_urls>", "https://*.example.com/*", "storage"],
        "optional_permissions": ["http://a.com/*", "cookies"]})"sv));
    EXPECT_EQ(manifest.permissions, (Vector<String> { "tabs"_string, "storage"_string }));
    EXPECT_EQ(manifest.host_permissions, (Vector<String> { "<all_urls>"_string, "https://*.example.com/*"_string }));
    EXPECT_EQ(manifest.optional_permissions, (Vector<String> { "cookies"_string }));
    EXPECT_EQ(manifest.optional_host_permissions, (Vector<String> { "http://a.com/*"_string }));
    EXPECT(manifest.has_optional_permission("cookies"sv));
}

TEST_CASE(mv3_host_patterns_in_permissions_are_ignored)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "permissions": ["tabs", "https://*/*"], "optional_host_permissions": ["https://a.com/*"]})"sv));
    EXPECT_EQ(manifest.permissions, (Vector<String> { "tabs"_string }));
    EXPECT(manifest.host_permissions.is_empty());
    EXPECT_EQ(manifest.optional_host_permissions, (Vector<String> { "https://a.com/*"_string }));
    EXPECT(!manifest.warnings.is_empty());
}

TEST_CASE(duplicate_permissions_are_collapsed)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "permissions": ["tabs", "tabs", "storage"]})"sv));
    EXPECT_EQ(manifest.permissions, (Vector<String> { "tabs"_string, "storage"_string }));
}

TEST_CASE(web_accessible_resources)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "web_accessible_resources": [
            {"resources": ["a.png"], "matches": ["https://a.com/*"], "use_dynamic_url": true},
            {"resources": ["b.png"], "extension_ids": ["aeblfdkhhhdcdjpifhhbdiojplfjncoa"]}
        ]})"sv));
    EXPECT_EQ(manifest.web_accessible_resources.size(), 2u);
    EXPECT(manifest.web_accessible_resources[0].use_dynamic_url);
    EXPECT_EQ(manifest.web_accessible_resources[1].extension_ids, (Vector<String> { "aeblfdkhhhdcdjpifhhbdiojplfjncoa"_string }));

    // Neither matches nor extension_ids: Chrome refuses to load.
    EXPECT(fails(R"({"manifest_version": 3, "name": "x", "version": "1", "web_accessible_resources": [{"resources": ["a.png"]}]})"sv));

    // MV2 uses a flat list, available to all sites.
    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 2, "name": "x", "version": "1",
        "web_accessible_resources": ["a.png", "b/*"]})"sv));
    EXPECT_EQ(manifest.web_accessible_resources.size(), 1u);
    EXPECT_EQ(manifest.web_accessible_resources[0].resources, (Vector<String> { "a.png"_string, "b/*"_string }));
    EXPECT_EQ(manifest.web_accessible_resources[0].matches, (Vector<String> { "<all_urls>"_string }));
    EXPECT(fails(R"({"manifest_version": 2, "name": "x", "version": "1", "web_accessible_resources": [{"resources": ["a.png"]}]})"sv));
}

TEST_CASE(commands)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "commands": {
            "toggle": {"description": "Toggle", "suggested_key": {"windows": "Ctrl+Y", "linux": "Alt+Y", "chromeos": "Alt+Shift+Y"}, "global": true},
            "_execute_action": {}
        }})"sv));
    EXPECT_EQ(manifest.commands.size(), 2u);
    auto toggle = manifest.command("toggle"sv);
    EXPECT(toggle.has_value());
    EXPECT(toggle->global);
    EXPECT(!toggle->suggested_key.default_key.has_value());
    EXPECT_EQ(toggle->suggested_key.for_platform("windows"sv), "Ctrl+Y"sv);
    EXPECT_EQ(toggle->suggested_key.for_platform("linux"sv), "Alt+Y"sv);
    EXPECT_EQ(toggle->suggested_key.chromeos, "Alt+Shift+Y"sv);
    EXPECT(!toggle->suggested_key.for_platform("mac"sv).has_value());
    EXPECT(manifest.command("_execute_action"sv).has_value());
    EXPECT(!manifest.command("nope"sv).has_value());

    // A suggested_key may be a plain string meaning "default".
    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "commands": {"a": {"description": "A", "suggested_key": "Ctrl+Shift+A"}}})"sv));
    EXPECT_EQ(manifest.command("a"sv)->suggested_key.default_key, "Ctrl+Shift+A"sv);
}

TEST_CASE(mv2_csp_string)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 2, "name": "x", "version": "1",
        "content_security_policy": "script-src 'self' 'unsafe-eval'; object-src 'self'"})"sv));
    EXPECT_EQ(manifest.extension_pages_csp(), "script-src 'self' 'unsafe-eval'; object-src 'self'"sv);

    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 2, "name": "x", "version": "1"})"sv));
    EXPECT_EQ(manifest.extension_pages_csp(), Manifest::default_mv2_extension_pages_csp());

    EXPECT(fails(R"({"manifest_version": 2, "name": "x", "version": "1", "content_security_policy": {"extension_pages": "x"}})"sv));
}

TEST_CASE(mv3_csp_sandbox)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "content_security_policy": {"sandbox": "sandbox allow-scripts; script-src 'self' https://cdn.example.com"}})"sv));
    EXPECT(!manifest.content_security_policy.extension_pages.has_value());
    EXPECT_EQ(manifest.extension_pages_csp(), Manifest::default_mv3_extension_pages_csp());
    EXPECT_EQ(manifest.sandbox_csp(), "sandbox allow-scripts; script-src 'self' https://cdn.example.com"sv);
}

TEST_CASE(misc_keys)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "version_name": "1.0 beta",
        "externally_connectable": {"matches": ["https://*.example.com/*"], "ids": ["*"], "accepts_tls_channel_id": true},
        "options_page": "options.html",
        "options_ui": {"page": "opts.html", "open_in_tab": true},
        "devtools_page": "devtools.html",
        "incognito": "split",
        "storage": {"managed_schema": "schema.json"},
        "offline_enabled": true})"sv));
    EXPECT_EQ(manifest.version_name, "1.0 beta"sv);
    EXPECT_EQ(manifest.externally_connectable->matches, (Vector<String> { "https://*.example.com/*"_string }));
    EXPECT_EQ(manifest.externally_connectable->ids, (Vector<String> { "*"_string }));
    EXPECT(manifest.externally_connectable->accepts_tls_channel_id);
    EXPECT_EQ(manifest.options_page, "options.html"sv);
    EXPECT_EQ(manifest.options_ui->page, "opts.html"sv);
    EXPECT(manifest.options_ui->open_in_tab);
    EXPECT_EQ(manifest.devtools_page, "devtools.html"sv);
    EXPECT_EQ(manifest.incognito, IncognitoMode::Split);
    EXPECT_EQ(manifest.storage->managed_schema, "schema.json"sv);
    EXPECT_EQ(manifest.offline_enabled, true);

    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1", "incognito": "not_allowed"})"sv));
    EXPECT_EQ(manifest.incognito, IncognitoMode::NotAllowed);
}

TEST_CASE(unknown_keys_are_ignored)
{
    auto manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "x", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "a@b"}}, "update_url": 5, "author": {"email": "a@b.c"}})"sv));
    EXPECT(manifest.raw.has("browser_specific_settings"sv));
    // "author" may be an object in Firefox manifests; only a string is kept.
    EXPECT(!manifest.author.has_value());
}

TEST_CASE(comments_and_bom)
{
    auto manifest = TRY_OR_FAIL(parse("\xEF\xBB\xBF{\n// comment\n\"manifest_version\": 3, /* block\n comment */ \"name\": \"a // not a comment /* nope */\", \"version\": \"1\"}"sv));
    EXPECT_EQ(manifest.name, "a // not a comment /* nope */"sv);

    manifest = TRY_OR_FAIL(parse(R"({"manifest_version": 3, "name": "quote \" // still string", "version": "1"} // trailing)"sv));
    EXPECT_EQ(manifest.name, "quote \" // still string"sv);

    EXPECT(fails(R"({"manifest_version": 3, "name": "x", "version": "1"} /* unterminated)"sv));
}

TEST_CASE(strip_json_comments)
{
    EXPECT_EQ(TRY_OR_FAIL(WebExtensions::strip_json_comments("{} // x"sv)), "{} "sv);
    EXPECT_EQ(TRY_OR_FAIL(WebExtensions::strip_json_comments("[1,/**/2]"sv)), "[1, 2]"sv);
    EXPECT_EQ(TRY_OR_FAIL(WebExtensions::strip_json_comments(R"("a\\"//x)"sv)), R"("a\\")"sv);
    EXPECT(WebExtensions::strip_json_comments("/* x"sv).is_error());
}

TEST_CASE(deeply_nested_json_does_not_crash)
{
    StringBuilder builder;
    builder.append(R"({"manifest_version": 3, "name": "x", "version": "1", "junk": )"sv);
    for (int i = 0; i < 100000; ++i)
        builder.append('[');
    builder.append('}');
    EXPECT(fails(builder.string_view()));
}
