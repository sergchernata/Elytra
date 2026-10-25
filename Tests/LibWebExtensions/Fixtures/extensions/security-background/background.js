chrome.runtime.onInstalled.addListener(() => {
    chrome.storage.session.set({ secret: "session-secret" });
    chrome.storage.local.set({ vault: "local-secret" });
});
// Whoever reaches the background is written down.
chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    chrome.storage.local.set({ last_sender: sender.url ?? "(no url)" });
    sendResponse({ url: sender.url });
});
