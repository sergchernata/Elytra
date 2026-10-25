// Content scripts get no native messaging.
document.title = JSON.stringify({
    connect_native: typeof chrome.runtime.connectNative,
    send_native_message: typeof chrome.runtime.sendNativeMessage,
});
