// Notes what it hears: it has webRequest, but not webRequestAuthProvider, and no host access to localhost.
const heard = { urls: [], auth: 0, ids: [] };
const save = () => chrome.storage.session.set({ heard });

chrome.webRequest.onBeforeRequest.addListener(details => {
    heard.urls.push(details.url);
    if (details.url.includes("rid-"))
        heard.ids.push(details.requestId);
    save();
}, { urls: ["<all_urls>"] });

// Without webRequestAuthProvider it's never asked; if it were, it would cancel.
chrome.webRequest.onAuthRequired.addListener(() => {
    heard.auth++;
    save();
    return { cancel: true };
}, { urls: ["<all_urls>"] }, ["blocking"]);

chrome.runtime.onInstalled.addListener(async () => {
    await chrome.declarativeNetRequest.updateDynamicRules({
        addRules: [{ id: 1, action: { type: "block" }, condition: { urlFilter: "other-blocked" } }],
    });
    await save();
    await chrome.storage.session.set({ ready: true });
});
