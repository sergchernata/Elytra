// Reports what the background heard, and what this extension's rules are; or, with ?server=&alt=, what it may read
// across origins.
const read = async url => {
    try {
        const response = await fetch(url);
        return (await response.json()).ok ? "read" : "unreadable";
    } catch (e) {
        return "blocked";
    }
};

(async () => {
    const report = {};
    const params = new URLSearchParams(location.search);
    if (params.has("server")) {
        // As in Chrome, an extension's own pages read what its host permissions cover without CORS (1Password's
        // servers send no CORS headers): 127.0.0.1 is this extension's, localhost isn't.
        report.permitted = await read(`${params.get("server")}/nocors`);
        report.not_permitted = await read(`${params.get("alt")}/nocors`);
        report._ = "done";
        document.title = JSON.stringify(report);
        return;
    }
    for (let tries = 0; tries < 200 && !(await chrome.storage.session.get("ready")).ready; ++tries)
        await new Promise(resolve => setTimeout(resolve, 25));
    const { heard } = await chrome.storage.session.get("heard");
    report.urls = (heard && heard.urls || []).join(" ");
    report.auth = heard ? heard.auth : -1;
    report.ids = (heard && heard.ids || []).join(",");
    report.dynamic = (await chrome.declarativeNetRequest.getDynamicRules()).map(rule => rule.id).join(",");
    report.session = (await chrome.declarativeNetRequest.getSessionRules()).length;
    report._ = "done";
    document.title = JSON.stringify(report);
})();
