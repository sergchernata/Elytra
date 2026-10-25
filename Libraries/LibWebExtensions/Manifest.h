/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>

namespace WebExtensions {

// Icon size in pixels -> extension-relative path.
using IconMap = HashMap<u32, String>;

// Picks the smallest icon at least `size` pixels, else the largest one.
Optional<String> best_icon_for_size(IconMap const&, u32 size);

struct ActionInfo {
    Optional<String> default_popup;
    Optional<String> default_title;
    // A plain string "default_icon" is stored at size 16, like Chrome does.
    IconMap default_icon;
};

struct BackgroundInfo {
    enum class Type : u8 {
        Classic,
        Module,
    };

    Optional<String> service_worker;
    Type type { Type::Classic };
    Vector<String> scripts; // MV2
    Optional<String> page;  // MV2
    Optional<bool> persistent;
};

struct ContentScript {
    enum class RunAt : u8 {
        DocumentStart,
        DocumentEnd,
        DocumentIdle,
    };

    enum class World : u8 {
        Isolated,
        Main,
    };

    Vector<String> matches;
    Vector<String> exclude_matches;
    Vector<String> include_globs;
    Vector<String> exclude_globs;
    Vector<String> js;
    Vector<String> css;
    RunAt run_at { RunAt::DocumentIdle };
    bool all_frames { false };
    bool match_about_blank { false };
    bool match_origin_as_fallback { false };
    World world { World::Isolated };
};

struct WebAccessibleResource {
    Vector<String> resources;
    Vector<String> matches;
    Vector<String> extension_ids;
    bool use_dynamic_url { false };
};

struct SuggestedKey {
    Optional<String> default_key;
    Optional<String> mac;
    Optional<String> windows;
    Optional<String> linux;
    Optional<String> chromeos;

    // The binding for a platform ("mac", "windows", "linux", "chromeos"), falling back to "default".
    Optional<String> for_platform(StringView platform) const;
};

struct Command {
    String name;
    Optional<String> description;
    SuggestedKey suggested_key;
    bool global { false };
};

struct ContentSecurityPolicy {
    Optional<String> extension_pages;
    Optional<String> sandbox;
};

struct ExternallyConnectable {
    Vector<String> matches;
    Vector<String> ids;
    bool accepts_tls_channel_id { false };
};

struct OptionsUI {
    String page;
    bool open_in_tab { false };
};

struct RuleResource {
    String id;
    bool enabled { false };
    String path;
};

struct DeclarativeNetRequest {
    Vector<RuleResource> rule_resources;
};

struct StorageInfo {
    Optional<String> managed_schema;
};

enum class IncognitoMode : u8 {
    Spanning,
    Split,
    NotAllowed,
};

struct Manifest {
    static ErrorOr<Manifest> parse(StringView json);

    // Chrome's defaults when no CSP is declared (csp_info.cc).
    static StringView default_mv3_extension_pages_csp();
    static StringView default_mv2_extension_pages_csp();
    static StringView default_sandbox_csp();
    // Chrome enforces this in addition to the declared MV3 extension_pages CSP.
    static StringView minimum_mv3_extension_pages_csp();

    // The declared CSP for extension pages, or the default for this manifest version.
    String extension_pages_csp() const;
    String sandbox_csp() const;

    bool has_permission(StringView) const;
    bool has_optional_permission(StringView) const;
    Optional<Command const&> command(StringView name) const;

    u8 manifest_version { 3 };
    String name;
    Optional<String> short_name;
    String version;
    Optional<String> version_name;
    Optional<String> description;
    Optional<String> default_locale;
    Optional<String> key;
    Optional<String> minimum_chrome_version;
    Optional<String> author;
    IconMap icons;

    // MV3 "action", or MV2 "browser_action"/"page_action".
    Optional<ActionInfo> action;
    Optional<BackgroundInfo> background;
    Vector<ContentScript> content_scripts;

    // API permissions. In MV2, host patterns found in "permissions" are moved to host_permissions.
    Vector<String> permissions;
    Vector<String> optional_permissions;
    Vector<String> host_permissions;
    Vector<String> optional_host_permissions;

    Vector<WebAccessibleResource> web_accessible_resources;
    Vector<Command> commands;
    ContentSecurityPolicy content_security_policy;
    Optional<ExternallyConnectable> externally_connectable;

    Optional<String> options_page;
    Optional<OptionsUI> options_ui;
    Optional<String> devtools_page;
    Optional<String> homepage_url;
    IncognitoMode incognito { IncognitoMode::Spanning };
    Optional<DeclarativeNetRequest> declarative_net_request;
    Optional<StorageInfo> storage;
    Optional<bool> offline_enabled;

    // Non-fatal problems, like Chrome's manifest warnings.
    Vector<String> warnings;

    // The parsed manifest, for keys this struct doesn't model.
    JsonObject raw;
};

// Strips "//" and "/* */" comments (outside strings) and a leading UTF-8 BOM,
// as Chrome's JSON reader allows them in manifest and message files.
ErrorOr<String> strip_json_comments(StringView);

}
