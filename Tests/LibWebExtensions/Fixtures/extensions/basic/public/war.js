// A web accessible page: reports what it is, and whether it runs under the extension's CSP.
let evaluated = "blocked";
try {
    eval("evaluated = 'ran'");
} catch (e) {
}
const report = JSON.stringify({
    real: true,
    origin: location.origin,
    eval: evaluated,
    chrome: typeof chrome,
});
document.title = report;
// In a frame, it tells the page embedding it.
if (window.parent !== window)
    window.parent.postMessage(report, "*");
