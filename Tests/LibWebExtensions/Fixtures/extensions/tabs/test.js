// The cases of TestExtensionTabs, chosen by ?case=: each reports what it saw as JSON in the title.
const parameters = new URLSearchParams(location.search);
const directory = parameters.get("dir"); // file:///.../ with the test's pages

const report = result => { document.title = JSON.stringify(result); };
const failure = async promise => {
    try {
        await promise;
        return "resolved";
    } catch (error) {
        return error.message;
    }
};
const titles = tabs => tabs.map(tab => tab.title ?? null);
const basename = url => (url ?? "").replace(/^.*\//, "");
const eventually = async (condition, what) => {
    for (let i = 0; i < 400; ++i) {
        if (condition())
            return true;
        await new Promise(resolve => setTimeout(resolve, 25));
    }
    throw new Error(`Timed out waiting for ${what}`);
};
const tabNamed = async name => (await chrome.tabs.query({ url: `file:///*/${name}` }))[0];

const cases = {
    async describe() {
        const all = await chrome.tabs.query({});
        const self = await chrome.tabs.getCurrent();
        const bee = all.find(tab => basename(tab.url) === "bee.html");
        const sea = all.find(tab => basename(tab.url) === "sea.html");
        const ids = tabs => tabs.map(tab => tab.id);
        return {
            count: all.length,
            ids_unique: new Set(ids(all)).size === all.length,
            self: { url: self.url, index: self.index, active: self.active, same_window_as_bee: self.windowId === bee.windowId },
            bee: { title: bee.title, index: bee.index, active: bee.active, pinned: bee.pinned, status: bee.status, incognito: bee.incognito, highlighted: bee.highlighted, muted: bee.mutedInfo.muted, favicon: bee.favIconUrl ?? null },
            sea: { title: sea.title, active: sea.active, other_window: sea.windowId !== self.windowId },
            active: (await chrome.tabs.query({ active: true })).length,
            current: JSON.stringify(ids(await chrome.tabs.query({ active: true, currentWindow: true }))) === JSON.stringify([self.id]),
            last_focused: JSON.stringify(ids(await chrome.tabs.query({ active: true, lastFocusedWindow: true }))) === JSON.stringify([self.id]),
            not_current: titles(await chrome.tabs.query({ currentWindow: false })),
            tabs_in_current_window: (await chrome.tabs.query({ windowId: chrome.windows.WINDOW_ID_CURRENT })).length,
            url_pattern: titles(await chrome.tabs.query({ url: "file:///*/bee.html" })),
            url_patterns: titles(await chrome.tabs.query({ url: ["*://nowhere.example/*", "file:///*/sea.html"] })),
            // The extension's own pages, by URL (1Password looks for its app tab so).
            own_page: JSON.stringify(ids(await chrome.tabs.query({ url: chrome.runtime.getURL("test.html*") }))) === JSON.stringify([self.id]),
            title_glob: titles(await chrome.tabs.query({ title: "B*" })),
            pinned: titles(await chrome.tabs.query({ pinned: true })),
            popup: titles(await chrome.tabs.query({ windowType: "popup" })),
            index: titles(await chrome.tabs.query({ index: 1, windowId: self.windowId })),
            complete_popup: titles(await chrome.tabs.query({ status: "complete", windowType: "popup" })),
            audible: (await chrome.tabs.query({ audible: true })).length,
            bad_pattern: await failure(chrome.tabs.query({ url: "not a pattern" })),
            bad_query: await failure(chrome.tabs.query("x")),
            bad_window: await failure(chrome.tabs.query({ windowId: "x" })),
            get: (await chrome.tabs.get(bee.id)).title,
            get_missing: await failure(chrome.tabs.get(99999)),
            get_bad: await failure(chrome.tabs.get("x")),
            get_none: await failure(chrome.tabs.get()),
            windows: (await chrome.windows.getAll({ populate: true })).map(window => ({ type: window.type, focused: window.focused, tabs: window.tabs.length, incognito: window.incognito, state: window.state })),
            unpopulated: (await chrome.windows.getAll()).every(window => window.tabs === undefined),
            popup_only: (await chrome.windows.getAll({ windowTypes: ["popup"] })).length,
            window_current: (await chrome.windows.get(chrome.windows.WINDOW_ID_CURRENT)).id === self.windowId,
            get_current: (await chrome.windows.getCurrent()).id === self.windowId,
            last_focused_window: (await chrome.windows.getLastFocused()).id === self.windowId,
            window_missing: await failure(chrome.windows.get(999)),
            window_bad: await failure(chrome.windows.get("x")),
            window_populated: titles((await chrome.windows.get(sea.windowId, { populate: true })).tabs),
            window_bounds: (({ left, top, width, height }) => ({ left, top, width, height }))(await chrome.windows.get(sea.windowId)),
            callback_style: await new Promise(resolve => chrome.tabs.query({ windowType: "popup" }, tabs => resolve(tabs.length))),
        };
    },

    // Tabs change, and the extension hears of it.
    async change() {
        const log = [];
        const self = await chrome.tabs.getCurrent();
        chrome.tabs.onCreated.addListener(tab => log.push({ event: "created", tab: tab.id, window: tab.windowId }));
        chrome.tabs.onUpdated.addListener((tabId, change, tab) => log.push({ event: "updated", tab: tabId, change, same: tab.id === tabId }));
        chrome.tabs.onActivated.addListener(info => log.push({ event: "activated", tab: info.tabId, window: info.windowId }));
        chrome.tabs.onRemoved.addListener((tabId, info) => log.push({ event: "removed", tab: tabId, window: info.windowId, closing: info.isWindowClosing }));
        chrome.windows.onCreated.addListener(window => log.push({ event: "window created", window: window.id, type: window.type }));
        chrome.windows.onRemoved.addListener(windowId => log.push({ event: "window removed", window: windowId }));
        chrome.windows.onFocusChanged.addListener(windowId => log.push({ event: "focus", window: windowId }));
        const seen = predicate => log.some(predicate);

        const created = await chrome.tabs.create({ url: directory + "dee.html", active: false });
        await eventually(() => seen(entry => entry.event === "updated" && entry.tab === created.id && entry.change.status === "complete"), "the new tab to load");
        await eventually(() => seen(entry => entry.event === "updated" && entry.tab === created.id && entry.change.title === "Dee"), "the new tab's title");
        const loaded = await chrome.tabs.get(created.id);

        const pinned = await chrome.tabs.update(created.id, { pinned: true });
        await eventually(() => seen(entry => entry.event === "updated" && entry.tab === created.id && entry.change.pinned === true), "the pin");
        await chrome.tabs.update(created.id, { active: true });
        await eventually(() => seen(entry => entry.event === "activated" && entry.tab === created.id), "the activation");
        const completes = () => log.filter(entry => entry.event === "updated" && entry.tab === created.id && entry.change.status === "complete").length;
        await chrome.tabs.reload(created.id);
        await eventually(() => completes() === 2, "the reload");
        await chrome.tabs.update(created.id, { url: directory + "sea.html" });
        await eventually(() => completes() === 3, "the navigation");
        const moved = await chrome.tabs.move(created.id, { index: 0 });
        await chrome.tabs.remove(created.id);
        await eventually(() => seen(entry => entry.event === "removed" && entry.tab === created.id), "the removal");

        const window = await chrome.windows.create({ url: directory + "sea.html", type: "popup", focused: true, left: 10, top: 20, width: 300, height: 200 });
        await eventually(() => seen(entry => entry.event === "focus" && entry.window === window.id), "the new window's focus");
        await chrome.windows.update(self.windowId, { focused: true });
        await eventually(() => seen(entry => entry.event === "focus" && entry.window === self.windowId), "the focus to come back");
        const windowTab = window.tabs[0].id;
        await eventually(() => seen(entry => entry.event === "updated" && entry.tab === windowTab && entry.change.status === "complete"), "the window's tab to load");
        await chrome.windows.remove(window.id);
        await eventually(() => seen(entry => entry.event === "window removed" && entry.window === window.id), "the window's removal");

        const mine = log.filter(entry => entry.tab === created.id || entry.window === window.id || entry.tab === windowTab);
        return {
            created: { pending: basename(created.pendingUrl), active: created.active, same_window: created.windowId === self.windowId, index: created.index },
            loaded: { url: basename(loaded.url), title: loaded.title, status: loaded.status },
            pinned: pinned.pinned,
            moved_index: moved.index,
            removed_gone: await failure(chrome.tabs.get(created.id)),
            window: { type: window.type, tabs: window.tabs.length, focused: window.focused, pending: basename(window.tabs[0].pendingUrl) },
            events: mine.map(entry => {
                if (entry.event !== "updated")
                    return entry.event + (entry.closing ? " closing" : "");
                return "updated " + Object.entries(entry.change).map(([key, value]) => `${key}=${typeof value === "string" ? basename(value) : value}`).join(",");
            }),
            focus_back_to_self: log.filter(entry => entry.event === "focus").at(-1).window === self.windowId,
            bad_create: await failure(chrome.tabs.create("x")),
            javascript_url: await failure(chrome.tabs.create({ url: "javascript:alert(1)" })),
            update_missing: await failure(chrome.tabs.update(99999, { pinned: true })),
            remove_bad: await failure(chrome.tabs.remove("x")),
            remove_missing: await failure(chrome.tabs.remove(99999)),
            window_remove_missing: await failure(chrome.windows.remove(99999)),
        };
    },

    // tabs.sendMessage and tabs.connect reach the extension's content scripts in a tab, by frame.
    async messages() {
        const self = await chrome.tabs.getCurrent();
        const bee = await tabNamed("bee.html");
        let home;
        chrome.runtime.onMessage.addListener((message, sender, respond) => {
            if (message.home) {
                home = { message: message.home, sender };
                respond("welcome");
            }
        });

        const frames = await chrome.webNavigation.getAllFrames({ tabId: bee.id });
        const child = frames.find(frame => frame.parentFrameId === 0);
        const top = await chrome.tabs.sendMessage(bee.id, "who", { frameId: 0 });
        const inner = await chrome.tabs.sendMessage(bee.id, "who", { frameId: child.frameId });
        const welcomed = await chrome.tabs.sendMessage(bee.id, "call home", { frameId: child.frameId });
        const byDocument = await chrome.tabs.sendMessage(bee.id, "who", { documentId: child.documentId });

        const port = chrome.tabs.connect(bee.id, { name: "lane", frameId: 0 });
        const echo = await new Promise(resolve => {
            port.onMessage.addListener(message => resolve(message));
            port.postMessage("ping");
        });
        port.disconnect();

        return {
            top: { file: basename(top.href), top: top.top, sender_id: top.sender_id, sender_url: top.sender_url === location.href, sender_has_tab: top.sender_has_tab },
            inner: { file: basename(inner.href), top: inner.top },
            by_document: basename(byDocument.href),
            later: await chrome.tabs.sendMessage(bee.id, "later", { frameId: 0 }),
            anyone: ["bee.html", "frame.html"].includes(basename((await chrome.tabs.sendMessage(bee.id, "who")).href)),
            welcomed,
            home: { file: basename(home.message.href), tab: home.sender.tab.id === bee.id, tab_url: basename(home.sender.tab.url), tab_title: home.sender.tab.title, frame: home.sender.frameId === child.frameId, url: basename(home.sender.url), document: home.sender.documentId === child.documentId, origin: home.sender.origin },
            echo: { echo: echo.echo, name: echo.name, top: echo.top },
            callback_style: await new Promise(resolve => chrome.tabs.sendMessage(bee.id, "who", { frameId: 0 }, response => resolve(basename(response.href)))),
            nobody: await failure(chrome.tabs.sendMessage(self.id, "who")),
            no_frame: await failure(chrome.tabs.sendMessage(bee.id, "who", { frameId: 12345 })),
            no_tab: await failure(chrome.tabs.sendMessage(99999, "who")),
            bad_tab: await failure(chrome.tabs.sendMessage("x", "who")),
        };
    },

    async scripting() {
        const self = await chrome.tabs.getCurrent();
        const bee = await tabNamed("bee.html");
        const frames = await chrome.webNavigation.getAllFrames({ tabId: bee.id });
        const child = frames.find(frame => frame.parentFrameId === 0);
        const run = (func, extra = {}) => chrome.scripting.executeScript({ target: { tabId: bee.id }, func, ...extra });
        const one = async (func, extra) => (await run(func, extra))[0].result;
        const color = () => one(() => getComputedStyle(document.body).color);

        const sum = await run((a, b) => a + b + document.title, { args: [1, 2] });
        const everywhere = await chrome.scripting.executeScript({ target: { tabId: bee.id, allFrames: true }, func: () => document.title });
        const inChild = await chrome.scripting.executeScript({ target: { tabId: bee.id, frameIds: [child.frameId] }, func: () => document.title });
        const byDocument = await chrome.scripting.executeScript({ target: { tabId: bee.id, documentIds: [child.documentId] }, func: () => document.title });

        await chrome.scripting.insertCSS({ target: { tabId: bee.id }, css: "body { color: rgb(1, 2, 3) }" });
        const styled = await color();
        await chrome.scripting.removeCSS({ target: { tabId: bee.id }, css: "body { color: rgb(1, 2, 3) }" });
        const unstyled = await color();
        await chrome.scripting.insertCSS({ target: { tabId: bee.id }, files: ["style.css"] });
        const styled_by_file = await color();
        await chrome.scripting.removeCSS({ target: { tabId: bee.id }, files: ["style.css"] });

        return {
            sum: { result: sum[0].result, frame: sum[0].frameId, document: typeof sum[0].documentId, count: sum.length },
            everywhere: everywhere.map(result => `${result.frameId === 0 ? "top" : result.frameId === child.frameId ? "child" : "?"}:${result.result}`),
            in_child: inChild.map(result => result.result),
            by_document: byDocument.map(result => result.result),
            isolated: await one(() => typeof window.pageGlobal),
            main: await one(() => typeof window.pageGlobal, { world: "MAIN" }),
            chrome_in_isolated: await one(() => typeof chrome.runtime.sendMessage),
            file: await one(undefined, { files: ["inject.js"] }),
            promise: await one(async () => { await null; return 7; }),
            nothing: await one(() => {}) ?? null,
            thrown: (await one(() => { throw new Error("boom"); })) ?? null,
            styled,
            unstyled,
            styled_by_file,
            restricted: await failure(chrome.scripting.executeScript({ target: { tabId: self.id }, func: () => 1 })),
            no_target: await failure(chrome.scripting.executeScript({ func: () => 1 })),
            no_code: await failure(chrome.scripting.executeScript({ target: { tabId: bee.id } })),
            missing_tab: await failure(chrome.scripting.executeScript({ target: { tabId: 99999 }, func: () => 1 })),
            missing_frame: await failure(chrome.scripting.executeScript({ target: { tabId: bee.id, frameIds: [12345] }, func: () => 1 })),
            missing_file: await failure(chrome.scripting.executeScript({ target: { tabId: bee.id }, files: ["nope.js"] })),
            bad_world: await failure(chrome.scripting.executeScript({ target: { tabId: bee.id }, func: () => 1, world: "OTHER" })),
            unserializable_args: await failure(chrome.scripting.executeScript({ target: { tabId: bee.id }, func: x => x, args: [() => 1] })),
            no_css: await failure(chrome.scripting.insertCSS({ target: { tabId: bee.id } })),
        };
    },

    async navigation() {
        const log = [];
        for (const event of ["onBeforeNavigate", "onCommitted", "onDOMContentLoaded", "onCompleted"])
            chrome.webNavigation[event].addListener(details => log.push({ event, ...details }));
        const sea = await tabNamed("sea.html");
        const mine = () => log.filter(entry => entry.tabId === sea.id);

        await chrome.tabs.update(sea.id, { url: directory + "nav.html" });
        await eventually(() => mine().some(entry => entry.event === "onCompleted" && entry.frameId === 0), "the top frame to complete");
        await eventually(() => mine().some(entry => entry.event === "onCompleted" && entry.frameId !== 0), "the child frame to complete");

        const frames = await chrome.webNavigation.getAllFrames({ tabId: sea.id });
        const top = await chrome.webNavigation.getFrame({ tabId: sea.id, frameId: 0 });
        const child = frames.find(frame => frame.frameId !== 0);
        const byDocument = await chrome.webNavigation.getFrame({ documentId: child.documentId });
        const sequence = frameIs => mine().filter(entry => frameIs(entry.frameId)).map(entry => entry.event);
        const committed = mine().find(entry => entry.event === "onCommitted" && entry.frameId === 0);
        return {
            top_sequence: sequence(id => id === 0),
            child_sequence: sequence(id => id !== 0),
            child_parent: mine().find(entry => entry.frameId !== 0).parentFrameId,
            top_parent: mine().find(entry => entry.frameId === 0).parentFrameId,
            urls: [...new Set(mine().map(entry => `${entry.frameId === 0 ? "top" : "child"}:${basename(entry.url)}`))],
            committed: { transition: typeof committed.transitionType, time: typeof committed.timeStamp, document: typeof committed.documentId, frame_type: committed.frameType },
            frames: frames.map(frame => `${frame.frameId === 0 ? "top" : "child"}:${frame.parentFrameId}:${basename(frame.url)}:${frame.errorOccurred}`),
            top: { url: basename(top.url), parent: top.parentFrameId, type: top.frameType, lifecycle: top.documentLifecycle },
            by_document: basename(byDocument.url),
            missing_frame: (await chrome.webNavigation.getFrame({ tabId: sea.id, frameId: 12345 })) ?? null,
            missing_tab: (await chrome.webNavigation.getAllFrames({ tabId: 99999 })) ?? null,
            bad_details: await failure(chrome.webNavigation.getAllFrames("x")),
        };
    },

    // An extension without "tabs" sees the URLs and titles of the tabs it has host access to, and no others.
    async limited() {
        const all = await chrome.tabs.query({});
        const id = name => Number(parameters.get(name));
        const self = await chrome.tabs.getCurrent();
        return {
            count: all.length,
            urls: all.map(tab => basename(tab.url) || null).sort(),
            titles: all.map(tab => tab.title ?? null).sort(),
            self_url: self.url ?? null,
            url_filter: titles(await chrome.tabs.query({ url: "file:///*" })),
            title_filter: titles(await chrome.tabs.query({ title: "*" })),
            allowed: (await chrome.scripting.executeScript({ target: { tabId: id("bee") }, func: () => document.title }))[0].result,
            own_page: await failure(chrome.scripting.executeScript({ target: { tabId: id("self") }, func: () => 1 })),
            child_frame: (await chrome.scripting.executeScript({ target: { tabId: id("bee"), allFrames: true }, func: () => document.title })).map(result => result.result),
            web_navigation: await failure(chrome.webNavigation.getAllFrames({ tabId: id("bee") })),
            others_content_scripts: await failure(chrome.tabs.sendMessage(id("bee"), "who")),
        };
    },
};

addEventListener("load", async () => {
    try {
        report(await cases[parameters.get("case")]());
    } catch (error) {
        report({ error: `${error.message} ${error.stack ?? ""}` });
    }
});
