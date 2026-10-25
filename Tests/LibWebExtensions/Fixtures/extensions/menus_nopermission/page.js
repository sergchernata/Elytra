// Without the "contextMenus" permission, nothing works.
(async () => {
    const report = {};
    report.create = await new Promise(resolve => chrome.contextMenus.create({ id: "x", title: "x" }, () => resolve(chrome.runtime.lastError ? `error: ${chrome.runtime.lastError.message}` : "created")));
    try {
        await chrome.contextMenus.removeAll();
        report.remove_all = "removed";
    } catch (e) {
        report.remove_all = `error: ${e.message}`;
    }
    document.title = JSON.stringify(report);
})();
