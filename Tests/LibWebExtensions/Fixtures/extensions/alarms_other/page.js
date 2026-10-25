// Sees only its own alarms, including those kept from before, and reports in the title.
(async () => {
    const report = {};
    for (let tries = 0; tries < 200; ++tries) {
        const { fired } = await chrome.storage.session.get("fired");
        if (fired && fired.includes("missed"))
            break;
        await new Promise(resolve => setTimeout(resolve, 25));
    }
    report.fired = ((await chrome.storage.session.get("fired")).fired || []).join(",");
    const all = await chrome.alarms.getAll();
    report.names = all.map(alarm => alarm.name).sort().join(",");
    const saved = all.find(alarm => alarm.name === "saved");
    report.saved = saved ? `${saved.scheduledTime}:${saved.periodInMinutes}` : "missing";
    report.clear_others = await chrome.alarms.clear("kept");
    report.get_others = String(await chrome.alarms.get("kept"));
    document.title = JSON.stringify(report);
})();
