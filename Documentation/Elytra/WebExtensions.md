# WebExtensions in Elytra

Goal: run Chrome Manifest V3 extensions, starting with 1Password, unmodified, installed from the Chrome Web Store.

## Pieces

| Piece | Where | Notes |
|---|---|---|
| Manifest, `_locales`, extension IDs | `Libraries/LibWebExtensions` | Plain data; unit tested. |
| Match patterns, globs | `LibWebExtensions/MatchPattern` | Which pages and frames content scripts run in. |
| `.crx` (CRX3) and ZIP | `LibWebExtensions/{Crx,Zip}` | Signatures verified; the ID comes from the signing key. |
| Native messaging | `LibWebExtensions/NativeMessaging` | Host manifests (Chrome's directories too), framing, origin check, hosts run without blocking the UI. |
| `chrome.storage` | `LibWebExtensions/StorageArea` | Chrome's semantics and quotas; local/sync saved per profile. |
| Installing | `LibWebView/ExtensionRegistry` | From the Web Store (or a `.crx`), or a folder; remembered in `<profile>/Extensions/Installed.json`; updates through Google's update service. |
| `chrome-extension://` | LibURL, LibWebCommon, Fetch, ResourceLoader | Tuple origin (scheme, ID) for IDs only. Files are brokered by the browser process, which alone knows where extensions live: an extension's process gets all its files, others what's web accessible to them, nothing outside the extension's folder. Secure context. The manifest's CSP and Chrome's minimum are sent with each file. As in Chrome, no document's CSP holds back an extension's files (1Password's `connect-src` lists only its servers, yet its background fetches its own files); the browser still decides which a document may have. |
| Extension processes | `CanonicalNavigable::process_to_host` | Extension documents get processes of their own, as WebUI pages do. Only such a process may speak for the extension. As in Chrome, its pages and background read what the extension's host permissions cover without CORS: the browser tells only that process the patterns (`hostAccess`), again when permissions change; content scripts' requests are their page's, and don't. |
| Bridge | `LibWeb/Extensions`, WebContent IPC | One bridge per extension context (page, background, content script world); JSON messages. |
| Content scripts | `LibWeb/Extensions/ExtensionContexts` | An `Extension` `WrapperWorld` per frame and extension: same DOM, own wrappers and globals. Injected at document_start, document_end and document_idle; MAIN world too. |
| `chrome.*` APIs | `Base/res/ladybird/extensions/chrome-api.js` over the bridge; `LibWebView/ExtensionRuntime` and `LibWebView/ExtensionAPI/*` | runtime (messaging, ports, native messaging, lifecycle), storage, tabs, windows, scripting, webNavigation, action, commands, alarms, idle, i18n, permissions, management, privacy. |
| `webRequest`, `declarativeNetRequest` | Rules in `LibWebExtensions/DeclarativeNetRequest` (unit tested); the browser's side in `LibWebView/ExtensionAPI/WebRequest.cpp`; the page side in `LibWeb/Extensions/WebRequests`, which Fetch calls | The browser tells each page which webRequest events are listened to and what the rules are (static rulesets, dynamic rules kept per profile, session rules; tab-only session rules go to their tabs' pages). The page applies the rules to its HTTP(S) requests (block, redirect as an internal 307, upgradeScheme, allow, allowAllRequests, request and response headers before CORS) and reports the events, which the browser sends to the listeners whose filter matches and whose extension has `webRequest` and host access to the URL and initiator; never another extension's requests. onAuthRequired's blocking listeners (with `webRequestAuthProvider`) answer Basic challenges before any prompt; the credentials are remembered, in memory, for the top-level site, origin and realm. Request IDs are the browser's. A page is told only the rules that can act, under names of its own (an extension's ID only for its own pages, or rules that redirect to its pages), with host permissions only where rules need them; it's told again only when that changes. Rules refuse header values that aren't header values, urlFilters over 2048 characters, and redirects anywhere but http(s), ws(s), data: and the extension's own pages; urlFilters match in linear time; redirect and modifyHeaders need access to every host for an opaque initiator. Extensions don't run in private windows (as Chrome without "Allow in Incognito", which Elytra has no setting for): no content scripts, events, rules, contexts, extension pages or context-menu items there. |
| Background | A hidden view per extension running its service worker script as a module | A real service worker can replace it later. |
| UI | `UI/Qt/Arc/Extensions.cpp` | Buttons in each tab's toolbar, before the ad blocker's, with badges; popups that fit their page; the Extensions window (File menu); keyboard commands; tabs and windows for chrome.tabs. |

## Tests

Unit tests in `Tests/LibWebExtensions`; end-to-end programs `Tests/LibWebView/TestExtension*.cpp`, which run the
extensions in `Tests/LibWebExtensions/Fixtures/extensions` in real WebContent processes; and the Arc self-test's
`extension_buttons_open_their_popups`.

## Not yet

- `offscreen`, `scripting.registerContentScripts`, `activeTab`; incognito access for extensions the user allows (none run in private windows).
- webRequest and declarativeNetRequest don't see workers' requests, WebSockets, responses Fetch's own memory cache
  serves, or requests the browser process makes; blocking webRequest listeners other than onAuthRequired's (MV3 has
  none); `getMatchedRules` (always empty), `testMatchOutcome`, `updateStaticRules`, response-header conditions; rules
  apply in the page process, so a compromised one can ignore them. The browser checks what a page reports against
  what it hosts (an initiator must be a document of the page; a navigation's URL the one the browser started, or a
  redirect of it the page reported; challenges only for a site of the page's documents or navigations, or an extension's
  own requests in its own process), but can't see subresource requests: a compromised page can still make up events,
  and challenges, for URLs of its own sites. Credentials an extension gives go to that page process, not RequestServer
  (whose auth is libcurl's, with no hook to ask the browser), as Ladybird's own prompt's do. Rule updates aren't
  rate-limited (each costs a serialization per changed page). Session-rule `tabIds` are applied by the browser to
  whole pages.
- 1Password's desktop helper checks the code signature of the browser that starts it, and knows only some browsers.
