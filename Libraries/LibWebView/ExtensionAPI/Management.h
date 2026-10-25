/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionRuntime.h>

namespace WebView {

class Extension;

// chrome.permissions, chrome.management, chrome.privacy, chrome.extension, chrome.storage.managed and the rest of
// chrome.runtime. What extensions are granted, whether they're enabled, and what they set for the browser is kept
// in <storage directory>/<id>/management.json.
class WEBVIEW_API ManagementAPI final : public ExtensionAPI {
public:
    explicit ManagementAPI(ExtensionRuntime&);
    virtual ~ManagementAPI() override;

    // The extension's API permissions: those in its manifest and the optional ones the user granted.
    bool has_permission(StringView extension_id, StringView permission) const;
    // Whether the extension's host permissions (with those granted, and its content scripts' matches) cover the URL.
    bool has_host_permission(StringView extension_id, URL::URL const&) const;
    // Those host permissions, as match patterns.
    Vector<String> host_permission_patterns(StringView extension_id) const;

    // Enabled: loaded by the registry (a disabled extension stays installed, unloaded).
    bool is_enabled(StringView extension_id) const;

    // runtime.setUninstallURL's: what to open once the extension is uninstalled.
    Optional<URL::URL> uninstall_url(StringView extension_id) const;

    // A chrome.privacy setting as the browser should use it ("services.passwordSavingEnabled"): the value of the
    // extension controlling it, or the default. Null for a name that isn't a setting.
    JsonValue privacy_setting(StringView name) const;
    // Opens the options page (without ExtensionRuntime::open_options_page) and the uninstall URL; without this, in a
    // new tab of the application.
    Function<void(URL::URL const&)> open_url_in_new_tab;

    // A setting's value changed, because an extension set or cleared it.
    Function<void(StringView name)> on_privacy_setting_changed;

    virtual void extensions_changed() override;

private:
    struct PrivacyValue {
        JsonValue value;
        u64 sequence { 0 };
    };
    struct State {
        AK_ALLOC_WITH_KMALLOC;

        Vector<String> granted_permissions;
        Vector<String> granted_origins;
        Optional<URL::URL> uninstall_url;
        HashMap<String, PrivacyValue> privacy;
    };

    State& state(StringView extension_id) const;
    void save_state(StringView extension_id) const;

    void register_permissions_functions();
    void register_management_functions();
    void register_privacy_functions();
    void register_runtime_functions();

    Vector<String> api_permissions(Extension const&) const;
    Vector<String> host_permissions(Extension const&) const;
    JsonObject extension_info(Extension const&) const;
    JsonObject disabled_extension_info(StringView id) const;
    Optional<JsonObject> info_of(StringView id) const;
    void open_url(URL::URL const&);
    void uninstall(String id, bool show_confirm_dialog, ExtensionRuntime::Reply, Optional<String> requester_id);
    void dispatch_to_managers(StringView event, JsonValue const& argument, Optional<StringView> skip = {});

    // The extension controlling a privacy setting, if any: the last enabled one to set it.
    Optional<String> privacy_controller(StringView name) const;
    void privacy_setting_changed(StringView name, JsonValue const& previous_value, Optional<String> const& previous_controller);
    JsonObject privacy_details(StringView name, Extension const& viewer) const;
    void apply_privacy_setting_to_browser(StringView name);

    virtual void extension_uninstalled(StringView extension_id) override { m_states.remove(extension_id); }

    mutable HashMap<String, NonnullOwnPtr<State>> m_states;
    mutable u64 m_next_privacy_sequence { 1 };
    // Versions of the extensions loaded, for management.onInstalled, and what's known (loaded or installed), for
    // onUninstalled.
    HashMap<String, String> m_loaded_versions;
    HashTable<String> m_known_extensions;
    // The user's own search suggestions setting, while an extension controls it.
    Optional<bool> m_user_search_suggestions;

    // Questions to the user: one at a time per extension, and what they just refused isn't asked again at once.
    struct Denial {
        Vector<String> permissions;
        Vector<String> origins;
        MonotonicTime until;
    };
    HashTable<String> m_asking_for_permissions;
    HashMap<String, Denial> m_denials;
    HashTable<String> m_asking_to_uninstall;
};

}
