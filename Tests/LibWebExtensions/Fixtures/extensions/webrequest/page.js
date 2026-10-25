// Runs the test named in the query against the test's server (?server=http://127.0.0.1:port&alt=http://localhost:port)
// and reports what it saw as JSON in the title.
const params = new URLSearchParams(location.search);
const server = params.get("server");
const alt = params.get("alt");
const report = {};
const attempt = async (name, fn) => {
    try {
        report[name] = await fn();
    } catch (e) {
        report[name] = `error: ${e.message}`;
    }
};
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const waitFor = async condition => {
    for (let tries = 0; tries < 300 && !(await condition()); ++tries)
        await sleep(20);
};
const wr = chrome.webRequest;
const dnr = chrome.declarativeNetRequest;

// Every event, as "event:url" with its details.
function record(filter = { urls: ["http://127.0.0.1/*", "http://localhost/*"] }) {
    const events = [];
    const names = ["onBeforeRequest", "onBeforeSendHeaders", "onSendHeaders", "onHeadersReceived", "onResponseStarted", "onBeforeRedirect", "onCompleted", "onErrorOccurred"];
    for (const name of names) {
        const extra = name === "onHeadersReceived" || name === "onResponseStarted" || name === "onCompleted" ? ["responseHeaders"] : name === "onSendHeaders" ? ["requestHeaders"] : [];
        wr[name].addListener(details => events.push({ name, details }), filter, extra);
    }
    return events;
}
const header = (headers, name) => (headers || []).find(h => h.name.toLowerCase() === name)?.value;
const fetchText = async (url, init) => {
    const response = await fetch(url, init);
    return `${response.status}:${await response.text()}`;
};
// The tab's title, once it has the prefix and (for pages that add their query) the suffix.
const tabTitle = async (tabId, prefix, suffix = "") => {
    let title = "";
    await waitFor(async () => {
        title = (await chrome.tabs.get(tabId)).title || "";
        return title.startsWith(prefix) && title.endsWith(suffix);
    });
    return title;
};

