// What a page of the extension can do with notifications. ?step=… picks what to do; the report is JSON in the title.
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
    const basic = { type: "basic", iconUrl: "icon.png", title: "Saved", message: "The item was saved." };
    if (step === "create") {
        await attempt("named", () => chrome.notifications.create("n1", {
            ...basic,
            iconUrl: "/icon.png",
            contextMessage: "1Password",
            priority: 2,
            eventTime: 1700000000000,
            buttons: [{ title: "Open" }, { title: "Dismiss" }, { title: "Too many" }],
            requireInteraction: true,
            silent: true,
            isClickable: true,
        }));
        await attempt("generated", () => chrome.notifications.create(basic));
        await attempt("generated_empty", () => chrome.notifications.create("", basic));
        await attempt("callback", () => new Promise(resolve => chrome.notifications.create("cb", basic, id => resolve(id))));
        const png = await (await fetch("icon.png")).arrayBuffer();
        const bytes = new Uint8Array(png);
        let binary = "";
        for (const byte of bytes)
            binary += String.fromCharCode(byte);
        await attempt("data_icon", () => chrome.notifications.create("data", { ...basic, iconUrl: `data:image/png;base64,${btoa(binary)}` }));
        await attempt("blob_icon", () => chrome.notifications.create("blob", { ...basic, iconUrl: URL.createObjectURL(new Blob([png], { type: "image/png" })) }));
        await attempt("own_url_icon", () => chrome.notifications.create("own", { ...basic, iconUrl: chrome.runtime.getURL("icon.png") }));
        await attempt("list", () => chrome.notifications.create("list", { ...basic, type: "list", items: [{ title: "One", message: "First" }, { title: "Two", message: "Second" }] }));
        await attempt("progress", () => chrome.notifications.create("progress", { ...basic, type: "progress", progress: 42 }));
        await attempt("remote_icon", () => chrome.notifications.create("remote", { ...basic, iconUrl: "https://example.com/icon.png" }));
        await attempt("escaping_icon", () => chrome.notifications.create("escaping", { ...basic, iconUrl: "../../../../etc/passwd" }));
        await attempt("missing_icon", () => chrome.notifications.create("missing", { ...basic, iconUrl: "nothing-here.png" }));
        await attempt("other_extension_icon", () => chrome.notifications.create("other", { ...basic, iconUrl: "chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/icon.png" }));
        await attempt("no_title", () => chrome.notifications.create("notitle", { type: "basic", iconUrl: "icon.png", message: "m" }));
        await attempt("no_type", () => chrome.notifications.create("notype", { iconUrl: "icon.png", title: "t", message: "m" }));
        await attempt("bad_type", () => chrome.notifications.create("badtype", { ...basic, type: "fancy" }));
        await attempt("bad_title", () => chrome.notifications.create("badtitle", { ...basic, title: 42 }));
        await attempt("bad_options", () => chrome.notifications.create("badoptions", "not an object"));
        await attempt("bad_id", () => chrome.notifications.create(42, basic));
        await attempt("bad_progress", () => chrome.notifications.create("badprogress", { ...basic, type: "progress", progress: 150 }));
        await attempt("items_on_basic", () => chrome.notifications.create("itemsbasic", { ...basic, items: [{ title: "x", message: "y" }] }));
        await attempt("image_without_url", () => chrome.notifications.create("noimage", { ...basic, type: "image" }));
        await attempt("bad_buttons", () => chrome.notifications.create("badbuttons", { ...basic, buttons: "Open" }));
        await attempt("updated", () => chrome.notifications.update("n1", { title: "Saved again", progress: undefined }));
        await attempt("updated_missing", () => chrome.notifications.update("nothing", { title: "x" }));
        await attempt("updated_bad", () => chrome.notifications.update("n1", { priority: "high" }));
        await attempt("cleared", () => chrome.notifications.clear("cb"));
        await attempt("cleared_missing", () => chrome.notifications.clear("nothing"));
        await attempt("all", async () => Object.keys(await chrome.notifications.getAll()).sort().join(","));
        await attempt("all_values", async () => Object.values(await chrome.notifications.getAll()).every(value => value === true));
        await attempt("level", () => chrome.notifications.getPermissionLevel());
    } else if (step === "replace") {
        // Creating with an ID in use replaces that notification.
        await attempt("replaced", () => chrome.notifications.create("n1", { ...basic, title: "Replaced", buttons: [{ title: "One" }, { title: "Two" }] }));
        await attempt("all", async () => Object.keys(await chrome.notifications.getAll()).sort().join(","));
    } else if (step === "all") {
        await attempt("all", async () => Object.keys(await chrome.notifications.getAll()).sort().join(","));
    } else {
        // Wait for what the browser did.
        for (let tries = 0; tries < 100; ++tries) {
            const items = await chrome.storage.session.get(null);
            if (items[step] !== undefined)
                break;
            await new Promise(resolve => setTimeout(resolve, 50));
        }
        const items = await chrome.storage.session.get(null);
        for (const [key, value] of Object.entries(items))
            report[key] = JSON.stringify(value);
    }
    document.title = JSON.stringify(report);
})();
