// A module service worker: answers messages and ports, and notes how it was installed.
import { describe } from "./helpers.js";

chrome.runtime.onInstalled.addListener(details => {
    chrome.storage.local.set({ installed: details.reason, background_url: location.href });
});

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.kind === "echo") {
        sendResponse({ echo: message.value, sender: describe(sender) });
        return;
    }
    if (message.kind === "later") {
        setTimeout(() => sendResponse({ later: true }), 10);
        return true;
    }
    if (message.kind === "promise")
        return Promise.resolve({ promised: true });
    // Anything else goes unanswered.
});

chrome.runtime.onConnect.addListener(port => {
    port.onMessage.addListener(message => {
        if (message === "bye") {
            port.disconnect();
            return;
        }
        port.postMessage({ name: port.name, got: message });
    });
});
