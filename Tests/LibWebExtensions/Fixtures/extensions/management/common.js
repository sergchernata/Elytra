// What the test pages share: page.html?step=<name> runs that step and reports as JSON in the title.
const report = {};
const step = new URLSearchParams(location.search).get("step");
const attempt = async (name, fn) => {
    try {
        const value = await fn();
        report[name] = value === undefined ? "ok" : value;
    } catch (e) {
        report[name] = `error: ${e.message}`;
    }
};
const done = () => {
    document.title = JSON.stringify(report);
};
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
// The events the background recorded for this version that pass the filter, once there are at least `count`.
const events = async (count, filter = () => true) => {
    const key = `events_${chrome.runtime.getManifest().version}`;
    const recorded = async () => ((await chrome.storage.local.get(key))[key] || []).filter(filter);
    for (let tries = 0; tries < 100; ++tries) {
        const matching = await recorded();
        if (matching.length >= count)
            return matching;
        await sleep(50);
    }
    return recorded();
};
const named = (...names) => event => names.some(name => name in event);
const steps = {};
window.addEventListener("load", async () => {
    await (steps[step] || (async () => { report.error = `no step ${step}`; }))();
    done();
});

// What the background heard, once it has heard `count` events (from ?count=), and a moment more.
steps.lifecycle = async () => {
    await events(Number(new URLSearchParams(location.search).get("count") || 1));
    await sleep(300);
    report.events = JSON.stringify(await events(0));
};

// Privacy settings, as this extension sees them.
const privacy = chrome.privacy.services.passwordSavingEnabled;
const privacyState = async () => JSON.stringify(await privacy.get({}));
