// Reports what the chrome.* API gives an extension page, as JSON in the title.
(async () => {
    const report = {};
    const attempt = async (name, fn) => {
        try {
            report[name] = await fn();
        } catch (e) {
            report[name] = `error: ${e.message}`;
        }
    };
    await attempt("bridge_hidden", () => typeof __extensionBridge === "undefined");
    await attempt("id", () => chrome.runtime.id);
    await attempt("url", () => chrome.runtime.getURL("/popup/index.html"));
    await attempt("manifest_version", () => chrome.runtime.getManifest().version);
    await attempt("message", () => chrome.i18n.getMessage("greeting", ["3", "Ada"]));
    await attempt("missing_message", () => chrome.i18n.getMessage("nope"));
    await attempt("platform", async () => (await chrome.runtime.getPlatformInfo()).os);
    await attempt("storage", async () => {
        await chrome.storage.local.set({ answer: 42, list: [1, 2] });
        const items = await chrome.storage.local.get(["answer", "list", "absent"]);
        return JSON.stringify(items);
    });
    await attempt("callback_style", () => new Promise(resolve => chrome.storage.local.get("answer", items => resolve(items.answer))));
    await attempt("unknown_api", async () => {
        try {
            await chrome.management.launchApp("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
            return "resolved";
        } catch (e) {
            return "rejected";
        }
    });
    await attempt("last_error", () => new Promise(resolve => chrome.storage.local.get(12, () => resolve(chrome.runtime.lastError ? "set" : "unset"))));
    document.title = JSON.stringify(report);
})();
