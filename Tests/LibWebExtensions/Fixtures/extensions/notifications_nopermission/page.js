// Without the "notifications" permission, nothing works.
(async () => {
    const report = {};
    const attempt = async (name, fn) => {
        try {
            report[name] = await fn();
        } catch (e) {
            report[name] = `error: ${e.message}`;
        }
    };
    await attempt("create", () => chrome.notifications.create("n1", { type: "basic", iconUrl: "icon.png", title: "t", message: "m" }));
    await attempt("all", () => chrome.notifications.getAll());
    await attempt("level", () => chrome.notifications.getPermissionLevel());
    document.title = JSON.stringify(report);
})();
