// Talks to the native hosts the test set up, and reports what happened as JSON in the title. The query says which case
// to run (and, for some, where the host should note that it exited).
const parameters = new URLSearchParams(location.search);
const report = value => { document.title = JSON.stringify(value); };

// Opens a port, posts what's given, and gathers what comes back until `count` messages or a disconnect.
function exchange(host, messages, count) {
    return new Promise(resolve => {
        const port = chrome.runtime.connectNative(host);
        const got = [];
        port.onMessage.addListener(message => {
            got.push(message);
            if (got.length === count)
                resolve({ port, got });
        });
        port.onDisconnect.addListener(() => {
            resolve({ port, got, error: chrome.runtime.lastError ? chrome.runtime.lastError.message : "(none)" });
        });
        for (const message of messages)
            port.postMessage(message);
    });
}

const cases = {
    async echo() {
        const { port, got, error } = await exchange("com.ladybird.echo", [{ cmd: "info" }, { hello: [1, "two"] }], 2);
        port.disconnect();
        return { info: got[0], echo: got[1], error, has_name: port.name };
    },
    async second_directory() {
        const { port, got, error } = await exchange("com.ladybird.second", [{ hi: 1 }], 1);
        port.disconnect();
        return { echo: got[0], error };
    },
    async order() {
        const count = 300;
        const messages = [];
        for (let i = 0; i < count; ++i)
            messages.push({ seq: i, padding: "p".repeat(i * 10) });
        messages.push({ cmd: "burst", count });
        const { port, got, error } = await exchange("com.ladybird.echo", messages, count * 2);
        port.disconnect();
        let in_order = got.length === count * 2;
        for (let i = 0; in_order && i < count; ++i)
            in_order = got[i].echo.seq === i && got[i].echo.padding.length === i * 10 && got[count + i].burst === i;
        return { received: got.length, in_order, error };
    },
    async disconnect(host, messages = []) {
        const { got, error } = await exchange(host, messages, -1);
        return { error, received: got.length };
    },
    exits: () => cases.disconnect("com.ladybird.exit"),
    exits_later: () => cases.disconnect("com.ladybird.echo", [{ cmd: "exit" }]),
    garbage: () => cases.disconnect("com.ladybird.echo", [{ cmd: "garbage" }]),
    oversized: () => cases.disconnect("com.ladybird.echo", [{ cmd: "oversized" }]),
    forbidden: () => cases.disconnect("com.ladybird.forbidden"),
    missing: () => cases.disconnect("com.ladybird.missing"),
    invalid_name: () => cases.disconnect("../com.ladybird.echo"),
    broken: () => cases.disconnect("com.ladybird.broken"),
    async disconnect_ends_host() {
        const { port, got } = await exchange("com.ladybird.echo", [{ cmd: "note_exit", path: parameters.get("note") }], 1);
        port.disconnect();
        return { noted: got[0] };
    },
    async leaving_ends_host() {
        // The port stays open; the test navigates away.
        const { got } = await exchange("com.ladybird.echo", [{ cmd: "note_exit", path: parameters.get("note") }], 1);
        return { noted: got[0] };
    },
    async send_native_message() {
        const result = {};
        const attempt = async (name, fn) => {
            try {
                result[name] = await fn();
            } catch (e) {
                result[name] = `error: ${e.message}`;
            }
        };
        await attempt("echo", () => chrome.runtime.sendNativeMessage("com.ladybird.echo", { a: 1 }));
        await attempt("ended", () => chrome.runtime.sendNativeMessage("com.ladybird.echo", { cmd: "note_exit", path: parameters.get("note") }));
        await attempt("missing", () => chrome.runtime.sendNativeMessage("com.ladybird.missing", {}));
        await attempt("exits", () => chrome.runtime.sendNativeMessage("com.ladybird.exit", {}));
        await attempt("not_a_name", () => chrome.runtime.sendNativeMessage(42, {}));
        await attempt("callback", () => new Promise(resolve => chrome.runtime.sendNativeMessage("com.ladybird.forbidden", {}, response => {
            resolve(`${response}: ${chrome.runtime.lastError && chrome.runtime.lastError.message}`);
        })));
        return result;
    },
};

(async () => {
    const which = parameters.get("case");
    try {
        report(await cases[which]());
    } catch (e) {
        report({ exception: String(e) });
    }
})();
