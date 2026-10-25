chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    (async () => {
        const result = { url: sender.url, origin: sender.origin, frameId: sender.frameId };
        const outcome = promise => promise.then(() => "ok", e => e.message);
        if (message.do === "set_session")
            await chrome.storage.session.set({ [message.key]: message.value });
        if (message.do === "open_session") {
            result.local_level = await outcome(chrome.storage.local.setAccessLevel({ accessLevel: "TRUSTED_AND_UNTRUSTED_CONTEXTS" }));
            result.bad_level = await outcome(chrome.storage.session.setAccessLevel({ accessLevel: "EVERYONE" }));
            result.no_level = await outcome(chrome.storage.session.setAccessLevel());
            result.open = await outcome(chrome.storage.session.setAccessLevel({ accessLevel: "TRUSTED_AND_UNTRUSTED_CONTEXTS" }));
        }
        if (message.do === "close_session")
            result.close = await outcome(chrome.storage.session.setAccessLevel({ accessLevel: "TRUSTED_CONTEXTS" }));
        sendResponse(result);
    })();
    return true;
});
