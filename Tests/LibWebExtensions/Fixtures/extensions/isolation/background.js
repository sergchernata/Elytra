chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    sendResponse({ echo: message, secret: "background-secret" });
});
