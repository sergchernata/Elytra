// ?step=set stores something and is granted an optional permission; ?step=check reports what's left.
(async () => {
    const step = new URLSearchParams(location.search).get("step");
    const report = {};
    if (step === "set") {
        await chrome.storage.local.set({ kept: 1 });
        report.granted = await chrome.permissions.request({ permissions: ["alarms"] });
    }
    report.stored = (await chrome.storage.local.get("kept")).kept ?? null;
    report.alarms = await chrome.permissions.contains({ permissions: ["alarms"] });
    document.title = JSON.stringify(report);
})();
