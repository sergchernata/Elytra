// Runs in the extension's isolated world on a page (isolation-engine.html in the test) that replaced its own world's
// builtins to catch what the engine might hand it of this world: callbacks passed to them would lead it to chrome.*.
// This script uses the engine's observers, promises and stored values the usual way, and reports whether what it got
// is its own world's.
(async () => {
    const report = {};
    const own = value => value instanceof Object;
    const withTimeout = (promise, name) =>
        Promise.race([promise, new Promise(resolve => setTimeout(() => resolve(name + ":timeout"), 3000))]);
    const settle = async promise => {
        try {
            return { value: await promise };
        } catch (error) {
            return { error };
        }
    };
    const box = document.getElementById("box");

    // Observers: their records, entries and the nodes in them are this world's.
    report.mutation = await withTimeout(new Promise(resolve => {
        const observer = new MutationObserver((records, observerArgument) => {
            const result = [Array.isArray(records) && own(records), records[0] instanceof MutationRecord,
                records[0].target instanceof Node, observerArgument === observer];
            records.forEach(() => {});
            records[0].addedNodes.forEach(node => result.push(node instanceof HTMLSpanElement));
            records[0].target.addEventListener("x", () => {});
            observer.disconnect();
            resolve(result.join());
        });
        observer.observe(document.body, { childList: true });
        document.body.appendChild(document.createElement("span"));
    }), "mutation");
    report.resize = await withTimeout(new Promise(resolve => {
        const observer = new ResizeObserver(entries => {
            const result = [own(entries), entries[0] instanceof ResizeObserverEntry, entries[0].target === box];
            entries.forEach(() => {});
            observer.disconnect();
            resolve(result.join());
        });
        observer.observe(box);
    }), "resize");
    report.intersection = await withTimeout(new Promise(resolve => {
        const observer = new IntersectionObserver(entries => {
            const result = [own(entries), entries[0] instanceof IntersectionObserverEntry, entries[0].target === box];
            entries.forEach(() => {});
            observer.disconnect();
            resolve(result.join());
        });
        observer.observe(box);
    }), "intersection");
    report.performance = await withTimeout(new Promise(resolve => {
        const observer = new PerformanceObserver((list, observerArgument) => {
            const entries = list.getEntries();
            const result = [own(list), own(entries), entries[0] instanceof PerformanceMark, observerArgument === observer];
            entries.forEach(() => {});
            observer.disconnect();
            resolve(result.join());
        });
        observer.observe({ entryTypes: ["mark"] });
        performance.mark("isolated-mark");
    }), "performance");
    report.animation_frame = await withTimeout(new Promise(resolve => requestAnimationFrame(time => resolve(typeof time))), "animation_frame");

    // Promises the engine makes are this world's, and so is what they settle with.
    const promises = {
        replace: () => new CSSStyleSheet().replace("a { color: red }"),
        fonts: () => document.fonts.ready,
        finished: () => box.animate([{ opacity: 0 }, { opacity: 1 }], 10).finished,
        decode: () => document.createElement("img").decode(),
        estimate: () => navigator.storage.estimate(),
        databases: () => indexedDB.databases(),
        json: () => new Response('{"list":[1]}').json(),
        read: () => new Response("x").body.getReader().read(),
    };
    for (const [name, make] of Object.entries(promises)) {
        let promise;
        try {
            promise = make();
        } catch (error) {
            report[name] = "threw:" + error;
            continue;
        }
        const outcome = await withTimeout(settle(promise), name);
        const settled = typeof outcome === "string" ? outcome
            : "error" in outcome ? "rejected:" + own(outcome.error)
            : "fulfilled:" + (outcome.value === undefined || outcome.value === null || typeof outcome.value !== "object" || own(outcome.value));
        report[name] = (promise instanceof Promise) + "," + settled;
    }
    report.json_list = await new Response('{"list":[1]}').json().then(value => value.list instanceof Array);
    report.read_value = await new Response("x").body.getReader().read().then(result => result.value instanceof Uint8Array);

    // What a stored value hands back is this world's.
    const pageSignal = await new Promise(resolve => {
        document.addEventListener("page-signal", event => resolve(event.detail), { once: true });
        document.dispatchEvent(new CustomEvent("content-wants-signal"));
    });
    try {
        pageSignal.throwIfAborted();
        report.page_abort = "none";
    } catch (error) {
        report.page_abort = own(error) + "," + error.why;
    }
    const controller = new AbortController();
    controller.abort({ why: "content" });
    const filter = () => NodeFilter.FILTER_ACCEPT;
    const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_ALL, filter);
    report.own_filter = walker.filter === filter;

    // frames[0] (and the frame by name) is this world's WindowProxy of the frame.
    report.frame = [window.frames.length, own(window.frames[0]), own(window.child), window.frames[0] === window.child].join();

    // window.opener set here stays here.
    window.opener = { secret: "content" };
    report.opener = window.opener && window.opener.secret;

    // A string handler runs in this world.
    setTimeout("document.documentElement.dataset.timer = typeof chrome === 'object' && chrome.runtime ? 'isolated' : 'main'", 0);
    await new Promise(resolve => setTimeout(resolve, 50));

    document.documentElement.dataset.engine = JSON.stringify(report);
    document.dispatchEvent(new CustomEvent("content-done", { detail: [controller.signal, walker] }));
})();
