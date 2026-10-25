// Notes what the user did with the notifications, where the test's page can read it.
// One at a time, so that no note overwrites another.
let notes = Promise.resolve();
const note = (key, value) => {
    notes = notes.then(async () => {
        const items = await chrome.storage.session.get(key);
        await chrome.storage.session.set({ [key]: [...(items[key] ?? []), value] });
    });
};
chrome.notifications.onClicked.addListener(id => note("clicked", id));
chrome.notifications.onButtonClicked.addListener((id, index) => note("button_clicked", [id, index]));
chrome.notifications.onClosed.addListener((id, byUser) => note("closed", [id, byUser]));
