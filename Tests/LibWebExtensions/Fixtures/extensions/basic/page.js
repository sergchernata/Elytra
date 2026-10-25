// Reports what an extension page can do, as JSON in the title.
const report = {
    origin: location.origin,
    secure: isSecureContext,
    module: import.meta.url,
};
try {
    localStorage.setItem("k", "v");
    report.local_storage = localStorage.getItem("k");
} catch (e) {
    report.local_storage = String(e);
}
try {
    report.fetch = (await (await fetch("private.txt")).text()).trim();
} catch (e) {
    report.fetch = String(e);
}
document.title = JSON.stringify(report);
