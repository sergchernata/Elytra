// Runs in the extension's isolated world, and hands the page what extensions commonly do: an event's detail, and an
// event handler on a shared node. The page (isolation.html in the test) tries to reach this world through them, and
// this script tries to reach the page's world through what the page hands back.
(() => {
    const stringify = JSON.stringify;
    const parse = JSON.parse;
    const report = {};
    let contentClicks = 0;

    const kindOf = value => {
        if (value === null || value === undefined)
            return String(value);
        if (typeof value !== "object" && typeof value !== "function")
            return typeof value;
        return value instanceof Object ? "own" : "foreign";
    };
    const escapeThrough = value => {
        try {
            return String(value.constructor.constructor("return typeof pageSecret")());
        } catch (e) {
            return "threw";
        }
    };

    // The page's objects, as they reach this world.
    document.addEventListener("to-content", event => {
        const detail = event.detail;
        report.page_detail = kindOf(detail);
        report.page_detail_value = detail && detail.nested && detail.nested.value;
        report.page_detail_escape = detail ? escapeThrough(detail) : "none";
    });
    document.addEventListener("to-content-function", event => {
        report.page_function_detail = kindOf(event.detail);
    });
    document.addEventListener("to-content-node", event => {
        report.page_node_detail = kindOf(event.detail);
        report.page_node_id = event.detail && event.detail.id;
    });
    document.addEventListener("page-done", () => {
        report.own_onclick = document.body.onclick === ownHandler ? "own" : kindOf(document.body.onclick);
        report.content_clicks = contentClicks;
        report.page_global = typeof window.pageSecret;
        document.documentElement.dataset.content = stringify(report);
    });

    const ownHandler = function () {
        contentClicks++;
    };
    window.isolatedMarker = "isolated";
    report.page_onclick = kindOf(document.body.onclick);
    document.body.onclick = ownHandler;
    document.dispatchEvent(new CustomEvent("from-content", { detail: { from: "content", nested: { value: 42 } } }));
    document.dispatchEvent(new CustomEvent("from-content-node", { detail: document.getElementById("target") }));

    // A page that got hold of this world could replace its builtins; chrome-api.js mustn't use them after it starts.
    const intercepted = [];
    JSON.parse = function (text) {
        intercepted.push("JSON.parse");
        return parse(text);
    };
    JSON.stringify = function (value) {
        intercepted.push("JSON.stringify");
        return stringify(value);
    };
    const realAssign = Object.assign;
    Object.assign = function (...args) {
        intercepted.push("Object.assign");
        return realAssign(...args);
    };
    const realFrom = Array.from;
    Array.from = function (...args) {
        intercepted.push("Array.from");
        return realFrom.apply(Array, args);
    };
    chrome.runtime.sendMessage("ping").then(response => {
        JSON.parse = parse;
        JSON.stringify = stringify;
        Object.assign = realAssign;
        Array.from = realFrom;
        report.background_echo = response && response.echo;
        report.intercepted = intercepted.join(",");
        document.dispatchEvent(new CustomEvent("content-ready"));
    });

    // Errors and rejections in this world are this world's: the page doesn't hear of them.
    setTimeout(() => {
        throw new TypeError("isolated-error");
    }, 0);
    Promise.reject(new TypeError("isolated-rejection"));
})();
