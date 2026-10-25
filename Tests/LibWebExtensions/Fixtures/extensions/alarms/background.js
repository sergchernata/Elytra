// Notes the alarms it hears and, when asked, the idle states.
const heard = { fired: [], idle_states: [] };
const note = (what, value) => {
    heard[what].push(value);
    chrome.storage.session.set({ [what]: heard[what] });
};

chrome.runtime.onInstalled.addListener(() => chrome.storage.session.set({ ready: true }));
chrome.alarms.onAlarm.addListener(alarm => note("fired", alarm.name));

const onIdle = state => note("idle_states", state);
chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message === "listen to idle") {
        chrome.idle.setDetectionInterval(15);
        chrome.idle.onStateChanged.addListener(onIdle);
    } else if (message === "stop listening to idle") {
        chrome.idle.onStateChanged.removeListener(onIdle);
    }
    sendResponse(chrome.idle.onStateChanged.hasListeners());
});
