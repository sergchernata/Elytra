// Alarms are refused until the user grants the optional permission, and work after.
(async () => {
    const report = {};
    const attempt = async (name, fn) => {
        try {
            report[name] = await fn();
        } catch (e) {
            report[name] = `error: ${e.message}`;
        }
    };
    await attempt("before", async () => { await chrome.alarms.create("a", { delayInMinutes: 1 }); return "created"; });
    await attempt("granted", () => chrome.permissions.request({ permissions: ["alarms"] }));
    await attempt("after", async () => { await chrome.alarms.create("a", { delayInMinutes: 1 }); return (await chrome.alarms.get("a")).name; });
    document.title = JSON.stringify(report);
})();
