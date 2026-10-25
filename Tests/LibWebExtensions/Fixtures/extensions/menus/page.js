// What a page of the extension can do with context menus. ?step=… picks what to do; the report is JSON in the title.
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
    // create() calls back, with runtime.lastError on failure.
    const create = properties => new Promise(resolve => {
        const returned = chrome.contextMenus.create(properties, () => {
            resolve(chrome.runtime.lastError ? `error: ${chrome.runtime.lastError.message}` : `created ${JSON.stringify(returned)}`);
        });
    });
    if (step === "create") {
        await attempt("remove_all", async () => (await chrome.contextMenus.removeAll(), "removed"));
        report.settings = await create({ id: "settings", title: "Settings", contexts: ["action"] });
        report.lock = await create({ id: "lock", title: "Lock", contexts: ["all"] });
        report.save = await create({ id: "save", title: "Save", contexts: ["all"] });
        report.save_login = await create({ id: "saveLogin", title: "Login", contexts: ["all"], parentId: "save" });
        report.flag = await create({ id: "flag", type: "checkbox", title: "Flag", contexts: ["all"], parentId: "save", checked: true });
        report.selection = await create({ id: "sel", title: "Search for “%s”", contexts: ["selection"] });
        report.link = await create({ id: "link", title: "Link item", contexts: ["link"], targetUrlPatterns: ["https://example.com/*"] });
        report.image = await create({ id: 7, title: "Image item", contexts: ["image"] });
        report.document_patterns = await create({ id: "docpat", title: "Only example.org", documentUrlPatterns: ["https://example.org/*"] });
        report.editable = await create({ id: "edit", title: "Editable item", contexts: ["editable"] });
        report.hidden = await create({ id: "hidden", title: "Hidden", contexts: ["all"], visible: false });
        report.disabled = await create({ id: "off", title: "Disabled", contexts: ["all"], enabled: false });
        report.radios = await create({ id: "radios", title: "Radios", contexts: ["all"] });
        report.r1 = await create({ id: "r1", type: "radio", title: "R1", contexts: ["all"], parentId: "radios" });
        report.r2 = await create({ id: "r2", type: "radio", title: "R2", contexts: ["all"], parentId: "radios" });
        report.r3 = await create({ id: "r3", type: "radio", title: "R3", contexts: ["all"], parentId: "radios", checked: true });
        report.separator = await create({ id: "sep", type: "separator", contexts: ["all"] });
        report.generated = await create({ title: "No ID", contexts: ["all"] });
        report.to_remove = await create({ id: "gone", title: "Gone", contexts: ["all"], parentId: "radios" });

        report.duplicate = await create({ id: "lock", title: "Again", contexts: ["all"] });
        report.missing_parent = await create({ id: "orphan", title: "Orphan", parentId: "nothing" });
        report.bad_context = await create({ id: "badcontext", title: "x", contexts: ["nonsense"] });
        report.bad_type = await create({ id: "badtype", title: "x", type: "fancy" });
        report.no_title = await create({ id: "notitle" });
        report.bad_title = await create({ id: "badtitle", title: 42 });
        report.bad_pattern = await create({ id: "badpattern", title: "x", documentUrlPatterns: ["not a pattern"] });
        report.bad_id = await create({ id: { nested: true }, title: "x" });
        report.bad_properties = await create("not an object");
        report.separator_parent = await create({ id: "child", title: "x", parentId: "sep" });

        await attempt("update", async () => (await chrome.contextMenus.update("lock", { title: "Lock now" }), "updated"));
        await attempt("update_missing", async () => (await chrome.contextMenus.update("nothing", { title: "x" }), "updated"));
        await attempt("update_cycle", async () => (await chrome.contextMenus.update("save", { parentId: "saveLogin" }), "updated"));
        await attempt("update_self", async () => (await chrome.contextMenus.update("save", { parentId: "save" }), "updated"));
        await attempt("update_bad", async () => (await chrome.contextMenus.update("lock", { contexts: ["nonsense"] }), "updated"));
        await attempt("remove", async () => (await chrome.contextMenus.remove("gone"), "removed"));
        await attempt("remove_missing", async () => (await chrome.contextMenus.remove("nothing"), "removed"));
    } else if (step === "remove_all") {
        await attempt("remove_all", async () => (await chrome.contextMenus.removeAll(), "removed"));
    } else {
        // Wait for what the browser did.
        for (let tries = 0; tries < 100; ++tries) {
            const { clicks = [] } = await chrome.storage.session.get("clicks");
            if (clicks.length >= Number(step))
                break;
            await new Promise(resolve => setTimeout(resolve, 50));
        }
        const { clicks = [] } = await chrome.storage.session.get("clicks");
        report.clicks = JSON.stringify(clicks);
    }
    document.title = JSON.stringify(report);
})();
