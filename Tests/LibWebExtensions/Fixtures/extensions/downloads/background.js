// Notes the downloads events, where the test's page can read them.
let notes = Promise.resolve();
const note = (event, value) => {
    notes = notes.then(async () => {
        const { events = [] } = await chrome.storage.session.get("events");
        events.push([event, value]);
        await chrome.storage.session.set({ events });
    });
};
chrome.downloads.onCreated.addListener(item => note("created", item));
chrome.downloads.onChanged.addListener(delta => note("changed", delta));
chrome.downloads.onErased.addListener(id => note("erased", id));