const tests = {
    // The events of a fetch, in order, with Chrome's details.
    async events() {
        const events = record();
        const body = JSON.parse((await fetchText(`${server}/echo?events`)).split(":").slice(1).join(":"));
        await waitFor(() => events.some(e => e.name === "onCompleted"));
        report.order = events.map(e => e.name).join(",");
        const ids = new Set(events.map(e => e.details.requestId));
        report.one_request_id = ids.size === 1 && typeof [...ids][0] === "string";
        const first = events[0].details;
        report.url = first.url;
        report.method = first.method;
        report.type = first.type;
        report.tab = first.tabId;
        report.frame = first.frameId;
        report.parent_frame = first.parentFrameId;
        report.initiator = first.initiator;
        report.time = typeof first.timeStamp === "number" && first.timeStamp > 0;
        const headers = events.find(e => e.name === "onHeadersReceived").details;
        report.status = headers.statusCode;
        report.status_line = headers.statusLine;
        report.x_test = header(headers.responseHeaders, "x-test");
        const completed = events.find(e => e.name === "onCompleted").details;
        report.from_cache = completed.fromCache;
        report.completed_status = completed.statusCode;
        const sent = events.find(e => e.name === "onSendHeaders").details;
        report.sent_has_headers = Array.isArray(sent.requestHeaders);
        report.before_has_no_headers = !("responseHeaders" in events.find(e => e.name === "onBeforeRequest").details);
        report.server_saw = typeof body["user-agent"] === "string";
    },

    async redirect() {
        const events = record();
        const response = await fetch(`${server}/redirect`);
        report.final = response.url;
        await waitFor(() => events.some(e => e.name === "onCompleted"));
        const redirect = events.find(e => e.name === "onBeforeRedirect");
        report.redirect_status = redirect?.details.statusCode;
        report.redirect_url = redirect?.details.redirectUrl;
        report.order = events.map(e => `${e.name}@${new URL(e.details.url).pathname}`).join(",");
        report.one_request_id = new Set(events.map(e => e.details.requestId)).size === 1;
    },

    // As 1Password does it: a tab's main frame, by tab.
    async tab() {
        const tab = await chrome.tabs.create({ url: "about:blank" });
        let resolved;
        const heard = new Promise(resolve => resolved = resolve);
        wr.onHeadersReceived.addListener(function listener(details) {
            if (details.statusCode >= 300 && details.statusCode < 400)
                return;
            wr.onHeadersReceived.removeListener(listener);
            resolved(details);
        }, { tabId: tab.id, urls: ["<all_urls>"] });
        const redirects = [];
        wr.onBeforeRedirect.addListener(details => redirects.push(details), { urls: ["<all_urls>"], types: ["main_frame"] });
        await chrome.tabs.update(tab.id, { url: `${server}/redirect-page` });
        const details = await Promise.race([heard, sleep(5000).then(() => null)]);
        report.status = details?.statusCode;
        report.type = details?.type;
        report.frame = details?.frameId;
        report.parent_frame = details?.parentFrameId;
        report.right_tab = details?.tabId === tab.id;
        report.url = details?.url;
        report.redirects = redirects.map(r => `${r.statusCode}>${new URL(r.redirectUrl).pathname}:${r.type}:${r.tabId === tab.id}`).join(",");
        await chrome.tabs.remove(tab.id);
    },

    async filters() {
        const heard = { image: 0, other_host: 0, other_tab: 0, all: 0, window: 0, too_many: 0 };
        // Past the browser's limit on a filter's patterns, a listener hears nothing.
        wr.onBeforeRequest.addListener(() => heard.too_many++, { urls: Array.from({ length: 1000 }, (_, i) => `http://h${i}.example/*`).concat(["<all_urls>"]) });
        wr.onBeforeRequest.addListener(() => heard.image++, { urls: ["<all_urls>"], types: ["image"] });
        wr.onBeforeRequest.addListener(() => heard.other_host++, { urls: ["http://example.com/*"] });
        wr.onBeforeRequest.addListener(() => heard.other_tab++, { urls: ["<all_urls>"], tabId: 12345 });
        wr.onBeforeRequest.addListener(() => heard.window++, { urls: ["<all_urls>"], windowId: 12345 });
        wr.onBeforeRequest.addListener(() => heard.all++, { urls: ["<all_urls>"], types: ["xmlhttprequest"] });
        await fetch(`${server}/echo?filters`);
        await waitFor(() => heard.all > 0);
        await sleep(100);
        Object.assign(report, heard);
    },

    async misuse() {
        const listener = () => {};
        await attempt("no_filter", () => { wr.onBeforeRequest.addListener(listener); return "added"; });
        await attempt("bad_urls", () => { wr.onBeforeRequest.addListener(listener, { urls: "x" }); return "added"; });
        await attempt("bad_extra", () => { wr.onBeforeRequest.addListener(listener, { urls: ["<all_urls>"] }, "blocking"); return "added"; });
        await attempt("bad_type", () => { wr.onBeforeRequest.addListener(listener, { urls: ["<all_urls>"], types: ["nope"] }); return "added"; });
        report.has_after_misuse = wr.onBeforeRequest.hasListener(listener);
        await attempt("handler_behavior_changed", async () => String(await wr.handlerBehaviorChanged()));
    },

    async errors() {
        const events = record();
        await dnr.updateSessionRules({ addRules: [{ id: 50, action: { type: "block" }, condition: { urlFilter: "/echo?blocked" } }] });
        await attempt("blocked", () => fetchText(`${server}/echo?blocked`));
        await dnr.updateSessionRules({ removeRuleIds: [50] });
        await waitFor(() => events.some(e => e.name === "onErrorOccurred"));
        const error = events.find(e => e.name === "onErrorOccurred")?.details;
        report.error = error?.error;
        report.blocked_order = events.map(e => e.name).join(",");
        report.after_unblock = await fetchText(`${server}/echo?unblocked`).then(text => text.split(":")[0]);
    },

    async auth_cancel() {
        let asked = 0;
        wr.onAuthRequired.addListener(details => {
            ++asked;
            report.details = `${details.statusCode}:${details.scheme}:${details.realm}:${details.isProxy}:${details.challenger.host}:${details.challenger.port === Number(new URL(server).port)}:${details.method}`;
            return { cancel: true };
        }, { urls: ["<all_urls>"] }, ["blocking"]);
        report.result = (await fetchText(`${server}/auth?cancel`, { credentials: "include" })).split(":")[0];
        report.asked = asked;
    },

    async auth_async() {
        let asked = 0;
        wr.onAuthRequired.addListener((details, callback) => {
            ++asked;
            setTimeout(() => callback({ authCredentials: { username: "user", password: "pass" } }), 50);
        }, { urls: ["<all_urls>"] }, ["asyncBlocking"]);
        report.result = await fetchText(`${server}/auth?async`, { credentials: "include" });
        report.asked = asked;
        // The browser remembers the credentials for the server, as Chrome does.
        report.again = await fetchText(`${server}/auth?again`, { credentials: "include" });
        report.asked_again = asked;
    },

    // A tab's page, with a promise for an answer, on the other server.
    async auth_navigation() {
        wr.onAuthRequired.addListener(async details => {
            report.type = details.type;
            return { authCredentials: { username: "user", password: "pass" } };
        }, { urls: ["<all_urls>"] }, ["blocking"]);
        const tab = await chrome.tabs.create({ url: `${alt}/auth?navigation` });
        report.title = await tabTitle(tab.id, "authed");
        await chrome.tabs.remove(tab.id);
    },

    async static_rules() {
        const echo = JSON.parse((await fetchText(`${server}/echo?static`)).slice(4));
        report.user_agent = echo["user-agent"] ?? "removed";
        report.x_dnr = echo["x-dnr"];
        report.enabled = (await dnr.getEnabledRulesets()).join(",");
        report.available = await dnr.getAvailableStaticRuleCount();
        report.before_enable = (await fetchText(`${server}/echo?ruleset2`)).split(":")[0];
        await dnr.updateEnabledRulesets({ enableRulesetIds: ["ruleset_2"] });
        await attempt("after_enable", () => fetchText(`${server}/echo?ruleset2`));
        report.enabled_after = (await dnr.getEnabledRulesets()).sort().join(",");
        report.available_after = await dnr.getAvailableStaticRuleCount();
        await attempt("enable_unknown", () => dnr.updateEnabledRulesets({ enableRulesetIds: ["nope"] }));
        await attempt("bad_options", () => dnr.updateEnabledRulesets("ruleset_2"));
        await attempt("bad_ids", () => dnr.updateEnabledRulesets({ disableRulesetIds: [5] }));
        report.enabled_after_errors = (await dnr.getEnabledRulesets()).sort().join(",");
        await dnr.updateEnabledRulesets({ disableRulesetIds: ["ruleset_2"] });
        report.after_disable = (await fetchText(`${server}/echo?ruleset2`)).split(":")[0];
        report.enabled_end = (await dnr.getEnabledRulesets()).join(",");
    },

    async dynamic_rules() {
        await dnr.updateDynamicRules({
            removeRuleIds: (await dnr.getDynamicRules()).map(rule => rule.id),
            addRules: [
                { id: 1, action: { type: "block" }, condition: { urlFilter: "/echo?dyn-block", resourceTypes: ["xmlhttprequest"] } },
                { id: 2, action: { type: "redirect", redirect: { transform: { query: "?redirected-by-rule" } } }, condition: { urlFilter: "/echo?dyn-redirect" } },
                { id: 3, action: { type: "modifyHeaders", responseHeaders: [{ header: "x-dnr-response", operation: "set", value: "dynamic" }], requestHeaders: [{ header: "x-dnr", operation: "set", value: "dynamic" }] }, condition: { urlFilter: "/echo?dyn-headers" } },
                { id: 4, priority: 2, action: { type: "allow" }, condition: { urlFilter: "/echo?dyn-block&allowed" } },
            ],
        });
        report.ids = (await dnr.getDynamicRules()).map(rule => rule.id).sort().join(",");
        report.filtered = (await dnr.getDynamicRules({ ruleIds: [2, 9] })).map(rule => rule.id).join(",");
        report.priority_default = (await dnr.getDynamicRules({ ruleIds: [1] }))[0].priority;
        await attempt("blocked", () => fetchText(`${server}/echo?dyn-block`));
        report.allowed = (await fetchText(`${server}/echo?dyn-block&allowed`)).split(":")[0];
        const redirected = await fetch(`${server}/echo?dyn-redirect`);
        report.redirected = `${redirected.redirected}:${new URL(redirected.url).search}`;
        const modified = await fetch(`${server}/echo?dyn-headers`);
        report.response_header = modified.headers.get("x-dnr-response");
        report.request_header = (await modified.json())["x-dnr"];

        await attempt("duplicate", () => dnr.updateDynamicRules({ addRules: [{ id: 1, action: { type: "block" }, condition: {} }] }));
        await attempt("invalid", () => dnr.updateDynamicRules({ addRules: [{ id: 9, action: { type: "explode" }, condition: {} }] }));
        await attempt("tab_ids", () => dnr.updateDynamicRules({ addRules: [{ id: 9, action: { type: "block" }, condition: { tabIds: [1] } }] }));
        await attempt("no_options", () => dnr.updateDynamicRules());
        await attempt("bad_options", () => dnr.updateDynamicRules("x"));
        report.ids_after_errors = (await dnr.getDynamicRules()).map(rule => rule.id).sort().join(",");
        await dnr.updateDynamicRules({ removeRuleIds: [1, 3, 4] });
        report.ids_after_remove = (await dnr.getDynamicRules()).map(rule => rule.id).join(",");
        report.unblocked = (await fetchText(`${server}/echo?dyn-block`)).split(":")[0];
        report.other_blocked = (await fetchText(`${server}/echo?other-blocked`)).split(":")[0];
        await attempt("regex_ok", async () => JSON.stringify(await dnr.isRegexSupported({ regex: "^https?://[a-z]+" })));
        await attempt("regex_bad", async () => JSON.stringify(await dnr.isRegexSupported({ regex: "(" })));
        await attempt("regex_missing", () => dnr.isRegexSupported({}));
        report.constants = `${dnr.MAX_NUMBER_OF_DYNAMIC_RULES}:${dnr.DYNAMIC_RULESET_ID}`;
    },

    // As 1Password's related origins fetcher does it: a session rule that lets a page read another origin's file.
    async session_rules() {
        const tab = await chrome.tabs.create({ url: `${server}/cors-page?before` });
        report.before = await tabTitle(tab.id, "cors:", "?before");
        await dnr.updateSessionRules({
            addRules: [{
                id: 1000000,
                priority: 1,
                action: { type: "modifyHeaders", requestHeaders: [{ header: "sec-fetch-site", operation: "set", value: "none" }], responseHeaders: [{ header: "access-control-allow-origin", operation: "set", value: "*" }] },
                condition: { requestDomains: [new URL(alt).hostname], urlFilter: "/nocors", resourceTypes: ["xmlhttprequest"] },
            }],
        });
        report.rules = (await dnr.getSessionRules()).map(rule => rule.id).join(",");
        await chrome.tabs.update(tab.id, { url: `${server}/cors-page?with-rule` });
        report.with_rule = await tabTitle(tab.id, "cors:", "?with-rule");

        // tabIds: only in this tab.
        await dnr.updateSessionRules({ removeRuleIds: [1000000], addRules: [{ id: 7, action: { type: "block" }, condition: { urlFilter: "/nocors", tabIds: [tab.id] } }] });
        await chrome.tabs.update(tab.id, { url: `${server}/cors-page?blocked-in-tab` });
        report.tab_rule = await tabTitle(tab.id, "cors:", "?blocked-in-tab");
        await dnr.updateSessionRules({ removeRuleIds: [7] });
        report.rules_end = (await dnr.getSessionRules()).length;
        await chrome.tabs.remove(tab.id);
    },
};

(async () => {
    const test = tests[params.get("test")];
    if (test)
        await test();
    report._ = "done";
    document.title = JSON.stringify(report);
})();
