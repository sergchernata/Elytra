// The chrome.* API of a WebExtension context: an extension page, its background, or a content script's world.
//
// The browser implements the APIs. This script turns calls into messages to it, through the bridge the browser puts
// in globalThis.__extensionBridge (and which this script takes away again), and its replies and events back into
// promises, callbacks and listeners as Chrome has them.
//
// Messages to the browser, as JSON:
//   { type: "call", id, api: "tabs.query", args: [...] }               an API function; answered with "reply"
//   { type: "listen", event: "tabs.onUpdated", listener, extra }        the first listener, with any filter arguments
//   { type: "unlisten", event, listener }
//   { type: "message_response", message_id, response?, handled }        an answer to a runtime/tabs message
//   { type: "port_post", port_id, message } / { type: "port_disconnect", port_id }
//   { type: "blocking_response", blocking, response }                   a blocking webRequest listener's answer
// Messages from the browser:
//   { type: "reply", id, result? , error? }
//   { type: "event", event, args, listener? }                           to every listener, or only the one named
//   { type: "event", event, args, listener, blocking, async }           to a blocking listener, which answers
//   { type: "message", message_id, message, sender }                    runtime.onMessage
//   { type: "port_connect", port_id, name, sender }                     runtime.onConnect
//   { type: "port_message", port_id, message } / { type: "port_disconnected", port_id, error? }
(() => {
    "use strict";

    // The builtins this script uses, as they were when the context started. Script in the context (or a page that got
    // into a content script's world) may replace the globals later, and mustn't hear what the browser and the
    // extension tell each other. NB: Their prototypes' methods are still the realm's own.
    const { apply } = Reflect;
    const functionToString = Function.prototype.toString;
    const Object = (O => O.freeze({ assign: O.assign, defineProperty: O.defineProperty, entries: O.entries, freeze: O.freeze, keys: O.keys }))(globalThis.Object);
    const JSON = Object.freeze({ parse: globalThis.JSON.parse, stringify: globalThis.JSON.stringify });
    const Array = (A => Object.freeze({ from: A.from, isArray: A.isArray }))(globalThis.Array);
    const Number = (N => Object.freeze({ isInteger: N.isInteger }))(globalThis.Number);
    const { Error, Map, Promise, String, TypeError } = globalThis;

    const bridge = globalThis.__extensionBridge;
    delete globalThis.__extensionBridge;
    if (!bridge)
        return;

    const info = JSON.parse(bridge.info);
    const contextType = info.contextType; // "page", "background", "popup" or "content_script"
    const isContentScript = contextType === "content_script";
    const extensionId = info.extensionId;
    const manifest = info.manifest;

    const post = message => bridge.postMessage(JSON.stringify(message));

    // runtime.lastError, set while a callback runs after a failed call.
    let lastError;
    let lastErrorChecked = false;

    function withLastError(error, callback) {
        lastError = error === undefined ? undefined : { message: String(error) };
        lastErrorChecked = false;
        try {
            callback();
        } finally {
            if (lastError !== undefined && !lastErrorChecked)
                console.error(`Unchecked runtime.lastError: ${lastError.message}`);
            lastError = undefined;
        }
    }

    let nextId = 1;
    const pendingCalls = new Map();

    // Calls the browser. Like Chrome's functions, it returns a promise, or calls a callback passed last instead.
    function call(api, args) {
        args = Array.from(args);
        let callback;
        if (args.length && typeof args[args.length - 1] === "function")
            callback = args.pop();
        while (args.length && args[args.length - 1] === undefined)
            args.pop();

        const promise = new Promise((resolve, reject) => {
            const id = nextId++;
            pendingCalls.set(id, { resolve, reject });
            post({ type: "call", id, api, args: toJSON(args) });
        });
        if (!callback)
            return promise;
        promise.then(
            result => withLastError(undefined, () => callback(result)),
            error => withLastError(error.message, () => callback()));
    }

    // What can cross to the browser: JSON, as Chrome's messages are.
    function toJSON(value) {
        return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
    }

    function apiFunction(api) {
        return function () {
            return call(api, arguments);
        };
    }

    // Events. The browser hears of an event's first listener (with any filter arguments, as webRequest's), so it only
    // sends events someone listens to.
    const events = new Map();
    let nextListenerId = 1;

    class Event {
        constructor(name) {
            this._name = name;
            this._listeners = new Map();
            events.set(name, this);
        }
        addListener(callback, ...extra) {
            if (typeof callback !== "function")
                throw new TypeError("The listener must be a function");
            if (this._name.startsWith("webRequest."))
                checkRequestFilter(this._name, extra);
            if (this.hasListener(callback))
                return;
            const listener = nextListenerId++;
            this._listeners.set(listener, callback);
            post({ type: "listen", event: this._name, listener, extra: toJSON(extra) });
        }
        removeListener(callback) {
            for (const [listener, candidate] of this._listeners) {
                if (candidate === callback) {
                    this._listeners.delete(listener);
                    post({ type: "unlisten", event: this._name, listener });
                }
            }
        }
        hasListener(callback) {
            for (const candidate of this._listeners.values()) {
                if (candidate === callback)
                    return true;
            }
            return false;
        }
        hasListeners() {
            return this._listeners.size > 0;
        }
        // A blocking webRequest listener's answer (onAuthRequired): what it returns, resolves, or passes to the callback
        // it's given when "asyncBlocking".
        _dispatchBlocking(args, only, blocking, async) {
            const callback = this._listeners.get(only);
            const answer = response => post({ type: "blocking_response", blocking, response: toJSON(response) ?? null });
            if (!callback)
                return answer(null);
            let answered = false;
            const once = response => {
                if (answered)
                    return;
                answered = true;
                answer(response);
            };
            try {
                const result = callback(...args, ...(async ? [once] : []));
                if (result && typeof result.then === "function")
                    result.then(once, () => once(null));
                else if (!async || result !== undefined)
                    once(result);
            } catch (error) {
                console.error(`Error in event handler for ${this._name}:`, error);
                once(null);
            }
        }
        _dispatch(args, only) {
            const results = [];
            for (const [listener, callback] of Array.from(this._listeners)) {
                if (only !== undefined && only !== listener)
                    continue;
                try {
                    results.push(callback(...args));
                } catch (error) {
                    console.error(`Error in event handler for ${this._name}:`, error);
                }
            }
            return results;
        }
    }

    // webRequest listeners take a filter ({ urls, types?, tabId?, windowId? }) and what to include, as Chrome checks them.
    const resourceTypes = ["main_frame", "sub_frame", "stylesheet", "script", "image", "font", "object", "xmlhttprequest", "ping", "csp_report", "media", "websocket", "webtransport", "webbundle", "other"];
    const extraInfoSpecs = ["blocking", "asyncBlocking", "requestHeaders", "responseHeaders", "extraHeaders", "requestBody"];
    function checkRequestFilter(name, [filter, extraInfoSpec]) {
        const fail = why => { throw new TypeError(`Error in invocation of ${name}.addListener: ${why}`); };
        if (!filter || typeof filter !== "object")
            fail("No matching signature: a filter is required.");
        if (!Array.isArray(filter.urls) || !filter.urls.every(url => typeof url === "string"))
            fail("'urls' must be a list of match patterns.");
        if (filter.types !== undefined && (!Array.isArray(filter.types) || !filter.types.every(type => resourceTypes.includes(type))))
            fail("Invalid value for 'types'.");
        for (const key of ["tabId", "windowId"]) {
            if (filter[key] !== undefined && !Number.isInteger(filter[key]))
                fail(`Invalid value for '${key}'.`);
        }
        if (extraInfoSpec !== undefined && (!Array.isArray(extraInfoSpec) || !extraInfoSpec.every(spec => extraInfoSpecs.includes(spec))))
            fail("Invalid value for 'extraInfoSpec'.");
    }

    // A namespace of browser functions and events, plus what's given.
    function namespace(name, functions = [], eventNames = [], extra = {}) {
        const object = {};
        for (const fn of functions)
            object[fn] = apiFunction(`${name}.${fn}`);
        for (const event of eventNames)
            object[event] = new Event(`${name}.${event}`);
        return Object.assign(object, extra);
    }

    // A browser setting, as in chrome.privacy: get, set, clear and onChange.
    function setting(name) {
        return namespace(name, ["get", "set", "clear"], ["onChange"]);
    }

    // runtime messaging
    const ports = new Map();

    class Port {
        constructor(id, name, sender) {
            this._id = id;
            this.name = name;
            if (sender !== undefined)
                this.sender = sender;
            this.onMessage = new LocalEvent();
            this.onDisconnect = new LocalEvent();
            this._connected = true;
            ports.set(id, this);
        }
        postMessage(message) {
            if (!this._connected)
                throw new Error("Attempting to use a disconnected port object");
            post({ type: "port_post", port_id: this._id, message: toJSON(message) });
        }
        disconnect() {
            if (!this._connected)
                return;
            this._connected = false;
            ports.delete(this._id);
            post({ type: "port_disconnect", port_id: this._id });
        }
    }

    // An event that's only ever fired here, as a port's.
    class LocalEvent {
        constructor() {
            this._listeners = [];
        }
        addListener(callback) {
            if (!this._listeners.includes(callback))
                this._listeners.push(callback);
        }
        removeListener(callback) {
            this._listeners = this._listeners.filter(candidate => candidate !== callback);
        }
        hasListener(callback) {
            return this._listeners.includes(callback);
        }
        hasListeners() {
            return this._listeners.length > 0;
        }
        _dispatch(args) {
            for (const callback of Array.from(this._listeners)) {
                try {
                    callback(...args);
                } catch (error) {
                    console.error("Error in event handler:", error);
                }
            }
        }
    }

    let nextPortId = 1;

    function connect(api, args) {
        // runtime.connect([extensionId], [connectInfo]) and tabs.connect(tabId, [connectInfo]).
        args = Array.from(args);
        const connectInfo = args.length && typeof args[args.length - 1] === "object" ? args.pop() : {};
        const target = args.length ? args[0] : undefined;
        const portId = `${info.contextId}:${nextPortId++}`;
        const port = new Port(portId, (connectInfo && connectInfo.name) || "");
        post({ type: "call", id: nextId++, api, args: toJSON([target, connectInfo, portId]) });
        return port;
    }

    // sendMessage([extensionId|tabId], message, [options], [callback]). The browser answers with the first response.
    function sendMessage(api, args) {
        args = Array.from(args);
        let callback;
        if (args.length && typeof args[args.length - 1] === "function")
            callback = args.pop();
        let target;
        let message;
        let options;
        if (api === "tabs.sendMessage") {
            [target, message, options] = args;
        } else if (args.length >= 2 && (typeof args[0] === "string" || args[0] === null || args[0] === undefined) && (args.length === 3 || typeof args[1] !== "object" || args[1] === null || !isMessageOptions(args[1]))) {
            [target, message, options] = args;
        } else {
            [message, options] = args;
        }
        const forwarded = [target === null ? undefined : target, message === undefined ? null : message, options];
        if (callback)
            return call(api, [...forwarded, callback]);
        return call(api, forwarded);
    }

    function isMessageOptions(value) {
        const keys = Object.keys(value);
        return keys.length > 0 && keys.every(key => key === "includeTlsChannelId" || key === "frameId" || key === "documentId");
    }

    function dispatchMessage(message) {
        const event = events.get("runtime.onMessage");
        let responded = false;
        let async = false;
        const respond = response => {
            if (responded)
                return;
            responded = true;
            post({ type: "message_response", message_id: message.message_id, response: toJSON(response), handled: true });
        };
        if (event) {
            for (const result of event._dispatch([message.message, message.sender, respond])) {
                if (result === true)
                    async = true;
                else if (result && typeof result.then === "function") {
                    async = true;
                    result.then(respond, error => respond(undefined));
                }
            }
        }
        if (!responded && !async)
            post({ type: "message_response", message_id: message.message_id, handled: false });
    }

    bridge.setMessageHandler(text => {
        const message = JSON.parse(text);
        switch (message.type) {
        case "reply": {
            const pending = pendingCalls.get(message.id);
            if (!pending)
                return;
            pendingCalls.delete(message.id);
            if (message.error !== undefined)
                pending.reject(new Error(message.error));
            else
                pending.resolve(message.result);
            return;
        }
        case "event": {
            const event = events.get(message.event);
            if (message.blocking !== undefined) {
                if (event)
                    event._dispatchBlocking(message.args || [], message.listener, message.blocking, !!message.async);
                else
                    post({ type: "blocking_response", blocking: message.blocking, response: null });
            } else if (event) {
                event._dispatch(message.args || [], message.listener);
            }
            return;
        }
        case "message":
            dispatchMessage(message);
            return;
        case "port_connect": {
            const port = new Port(message.port_id, message.name, message.sender);
            const event = events.get("runtime.onConnect");
            if (event)
                event._dispatch([port]);
            return;
        }
        case "port_message": {
            const port = ports.get(message.port_id);
            if (port)
                port.onMessage._dispatch([message.message, port]);
            return;
        }
        case "port_disconnected": {
            const port = ports.get(message.port_id);
            if (!port)
                return;
            port._connected = false;
            ports.delete(message.port_id);
            withLastError(message.error, () => port.onDisconnect._dispatch([port]));
            return;
        }
        }
    });

    // scripting.executeScript(injection, [callback]): a func crosses to the browser as source, called with its args,
    // as { ...injection, js: "(<func>)(...<args>)" }.
    function executeScript(args) {
        args = Array.from(args);
        const callback = typeof args[args.length - 1] === "function" ? args.pop() : undefined;
        let injection = args[0];
        if (injection && typeof injection === "object" && typeof injection.func === "function") {
            const { func, args: funcArgs = [], ...rest } = injection;
            let serialized;
            if (Array.isArray(funcArgs) && funcArgs.every(arg => typeof arg !== "function" && typeof arg !== "symbol" && arg !== undefined))
                serialized = JSON.stringify(funcArgs);
            if (serialized === undefined) {
                const error = new Error("Error in invocation of scripting.executeScript: args must be an array of JSON-serializable values.");
                if (!callback)
                    return Promise.reject(error);
                withLastError(error.message, () => callback());
                return;
            }
            injection = { ...rest, js: `(${apply(functionToString, func, [])})(...${serialized})` };
        }
        return call("scripting.executeScript", callback ? [injection, callback] : [injection]);
    }

    // chrome.i18n.getMessage, which is synchronous, from the messages the browser chose for this context.
    function getMessage(name, substitutions) {
        if (typeof name !== "string")
            return "";
        const key = name.toLowerCase();
        if (key.startsWith("@@")) {
            const rtl = /^(ar|fa|he|iw|ur|yi|ps|sd|ug|dv)\b/.test(info.uiLocale || "");
            switch (key) {
            case "@@extension_id": return extensionId;
            case "@@ui_locale": return info.uiLocale || "en";
            case "@@bidi_dir": return rtl ? "rtl" : "ltr";
            case "@@bidi_reversed_dir": return rtl ? "ltr" : "rtl";
            case "@@bidi_start_edge": return rtl ? "right" : "left";
            case "@@bidi_end_edge": return rtl ? "left" : "right";
            }
            return "";
        }
        const entry = (info.messages || {})[key];
        if (!entry || typeof entry.message !== "string")
            return "";
        if (substitutions === undefined || substitutions === null)
            substitutions = [];
        else if (!Array.isArray(substitutions))
            substitutions = [substitutions];
        substitutions = substitutions.slice(0, 9).map(String);

        const placeholders = {};
        for (const [placeholder, value] of Object.entries(entry.placeholders || {}))
            placeholders[placeholder.toLowerCase()] = value && typeof value.content === "string" ? value.content : "";

        const numbered = text => text.replace(/\$(\$|[1-9])/g, (match, which) => which === "$" ? "$" : (substitutions[which - 1] ?? ""));
        return entry.message.replace(/\$([A-Za-z0-9_@]+)\$|\$\$|\$([1-9])/g, (match, placeholder, number) => {
            if (placeholder !== undefined) {
                const content = placeholders[placeholder.toLowerCase()];
                return content === undefined ? match : numbered(content);
            }
            if (match === "$$")
                return "$";
            return substitutions[number - 1] ?? "";
        });
    }

    // As in Chrome, native messaging is only there with its permission, and never in content scripts.
    const hasNativeMessaging = !isContentScript && Array.isArray(manifest.permissions) && manifest.permissions.includes("nativeMessaging");
    const runtime = namespace("runtime",
        isContentScript ? [] : ["getPlatformInfo", "getContexts", "openOptionsPage", "setUninstallURL", "requestUpdateCheck", "reload", "restart", ...(hasNativeMessaging ? ["sendNativeMessage"] : []), "getPackageDirectoryEntry"],
        isContentScript ? ["onMessage", "onConnect"] : ["onMessage", "onConnect", "onMessageExternal", "onConnectExternal", "onInstalled", "onStartup", "onSuspend", "onSuspendCanceled", "onUpdateAvailable", "onRestartRequired", "onUserScriptMessage"],
        {
            id: extensionId,
            getURL: path => `chrome-extension://${extensionId}/${String(path ?? "").replace(/^\/+/, "")}`,
            getManifest: () => JSON.parse(JSON.stringify(manifest)),
            sendMessage: function () { return sendMessage("runtime.sendMessage", arguments); },
            connect: function () { return connect("runtime.connect", arguments); },
            OnInstalledReason: { INSTALL: "install", UPDATE: "update", CHROME_UPDATE: "chrome_update", SHARED_MODULE_UPDATE: "shared_module_update" },
            OnRestartRequiredReason: { APP_UPDATE: "app_update", OS_UPDATE: "os_update", PERIODIC: "periodic" },
            PlatformOs: { MAC: "mac", WIN: "win", ANDROID: "android", CROS: "cros", LINUX: "linux", OPENBSD: "openbsd", FUCHSIA: "fuchsia" },
            PlatformArch: { ARM: "arm", ARM64: "arm64", X86_32: "x86-32", X86_64: "x86-64", MIPS: "mips", MIPS64: "mips64" },
            ContextType: { TAB: "TAB", POPUP: "POPUP", BACKGROUND: "BACKGROUND", OFFSCREEN_DOCUMENT: "OFFSCREEN_DOCUMENT", SIDE_PANEL: "SIDE_PANEL", DEVELOPER_TOOLS: "DEVELOPER_TOOLS" },
        });
    if (hasNativeMessaging)
        runtime.connectNative = function (application) {
            const portId = `${info.contextId}:${nextPortId++}`;
            const port = new Port(portId, "");
            post({ type: "call", id: nextId++, api: "runtime.connectNative", args: [String(application), portId] });
            return port;
        };
    Object.defineProperty(runtime, "lastError", {
        get() {
            lastErrorChecked = true;
            return lastError;
        },
        enumerable: true,
    });

    const i18n = namespace("i18n", ["getAcceptLanguages", "detectLanguage"], [], {
        getMessage,
        getUILanguage: () => (info.uiLocale || "en").replace(/_/g, "-"),
    });

    function storageArea(area, extraFunctions = []) {
        return namespace(`storage.${area}`, ["get", "set", "remove", "clear", "getBytesInUse", "getKeys", "setAccessLevel", ...extraFunctions], ["onChanged"]);
    }
    const storage = namespace("storage", [], ["onChanged"], {
        local: Object.assign(storageArea("local"), { QUOTA_BYTES: 10485760 }),
        sync: Object.assign(storageArea("sync"), { QUOTA_BYTES: 102400, QUOTA_BYTES_PER_ITEM: 8192, MAX_ITEMS: 512, MAX_WRITE_OPERATIONS_PER_HOUR: 1800, MAX_WRITE_OPERATIONS_PER_MINUTE: 120 }),
        session: Object.assign(storageArea("session"), { QUOTA_BYTES: 10485760 }),
        managed: namespace("storage.managed", ["get", "getBytesInUse", "getKeys"], ["onChanged"]),
        AccessLevel: { TRUSTED_CONTEXTS: "TRUSTED_CONTEXTS", TRUSTED_AND_UNTRUSTED_CONTEXTS: "TRUSTED_AND_UNTRUSTED_CONTEXTS" },
    });

    const extensionNamespace = {
        getURL: runtime.getURL,
        inIncognitoContext: false,
    };

    // As in Chrome, chrome.storage is there only with the "storage" permission (or the chance of it).
    const declares = key => Array.isArray(manifest[key]) && manifest[key].includes("storage");
    const chrome = { runtime, i18n, extension: extensionNamespace };
    if (declares("permissions") || declares("optional_permissions"))
        chrome.storage = storage;

    if (isContentScript) {
        // Closed shadow roots, which only content scripts may open.
        chrome.dom = { openOrClosedShadowRoot: element => bridge.openOrClosedShadowRoot(element) };
    } else {
        Object.assign(chrome, {
            action: namespace("action", ["setIcon", "setTitle", "getTitle", "setPopup", "getPopup", "setBadgeText", "getBadgeText", "setBadgeBackgroundColor", "getBadgeBackgroundColor", "setBadgeTextColor", "getBadgeTextColor", "enable", "disable", "isEnabled", "getUserSettings", "openPopup"], ["onClicked", "onUserSettingsChanged"]),
            alarms: namespace("alarms", ["create", "get", "getAll", "clear", "clearAll"], ["onAlarm"]),
            commands: namespace("commands", ["getAll"], ["onCommand"]),
            contextMenus: namespace("contextMenus", ["create", "update", "remove", "removeAll"], ["onClicked"], {
                // As in Chrome, create() returns the item's ID rather than a promise, and its callback hears of errors. An item
                // created without an ID gets one from the browser, which only onClicked tells of.
                create(properties, callback) {
                    call("contextMenus.create", [properties, typeof callback === "function" ? callback : () => {}]);
                    return properties?.id;
                },
                ACTION_MENU_TOP_LEVEL_LIMIT: 6,
                ContextType: { ALL: "all", PAGE: "page", FRAME: "frame", SELECTION: "selection", LINK: "link", EDITABLE: "editable", IMAGE: "image", VIDEO: "video", AUDIO: "audio", LAUNCHER: "launcher", BROWSER_ACTION: "browser_action", PAGE_ACTION: "page_action", ACTION: "action" },
                ItemType: { NORMAL: "normal", CHECKBOX: "checkbox", RADIO: "radio", SEPARATOR: "separator" },
            }),
            declarativeNetRequest: namespace("declarativeNetRequest", ["updateDynamicRules", "getDynamicRules", "updateSessionRules", "getSessionRules", "updateEnabledRulesets", "getEnabledRulesets", "getAvailableStaticRuleCount", "isRegexSupported", "getMatchedRules", "setExtensionActionOptions", "testMatchOutcome"], ["onRuleMatchedDebug"], {
                MAX_NUMBER_OF_DYNAMIC_RULES: 30000, MAX_NUMBER_OF_SESSION_RULES: 5000, GUARANTEED_MINIMUM_STATIC_RULES: 30000, DYNAMIC_RULESET_ID: "_dynamic", SESSION_RULESET_ID: "_session",
            }),
            downloads: namespace("downloads", ["download", "search", "pause", "resume", "cancel", "open", "show", "showDefaultFolder", "erase", "removeFile", "acceptDanger", "setUiOptions", "getFileIcon"], ["onCreated", "onErased", "onChanged", "onDeterminingFilename"]),
            idle: namespace("idle", ["queryState", "setDetectionInterval", "getAutoLockDelay"], ["onStateChanged"], { IdleState: { ACTIVE: "active", IDLE: "idle", LOCKED: "locked" } }),
            management: namespace("management", ["getAll", "get", "getSelf", "getPermissionWarningsById", "getPermissionWarningsByManifest", "setEnabled", "uninstall", "uninstallSelf", "launchApp", "createAppShortcut", "setLaunchType", "generateAppForLink"], ["onInstalled", "onUninstalled", "onEnabled", "onDisabled"]),
            notifications: namespace("notifications", ["create", "update", "clear", "getAll", "getPermissionLevel"], ["onClosed", "onClicked", "onButtonClicked", "onPermissionLevelChanged", "onShowSettings"], {
                TemplateType: { BASIC: "basic", IMAGE: "image", LIST: "list", PROGRESS: "progress" },
            }),
            offscreen: namespace("offscreen", ["createDocument", "closeDocument", "hasDocument"], [], {
                Reason: { CLIPBOARD: "CLIPBOARD", DOM_PARSER: "DOM_PARSER", DOM_SCRAPING: "DOM_SCRAPING", BLOBS: "BLOBS", WORKERS: "WORKERS", LOCAL_STORAGE: "LOCAL_STORAGE", IFRAME_SCRIPTING: "IFRAME_SCRIPTING", AUDIO_PLAYBACK: "AUDIO_PLAYBACK", USER_MEDIA: "USER_MEDIA", DISPLAY_MEDIA: "DISPLAY_MEDIA", WEB_RTC: "WEB_RTC", TESTING: "TESTING", MATCH_MEDIA: "MATCH_MEDIA", GEOLOCATION: "GEOLOCATION", BATTERY_STATUS: "BATTERY_STATUS" },
            }),
            permissions: namespace("permissions", ["contains", "getAll", "request", "remove", "addHostAccessRequest", "removeHostAccessRequest"], ["onAdded", "onRemoved"]),
            privacy: {
                network: { networkPredictionEnabled: setting("privacy.network.networkPredictionEnabled"), webRTCIPHandlingPolicy: setting("privacy.network.webRTCIPHandlingPolicy") },
                services: {
                    alternateErrorPagesEnabled: setting("privacy.services.alternateErrorPagesEnabled"),
                    autofillAddressEnabled: setting("privacy.services.autofillAddressEnabled"),
                    autofillCreditCardEnabled: setting("privacy.services.autofillCreditCardEnabled"),
                    autofillEnabled: setting("privacy.services.autofillEnabled"),
                    passwordSavingEnabled: setting("privacy.services.passwordSavingEnabled"),
                    safeBrowsingEnabled: setting("privacy.services.safeBrowsingEnabled"),
                    searchSuggestEnabled: setting("privacy.services.searchSuggestEnabled"),
                    spellingServiceEnabled: setting("privacy.services.spellingServiceEnabled"),
                    translationServiceEnabled: setting("privacy.services.translationServiceEnabled"),
                },
                websites: { thirdPartyCookiesAllowed: setting("privacy.websites.thirdPartyCookiesAllowed"), hyperlinkAuditingEnabled: setting("privacy.websites.hyperlinkAuditingEnabled"), referrersEnabled: setting("privacy.websites.referrersEnabled"), doNotTrackEnabled: setting("privacy.websites.doNotTrackEnabled"), protectedContentEnabled: setting("privacy.websites.protectedContentEnabled") },
            },
            scripting: namespace("scripting", ["executeScript", "insertCSS", "removeCSS", "registerContentScripts", "getRegisteredContentScripts", "unregisterContentScripts", "updateContentScripts"], [], {
                ExecutionWorld: { ISOLATED: "ISOLATED", MAIN: "MAIN" },
                executeScript: function () { return executeScript(arguments); },
            }),
            tabs: namespace("tabs", ["get", "getCurrent", "create", "duplicate", "query", "highlight", "update", "move", "reload", "remove", "group", "ungroup", "detectLanguage", "captureVisibleTab", "executeScript", "insertCSS", "removeCSS", "setZoom", "getZoom", "setZoomSettings", "getZoomSettings", "discard", "goForward", "goBack"], ["onCreated", "onUpdated", "onMoved", "onActivated", "onHighlighted", "onDetached", "onAttached", "onRemoved", "onReplaced", "onZoomChange"], {
                sendMessage: function () { return sendMessage("tabs.sendMessage", arguments); },
                connect: function () { return connect("tabs.connect", arguments); },
                TAB_ID_NONE: -1,
                TAB_INDEX_NONE: -1,
                MAX_CAPTURE_VISIBLE_TAB_CALLS_PER_SECOND: 2,
                TabStatus: { UNLOADED: "unloaded", LOADING: "loading", COMPLETE: "complete" },
                WindowType: { NORMAL: "normal", POPUP: "popup", PANEL: "panel", APP: "app", DEVTOOLS: "devtools" },
                MutedInfoReason: { USER: "user", CAPTURE: "capture", EXTENSION: "extension" },
            }),
            webNavigation: namespace("webNavigation", ["getFrame", "getAllFrames"], ["onBeforeNavigate", "onCommitted", "onDOMContentLoaded", "onCompleted", "onErrorOccurred", "onCreatedNavigationTarget", "onReferenceFragmentUpdated", "onTabReplaced", "onHistoryStateUpdated"]),
            webRequest: namespace("webRequest", ["handlerBehaviorChanged"], ["onBeforeRequest", "onBeforeSendHeaders", "onSendHeaders", "onHeadersReceived", "onAuthRequired", "onResponseStarted", "onBeforeRedirect", "onCompleted", "onErrorOccurred", "onActionIgnored"], {
                MAX_HANDLER_BEHAVIOR_CHANGED_CALLS_PER_10_MINUTES: 20,
            }),
            windows: namespace("windows", ["get", "getCurrent", "getLastFocused", "getAll", "create", "update", "remove"], ["onCreated", "onRemoved", "onFocusChanged", "onBoundsChanged"], {
                WINDOW_ID_NONE: -1,
                WINDOW_ID_CURRENT: -2,
                WindowType: { NORMAL: "normal", POPUP: "popup", PANEL: "panel", APP: "app", DEVTOOLS: "devtools" },
                WindowState: { NORMAL: "normal", MINIMIZED: "minimized", MAXIMIZED: "maximized", FULLSCREEN: "fullscreen", LOCKED_FULLSCREEN: "locked-fullscreen" },
                CreateType: { NORMAL: "normal", POPUP: "popup", PANEL: "panel" },
            }),
        });
        Object.assign(extensionNamespace, {
            getViews: () => [],
            getBackgroundPage: () => null,
            isAllowedIncognitoAccess: apiFunction("extension.isAllowedIncognitoAccess"),
            isAllowedFileSchemeAccess: apiFunction("extension.isAllowedFileSchemeAccess"),
        });
    }

    Object.defineProperty(globalThis, "chrome", { value: chrome, writable: true, configurable: true, enumerable: false });

    // An extension page tells the browser how big its document is, so a popup can fit it.
    if (contextType === "page" && globalThis.top === globalThis) {
        let reported = "";
        // The document's own size, as Chrome sizes popups: as wide as its content wants, and as tall as it then is,
        // whatever the window's size now.
        const report = () => {
            const root = document.documentElement;
            if (!root)
                return;
            const previous_width = root.style.width;
            root.style.width = "max-content";
            const box = root.getBoundingClientRect();
            root.style.width = previous_width;
            const width = Math.ceil(box.width);
            const height = Math.ceil(box.height);
            const size = `${width}x${height}`;
            if (size === reported)
                return;
            reported = size;
            post({ type: "document_size", width, height });
        };
        addEventListener("DOMContentLoaded", () => {
            report();
            new ResizeObserver(report).observe(document.documentElement);
            if (document.body)
                new ResizeObserver(report).observe(document.body);
        });
        addEventListener("load", report);
    }
})();
