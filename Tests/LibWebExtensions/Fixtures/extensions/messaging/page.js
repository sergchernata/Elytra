// Talks to the background, and reports what came back as JSON in the title.
(async () => {
    const report = {};
    const attempt = async (name, fn) => {
        try {
            report[name] = await fn();
        } catch (e) {
            report[name] = `error: ${e.message}`;
        }
    };
    // The background may still be starting.
    for (let tries = 0; tries < 100; ++tries) {
        const { installed } = await chrome.storage.local.get("installed");
        if (installed)
            break;
        await new Promise(resolve => setTimeout(resolve, 50));
    }
    await attempt("installed", async () => (await chrome.storage.local.get("installed")).installed);
    await attempt("background_url", async () => (await chrome.storage.local.get("background_url")).background_url);
    await attempt("echo", () => chrome.runtime.sendMessage({ kind: "echo", value: 7 }));
    await attempt("later", () => chrome.runtime.sendMessage({ kind: "later" }));
    await attempt("promise", () => chrome.runtime.sendMessage({ kind: "promise" }));
    await attempt("clients", () => chrome.runtime.sendMessage({ kind: "clients" }));
    await attempt("unanswered", async () => String(await chrome.runtime.sendMessage({ kind: "nothing" })));
    await attempt("callback", () => new Promise(resolve => chrome.runtime.sendMessage({ kind: "echo", value: "cb" }, response => resolve(response.echo))));
    await attempt("port", () => new Promise(resolve => {
        const port = chrome.runtime.connect({ name: "lane" });
        const got = [];
        port.onMessage.addListener(message => {
            got.push(message);
            if (got.length === 2)
                port.postMessage("bye");
        });
        port.onDisconnect.addListener(() => resolve(JSON.stringify(got)));
        port.postMessage(1);
        port.postMessage("two");
    }));
    document.title = JSON.stringify(report);
})();
