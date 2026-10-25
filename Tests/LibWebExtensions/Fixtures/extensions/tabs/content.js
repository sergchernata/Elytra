// Answers the extension's messages and ports, from each frame of the pages it runs in.
const where = () => ({ href: location.href, top: window === top });

chrome.runtime.onMessage.addListener((message, sender, respond) => {
    if (message === "who")
        respond({ ...where(), sender_id: sender.id, sender_url: sender.url, sender_has_tab: "tab" in sender });
    if (message === "later") {
        setTimeout(() => respond({ later: true, ...where() }), 20);
        return true;
    }
    if (message === "call home") {
        chrome.runtime.sendMessage({ home: where() }).then(respond);
        return true;
    }
});

chrome.runtime.onConnect.addListener(port => {
    port.onMessage.addListener(message => port.postMessage({ echo: message, name: port.name, ...where() }));
});
