chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    sendResponse({ heard: message, tab: typeof sender.tab?.id, frameId: sender.frameId, url: sender.url });
});
