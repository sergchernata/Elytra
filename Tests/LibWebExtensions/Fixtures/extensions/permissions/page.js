const has = (permissions) => chrome.permissions.contains(permissions);

steps.permissions = async () => {
    await attempt("all", async () => JSON.stringify(await chrome.permissions.getAll()));
    await attempt("contains_required", () => has({ permissions: ["storage"] }));
    await attempt("contains_optional", () => has({ permissions: ["notifications"] }));
    await attempt("contains_origin", () => has({ origins: ["https://granted.example/*"] }));
    await attempt("contains_origin_path", () => has({ origins: ["https://granted.example/some/path"] }));
    await attempt("contains_scripted", () => has({ origins: ["https://scripted.example/*"] }));
    await attempt("contains_other_origin", () => has({ origins: ["https://other.example/*"] }));
    await attempt("contains_all_urls", () => has({ origins: ["<all_urls>"] }));
    await attempt("contains_bad_origin", () => has({ origins: ["not a pattern"] }));
    await attempt("contains_bad_type", () => has("storage"));
    await attempt("contains_bad_list", () => has({ permissions: [1] }));
    await attempt("contains_nothing", () => has());

    await attempt("request", () => chrome.permissions.request({ permissions: ["notifications"] }));
    await attempt("contains_granted", () => has({ permissions: ["notifications"] }));
    await attempt("request_again", () => chrome.permissions.request({ permissions: ["notifications", "storage"] }));
    await attempt("request_undeclared", () => chrome.permissions.request({ permissions: ["tabs"] }));
    await attempt("request_origin", () => chrome.permissions.request({ origins: ["https://www.optional.example/*"] }));
    await attempt("contains_granted_origin", () => has({ origins: ["https://www.optional.example/page"] }));
    await attempt("contains_wider_origin", () => has({ origins: ["https://*.optional.example/*"] }));
    await attempt("request_denied", () => chrome.permissions.request({ origins: ["https://deny.optional.example/*"] }));
    await attempt("contains_denied", () => has({ origins: ["https://deny.optional.example/*"] }));
    await attempt("request_undeclared_origin", () => chrome.permissions.request({ origins: ["https://nope.example/*"] }));
    await attempt("request_bad", () => chrome.permissions.request({ origins: "https://www.optional.example/*" }));
    await attempt("all_granted", async () => JSON.stringify(await chrome.permissions.getAll()));

    await attempt("remove_required", () => chrome.permissions.remove({ permissions: ["storage"] }));
    await attempt("remove_required_origin", () => chrome.permissions.remove({ origins: ["https://granted.example/*"] }));
    await attempt("remove", () => chrome.permissions.remove({ permissions: ["notifications"] }));
    await attempt("contains_removed", () => has({ permissions: ["notifications"] }));
    await attempt("request_idle", () => chrome.permissions.request({ permissions: ["idle"] }));

    await attempt("host_access_request", () => chrome.permissions.addHostAccessRequest({ tabId: 1 }));
    await attempt("host_access_request_pattern", () => chrome.permissions.addHostAccessRequest({ tabId: 1, pattern: "https://www.optional.example/*" }));
    await attempt("host_access_request_empty", () => chrome.permissions.addHostAccessRequest({}));
    await attempt("host_access_request_both", () => chrome.permissions.addHostAccessRequest({ tabId: 1, documentId: "ABC" }));
    await attempt("host_access_request_bad_tab", () => chrome.permissions.addHostAccessRequest({ tabId: "1" }));
    await attempt("host_access_request_bad_pattern", () => chrome.permissions.removeHostAccessRequest({ tabId: 1, pattern: "bad" }));
    await attempt("remove_host_access_request", () => chrome.permissions.removeHostAccessRequest({ tabId: 1 }));

    await attempt("events", async () => JSON.stringify(await events(4, named("permissions.onAdded", "permissions.onRemoved"))));
};

steps.deny = async () => {
    await attempt("request", () => chrome.permissions.request({ permissions: ["notifications"] }));
    await attempt("still_granted", () => has({ permissions: ["idle"], origins: ["https://www.optional.example/*"] }));
};

// permissions.request needs the user's gesture; while the user is asked, and just after they said no, it doesn't ask again.
steps.gesture = async () => {
    await attempt("request", () => chrome.permissions.request({ permissions: ["notifications"] }));
};

steps.prompts = async () => {
    const first = chrome.permissions.request({ permissions: ["notifications"] }).then(String, e => `error: ${e.message}`);
    await attempt("while_asking", () => chrome.permissions.request({ origins: ["https://w.optional.example/*"] }));
    report.first = await first;
    await attempt("after_denial", () => chrome.permissions.request({ permissions: ["notifications", "idle"] }));
    await attempt("other_after_denial", () => chrome.permissions.request({ origins: ["https://x.optional.example/*"] }));
};

steps.privacy = async () => {
    await attempt("get", privacyState);
    await attempt("set", () => privacy.set({ value: false }));
    await attempt("clear", () => privacy.clear({}));
};

steps.management = async () => {
    await attempt("self", async () => {
        const info = await chrome.management.getSelf();
        return `${info.shortName} ${info.optionsUrl === ""} ${info.homepageUrl === undefined} ${JSON.stringify(info.icons)} ${info.offlineEnabled} ${JSON.stringify(info.permissions)} ${JSON.stringify(info.hostPermissions)}`;
    });
    await attempt("all", async () => (await chrome.management.getAll()).map(info => info.id).join(","));
    await attempt("uninstall_other", () => chrome.management.uninstall("epcdjeeimbldakmnfpideclgjeiflopp"));
};
