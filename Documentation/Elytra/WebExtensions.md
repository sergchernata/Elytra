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
| `chrome-extension://` | LibURL, LibWebCommon, Fetch, ResourceLoader | Tuple origin (scheme, ID) for IDs only. Files are brokered by the browser process, which alone knows where extensions live: an extension's process gets all its files, others what's web accessible to them, nothing outside the extension's folder. Secure context. The manifest's CSP and Chrome's minimum are sent with each file. |
| Extension processes | `CanonicalNavigable::process_to_host` | Extension documents get processes of their own, as WebUI pages do. Only such a process may speak for the extension. |
| Bridge | `LibWeb/Extensions`, WebContent IPC | One bridge per extension context (page, background, content script world); JSON messages. |
| Content scripts | `LibWeb/Extensions/ExtensionContexts` | An `Extension` `WrapperWorld` per frame and extension: same DOM, own wrappers and globals. Injected at document_start, document_end and document_idle; MAIN world too. |
| `chrome.*` APIs | `Base/res/ladybird/extensions/chrome-api.js` over the bridge; `LibWebView/ExtensionRuntime` and `LibWebView/ExtensionAPI/*` | runtime (messaging, ports, native messaging, lifecycle), storage, tabs, windows, scripting, webNavigation, action, commands, alarms, idle, i18n, permissions, management, privacy. |
| Background | A hidden view per extension running its service worker script as a module | A real service worker can replace it later. |
| UI | `UI/Qt/Arc/Extensions.cpp` | Buttons in the sidebar's top strip, with badges; popups that fit their page; the Extensions window (File menu); keyboard commands; tabs and windows for chrome.tabs. |

## Tests

Unit tests in `Tests/LibWebExtensions`; end-to-end programs `Tests/LibWebView/TestExtension*.cpp`, which run the
extensions in `Tests/LibWebExtensions/Fixtures/extensions` in real WebContent processes; and the Arc self-test's
`extension_buttons_open_their_popups`.

## Not yet

- `webRequest` and `declarativeNetRequest` (1Password's static ruleset), `notifications`, `contextMenus`, `downloads`,
  `offscreen`, `scripting.registerContentScripts`, `activeTab`.
- 1Password's desktop helper checks the code signature of the browser that starts it, and knows only some browsers.
