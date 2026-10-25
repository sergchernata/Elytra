// Runs once the DOM is parsed, in the same isolated world as start.js, and reports as JSON in the title.
(async () => {
    const report = {
        same_world: window.isolatedValue,
        ready_state_at_start: window.readyStateAtStart,
        page_scripts_before_start: window.pageScriptsRanBeforeStart,
        ready_state_at_end: document.readyState,
        page_global: typeof window.pageSecret,
        main_world_global: typeof window.fromMainWorld,
        dom_text: document.getElementById("secret")?.textContent,
        page_view: document.body.dataset.page,
        id: chrome.runtime.id,
        has_tabs_api: typeof chrome.tabs,
        closed_shadow: chrome.dom.openOrClosedShadowRoot(document.getElementById("host"))?.textContent,
    };
    try {
        report.background = await chrome.runtime.sendMessage("hi");
    } catch (e) {
        report.background = `error: ${e.message}`;
    }
    document.title = JSON.stringify(report);
})();
