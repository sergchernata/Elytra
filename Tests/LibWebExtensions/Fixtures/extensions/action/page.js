// What a page of the extension can do with its toolbar button. ?step=… picks what to do; the report is JSON in the
// title.
(async () => {
    const step = new URLSearchParams(location.search).get("step");
    const report = {};
    const attempt = async (name, fn) => {
        try {
            report[name] = await fn();
        } catch (e) {
            report[name] = `error: ${e.message}`;
        }
    };
    if (step === "set") {
        await attempt("default_title", () => chrome.action.getTitle({}));
        await attempt("default_popup", () => chrome.action.getPopup({}));
        await chrome.action.setTitle({ title: "Everywhere" });
        await chrome.action.setTitle({ title: "Just tab 5", tabId: 5 });
        await chrome.action.setBadgeText({ text: "42" });
        await chrome.action.setBadgeBackgroundColor({ color: "#ff0000" });
        await chrome.action.setBadgeTextColor({ color: [0, 0, 255, 255] });
        await attempt("title", () => chrome.action.getTitle({}));
        await attempt("tab_title", () => chrome.action.getTitle({ tabId: 5 }));
        await attempt("other_tab_title", () => chrome.action.getTitle({ tabId: 6 }));
        await attempt("badge", () => chrome.action.getBadgeText({}));
        await attempt("badge_color", async () => JSON.stringify(await chrome.action.getBadgeBackgroundColor({})));
        await attempt("badge_text_color", async () => JSON.stringify(await chrome.action.getBadgeTextColor({})));
        await attempt("bad_color", () => chrome.action.setBadgeBackgroundColor({ color: "not a colour" }));
        await attempt("bad_popup", () => chrome.action.setPopup({ popup: "https://example.com/popup.html" }));
        await attempt("escaping_popup", () => chrome.action.setPopup({ popup: "../../etc/passwd" }));
        await chrome.action.disable(7);
        await attempt("enabled", () => chrome.action.isEnabled());
        await attempt("enabled_tab_7", () => chrome.action.isEnabled(7));
        await attempt("settings", async () => JSON.stringify(await chrome.action.getUserSettings()));
        await attempt("commands", async () => JSON.stringify((await chrome.commands.getAll()).map(c => [c.name, c.shortcut, c.description ?? ""])));
        await chrome.action.setPopup({ popup: "" });
    } else {
        // Wait for what the browser did.
        for (let tries = 0; tries < 100; ++tries) {
            const items = await chrome.storage.session.get(null);
            if (items[step] !== undefined)
                break;
            await new Promise(resolve => setTimeout(resolve, 50));
        }
        Object.assign(report, await chrome.storage.session.get(null));
    }
    document.title = JSON.stringify(report);
})();
