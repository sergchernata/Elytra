const self_id = "epcdjeeimbldakmnfpideclgjeiflopp";
const other_id = "njaomanglkphpmpbpgpbeolgfhgafipk";
const permissions_id = "cdgfcddbilmookcknhbepmneijognhgb";

steps.management = async () => {
    await attempt("self", async () => JSON.stringify(await chrome.management.getSelf()));
    await attempt("all", async () => (await chrome.management.getAll()).map(info => info.id).sort().join(","));
    await attempt("other", async () => {
        const info = await chrome.management.get(other_id);
        return `${info.name} ${info.version} ${info.enabled} ${info.installType}`;
    });
    await attempt("unknown", () => chrome.management.get("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    await attempt("bad_id", () => chrome.management.get(5));
    await attempt("disable_self", () => chrome.management.setEnabled(self_id, false));
    await attempt("bad_set_id", () => chrome.management.setEnabled(42, false));
    await attempt("bad_set_enabled", () => chrome.management.setEnabled(other_id, "no"));
    await attempt("set_unknown", () => chrome.management.setEnabled("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", false));
    await attempt("disable", () => chrome.management.setEnabled(other_id, false));
    await attempt("other_disabled", async () => {
        const info = await chrome.management.get(other_id);
        return `${info.enabled} ${info.disabledReason}`;
    });
    await attempt("callback", () => new Promise(resolve => chrome.management.getSelf(info => resolve(info.id))));
};

steps.enable = async () => {
    await attempt("enable", () => chrome.management.setEnabled(other_id, true));
    await attempt("other_enabled", async () => (await chrome.management.get(other_id)).enabled);
    await attempt("events", async () => JSON.stringify(await events(2, named("management.onEnabled", "management.onDisabled"))));
};

steps.privacy = async () => {
    await attempt("initial", privacyState);
    await attempt("set", () => privacy.set({ value: false }));
    await attempt("after_set", privacyState);
    await attempt("wrong_type", () => privacy.set({ value: "no" }));
    await attempt("no_value", () => privacy.set({}));
    await attempt("no_details", () => privacy.set());
    await attempt("incognito_scope", () => privacy.set({ value: false, scope: "incognito_persistent" }));
    await attempt("bad_scope", () => privacy.set({ value: false, scope: "everywhere" }));
    await attempt("incognito_get", () => privacy.get({ incognito: true }));
    await attempt("webrtc_bad", () => chrome.privacy.network.webRTCIPHandlingPolicy.set({ value: "bogus" }));
    await attempt("webrtc", async () => {
        await chrome.privacy.network.webRTCIPHandlingPolicy.set({ value: "disable_non_proxied_udp" });
        return JSON.stringify(await chrome.privacy.network.webRTCIPHandlingPolicy.get({}));
    });
    await attempt("defaults", async () => {
        const names = ["alternateErrorPagesEnabled", "autofillAddressEnabled", "autofillCreditCardEnabled", "autofillEnabled", "safeBrowsingEnabled", "spellingServiceEnabled", "translationServiceEnabled"];
        const values = [];
        for (const name of names)
            values.push(`${name}=${(await chrome.privacy.services[name].get({})).value}`);
        for (const name of ["thirdPartyCookiesAllowed", "hyperlinkAuditingEnabled", "referrersEnabled", "doNotTrackEnabled", "protectedContentEnabled"])
            values.push(`${name}=${(await chrome.privacy.websites[name].get({})).value}`);
        values.push(`networkPredictionEnabled=${(await chrome.privacy.network.networkPredictionEnabled.get({})).value}`);
        return values.join(" ");
    });
    await attempt("suggest_off", async () => {
        await chrome.privacy.services.searchSuggestEnabled.set({ value: true });
        return JSON.stringify(await chrome.privacy.services.searchSuggestEnabled.get({}));
    });
    await attempt("on_change", async () => JSON.stringify(await events(1, named("privacy.onChange"))));
};

steps.privacy_after_other = async () => {
    await attempt("state", privacyState);
    await attempt("suggest_clear", async () => {
        await chrome.privacy.services.searchSuggestEnabled.clear({});
        return JSON.stringify(await chrome.privacy.services.searchSuggestEnabled.get({}));
    });
};

steps.runtime = async () => {
    await attempt("contexts", async () => (await chrome.runtime.getContexts({})).map(context => `${context.contextType} ${context.tabId === -1 ? "-1" : "tab"} ${context.frameId} ${context.documentUrl || ""} ${context.incognito}`).sort().join("|"));
    await attempt("background_contexts", async () => {
        const contexts = await chrome.runtime.getContexts({ contextTypes: ["BACKGROUND"] });
        return `${contexts.length} ${typeof contexts[0].contextId}`;
    });
    await attempt("tab_by_url", async () => (await chrome.runtime.getContexts({ documentUrls: [location.href] })).length);
    await attempt("bad_filter", () => chrome.runtime.getContexts("x"));
    await attempt("bad_filter_types", () => chrome.runtime.getContexts({ contextTypes: "TAB" }));
    await attempt("options", () => chrome.runtime.openOptionsPage());
    await attempt("uninstall_url", () => chrome.runtime.setUninstallURL("https://example.com/bye"));
    await attempt("uninstall_url_javascript", () => chrome.runtime.setUninstallURL("javascript:alert(1)"));
    await attempt("uninstall_url_long", () => chrome.runtime.setUninstallURL(`https://example.com/${"a".repeat(1100)}`));
    await attempt("uninstall_url_type", () => chrome.runtime.setUninstallURL(5));
    await attempt("update_check", async () => JSON.stringify(await chrome.runtime.requestUpdateCheck()));
    await attempt("restart", () => chrome.runtime.restart());
    await attempt("incognito_access", () => chrome.extension.isAllowedIncognitoAccess());
    await attempt("file_access", () => chrome.extension.isAllowedFileSchemeAccess());
    await attempt("managed", async () => JSON.stringify(await chrome.storage.managed.get(null)));
    await attempt("managed_keys", async () => JSON.stringify(await chrome.storage.managed.getKeys()));
    await attempt("managed_bytes", () => chrome.storage.managed.getBytesInUse(null));
};

steps.reload = async () => {
    if (!localStorage.getItem("reloaded")) {
        localStorage.setItem("reloaded", "1");
        report.starts_before = (await chrome.storage.local.get("starts")).starts;
        chrome.runtime.reload();
        await new Promise(() => {});
    }
    localStorage.removeItem("reloaded");
    for (let tries = 0; tries < 100; ++tries) {
        if ((await chrome.storage.local.get("starts")).starts >= 2)
            break;
        await sleep(50);
    }
    report.starts = (await chrome.storage.local.get("starts")).starts;
    report.reloaded = true;
};

steps.uninstall = async () => {
    await attempt("bad_options", () => chrome.management.uninstallSelf("x"));
    await attempt("cancelled", () => chrome.management.uninstallSelf({ showConfirmDialog: true }));
    await attempt("uninstall", () => chrome.management.uninstallSelf({ showConfirmDialog: true }));
};

steps.uninstall_without_hook = async () => {
    await attempt("uninstall", () => chrome.management.uninstallSelf({ showConfirmDialog: true }));
};
