// The page's fetch runs under the manifest's string CSP, which allows no connections (but to the extension's own files,
// which as in Chrome no CSP holds back).
(async () => {
    const report = { origin: location.origin };
    try { await fetch("data:text/plain,elsewhere"); report.page_fetch = "loaded"; } catch (e) { report.page_fetch = "blocked"; }
    try { await fetch("page.html"); report.own_fetch = "loaded"; } catch (e) { report.own_fetch = "blocked"; }
    document.title = JSON.stringify(report);
})();
