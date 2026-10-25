// Runs the test named in the query and reports what it saw as JSON in the title.
const report = {};
const attempt = async (name, fn) => {
    try {
        report[name] = await fn();
    } catch (e) {
        report[name] = `error: ${e.message}`;
    }
};
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const waitFor = async condition => {
    for (let tries = 0; tries < 200 && !(await condition()); ++tries)
        await sleep(25);
};
const session = async key => (await chrome.storage.session.get(key))[key];

const tests = {
    // Chrome's 30-second minimum.
    async default_minimum() {
        const before = Date.now();
        await chrome.alarms.create("short", { delayInMinutes: 0.001 });
        const alarm = await chrome.alarms.get("short");
        report.delay = alarm.scheduledTime - before;
        report.cleared = await chrome.alarms.clear("short");
    },

    async basics() {
        const before = Date.now();
        await attempt("created", async () => String(await chrome.alarms.create("a", { delayInMinutes: 10 })));
        const a = await chrome.alarms.get("a");
        report.a_name = a.name;
        report.a_delay = Math.round((a.scheduledTime - before) / 1000);
        report.a_has_period = "periodInMinutes" in a;

        await chrome.alarms.create({ when: Date.now() + 3600000, periodInMinutes: 2 });
        const unnamed = await chrome.alarms.get();
        report.unnamed = unnamed ? `${JSON.stringify(unnamed.name)}:${unnamed.periodInMinutes}` : "missing";
        await attempt("periodic_only", async () => {
            await chrome.alarms.create("p", { periodInMinutes: 5 });
            const p = await chrome.alarms.get("p");
            return `${Math.round((p.scheduledTime - Date.now()) / 60000)}:${p.periodInMinutes}`;
        });

        // The same name replaces.
        await chrome.alarms.create("a", { delayInMinutes: 20 });
        report.replaced_delay = Math.round(((await chrome.alarms.get("a")).scheduledTime - before) / 60000);
        report.names = (await chrome.alarms.getAll()).map(alarm => alarm.name).sort().join(",");

        report.missing = String(await chrome.alarms.get("missing"));
        report.clear_missing = await chrome.alarms.clear("missing");
        report.clear_a = await chrome.alarms.clear("a");
        report.after_clear = String(await chrome.alarms.get("a"));

        await attempt("both_times", () => chrome.alarms.create("x", { when: Date.now() + 1000, delayInMinutes: 1 }));
        await attempt("no_time", () => chrome.alarms.create("x", {}));
        await attempt("no_info", () => chrome.alarms.create("x"));
        await attempt("bad_info", () => chrome.alarms.create("x", "soon"));
        await attempt("bad_name", () => chrome.alarms.create(5, { delayInMinutes: 1 }));
        await attempt("bad_delay", () => chrome.alarms.create("x", { delayInMinutes: "1" }));
        await attempt("bad_period", () => chrome.alarms.create("x", { delayInMinutes: 1, periodInMinutes: true }));
        await attempt("bad_get", () => chrome.alarms.get(5));
        await attempt("bad_clear", () => chrome.alarms.clear({}));
        report.x_exists = String(await chrome.alarms.get("x"));

        await attempt("callback", () => new Promise(resolve => chrome.alarms.clear("p", wasCleared => resolve(wasCleared))));
        report.clear_all = await chrome.alarms.clearAll();
        report.all_after_clear_all = (await chrome.alarms.getAll()).length;
    },

    // Alarms fire to the page and the background; periodic ones again, one-shots once.
    async firing() {
        await waitFor(() => session("ready"));
        const heard = [];
        let repeated;
        chrome.alarms.onAlarm.addListener(alarm => {
            heard.push(alarm.name);
            if (alarm.name === "repeat")
                repeated = alarm;
        });
        const before = Date.now();
        await chrome.alarms.create("once", { delayInMinutes: 0.002 });
        await chrome.alarms.create("when", { when: Date.now() + 100 });
        await chrome.alarms.create("repeat", { delayInMinutes: 0.002, periodInMinutes: 0.003 });
        await waitFor(() => heard.includes("once") && heard.includes("when") && heard.filter(name => name === "repeat").length >= 2);
        report.elapsed_enough = Date.now() - before >= 120 + 180;
        report.once_heard = heard.filter(name => name === "once").length;
        report.when_heard = heard.filter(name => name === "when").length;
        report.repeats = heard.filter(name => name === "repeat").length >= 2;
        report.repeat_event = repeated ? `${repeated.name}:${repeated.periodInMinutes}:${typeof repeated.scheduledTime}` : "none";
        report.once_after = String(await chrome.alarms.get("once"));
        report.repeat_after = String((await chrome.alarms.get("repeat"))?.periodInMinutes);
        await chrome.alarms.clear("repeat");
        await waitFor(async () => ((await session("fired")) || []).includes("once"));
        const fired = (await session("fired")) || [];
        report.background_heard = ["once", "when", "repeat"].filter(name => fired.includes(name)).join(",");
        const count = heard.length;
        await sleep(400);
        report.quiet_after_clear = heard.length === count;
    },

    async keep() {
        await chrome.alarms.create("kept", { delayInMinutes: 60 });
        report.names = (await chrome.alarms.getAll()).map(alarm => alarm.name).join(",");
    },

    async idle_query() {
        report.at_15 = await chrome.idle.queryState(15);
        report.at_60 = await chrome.idle.queryState(60);
        report.at_1 = await chrome.idle.queryState(1);
        await attempt("bad_query", () => chrome.idle.queryState("15"));
        await attempt("no_query", () => chrome.idle.queryState());
        await attempt("bad_interval", () => chrome.idle.setDetectionInterval("15"));
        report.auto_lock = await chrome.idle.getAutoLockDelay();
        report.constant = chrome.idle.IdleState.LOCKED;
    },

    async idle_listen() {
        await waitFor(() => session("ready"));
        report.listening = await chrome.runtime.sendMessage("listen to idle");
    },

    async idle_stop() {
        report.listening = await chrome.runtime.sendMessage("stop listening to idle");
    },

    async idle_events() {
        report.states = ((await session("idle_states")) || []).join(",");
    },

    async i18n() {
        report.accept = (await chrome.i18n.getAcceptLanguages()).join(",");
        report.japanese = JSON.stringify(await chrome.i18n.detectLanguage("\u3053\u3093\u306b\u3061\u306f\u3001\u4e16\u754c"));
        report.english = JSON.stringify(await chrome.i18n.detectLanguage("Hello there"));
        await attempt("bad_detect", () => chrome.i18n.detectLanguage(5));
        await attempt("callback", () => new Promise(resolve => chrome.i18n.getAcceptLanguages(languages => resolve(languages.length))));
    },
};

(async () => {
    const test = new URLSearchParams(location.search).get("test");
    await attempt("_", async () => { await tests[test](); return "done"; });
    document.title = JSON.stringify(report);
})();
