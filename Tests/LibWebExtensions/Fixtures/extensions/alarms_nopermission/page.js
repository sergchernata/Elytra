// Without the permissions, chrome.alarms and chrome.idle refuse; chrome.i18n needs none.
(async () => {
    const report = {};
    const attempt = async (name, fn) => {
        try {
            report[name] = String(await fn());
        } catch (e) {
            report[name] = `error: ${e.message}`;
        }
    };
    await attempt("create", () => chrome.alarms.create("a", { delayInMinutes: 1 }));
    await attempt("get_all", () => chrome.alarms.getAll());
    await attempt("clear_all", () => chrome.alarms.clearAll());
    await attempt("query", () => chrome.idle.queryState(60));
    await attempt("interval", () => chrome.idle.setDetectionInterval(60));
    await attempt("accept", async () => (await chrome.i18n.getAcceptLanguages()).length > 0);
    document.title = JSON.stringify(report);
})();
