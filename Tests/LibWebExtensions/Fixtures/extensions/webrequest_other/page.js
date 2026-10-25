// Reports what the background heard, and what this extension's rules are.
(async () => {
    const report = {};
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
