// Runs in file:// test pages: what a content script may do with storage, and which storage events it hears.
(async () => {
    const report = {};
    // Fresh values, so each run changes what's stored.
    const nonce = Math.random();
    const heard = [];
    chrome.storage.onChanged.addListener((changes, area) => heard.push(`${area}:${Object.keys(changes).join()}`));
    chrome.storage.session.onChanged.addListener(changes => heard.push(`session-area:${Object.keys(changes).join()}`));
    const settle = () => new Promise(resolve => setTimeout(resolve, 300));
    const outcome = (promise, map = value => value) => promise.then(map, e => `error: ${e.message}`);

    report.local = await outcome(chrome.storage.local.set({ mine: nonce }).then(() => chrome.storage.local.get("mine")), items => items.mine === nonce ? 1 : 0);
    report.session_closed = await outcome(chrome.storage.session.get(null), items => JSON.stringify(items));
    report.session_set_closed = await outcome(chrome.storage.session.set({ from_content: 1 }), () => "ok");
    report.level_from_content = await outcome(chrome.storage.session.setAccessLevel({ accessLevel: "TRUSTED_AND_UNTRUSTED_CONTEXTS" }), () => "ok");
    const sender = await chrome.runtime.sendMessage({ do: "set_session", key: "hidden", value: nonce });
    report.sender_url = sender.url;
    report.sender_origin = sender.origin;
    report.sender_frame = sender.frameId;
    await settle();
    report.heard_closed = heard.join("|");

    report.opened = await chrome.runtime.sendMessage({ do: "open_session" });
    report.session_open = await outcome(chrome.storage.session.get("hidden"), items => items.hidden === nonce ? 1 : 0);
    report.level_from_content_open = await outcome(chrome.storage.session.setAccessLevel({ accessLevel: "TRUSTED_CONTEXTS" }), () => "ok");
    await chrome.runtime.sendMessage({ do: "set_session", key: "shown", value: nonce });
    await settle();
    report.heard_open = heard.join("|");

    await chrome.runtime.sendMessage({ do: "close_session" });
    report.session_reclosed = await outcome(chrome.storage.session.get(null), items => JSON.stringify(items));
    document.title = JSON.stringify(report);
})();
