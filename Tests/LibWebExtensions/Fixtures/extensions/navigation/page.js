// The page's fetch runs under the manifest's string CSP, which allows no connections.
(async () => {
    const report = { origin: location.origin };
    try { await fetch("page.html"); report.page_fetch = "loaded"; } catch (e) { report.page_fetch = "blocked"; }
    document.title = JSON.stringify(report);
})();
