// Notes the alarms it hears.
const fired = [];
chrome.alarms.onAlarm.addListener(alarm => {
    fired.push(alarm.name);
    chrome.storage.session.set({ fired });
});
