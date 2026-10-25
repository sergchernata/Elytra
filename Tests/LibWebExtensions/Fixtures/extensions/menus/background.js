// Notes the clicks, where the test's page can read them.
let notes = Promise.resolve();
chrome.contextMenus.onClicked.addListener((info, tab) => {
    notes = notes.then(async () => {
        const { clicks = [] } = await chrome.storage.session.get("clicks");
        clicks.push({ ...info, tab: tab?.id ?? null });
        await chrome.storage.session.set({ clicks });
    });
});
