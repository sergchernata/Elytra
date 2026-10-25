# WebExtensions in Elytra

Goal: run Chrome Manifest V3 extensions, starting with 1Password, unmodified, installed from the Chrome Web Store.

## Pieces

| Piece | Where | Notes |
|---|---|---|
| Manifest, `_locales`, extension IDs | `Libraries/LibWebExtensions` | Plain data; unit tested. |
| Match patterns, globs | `LibWebExtensions/MatchPattern` | Which pages and frames content scripts run in. |
| `.crx` (CRX3) and ZIP | `LibWebExtensions/{Crx,Zip}` | Signatures verified; the ID comes from the signing key. |
| Native messaging | `LibWebExtensions/NativeMessaging` | Host manifests (Chrome's directories too), framing, origin check. |
| `chrome.storage` | `LibWebExtensions/StorageArea` | Chrome's semantics and quotas; local/sync saved per profile. |
| `chrome-extension://` | LibURL, LibWebCommon, Fetch, ResourceLoader | Tuple origin (scheme, ID). Files are brokered by the browser process, which alone knows where extensions live. Secure context. `web_accessible_resources` checked for other origins, in WebContent and again in the broker. Manifest CSP sent as a header. |
| Extension processes | `CanonicalNavigable::process_to_host` | Extension documents get their own WebContent processes, as WebUI pages do. |
| Bridge | WebContent ↔ browser IPC, like WebUI | One connection per extension document (and per content-script world), with request IDs for replies. |
| Content scripts | LibWeb, an `Extension` `WrapperWorld` per frame | Same DOM, own wrappers and globals. Injected at document_start (`set_ready_to_run_scripts`), document_end and document_idle (`update_readiness`). |
| `chrome.*` APIs | JS shim in each extension context over the bridge; implemented in the browser process | tabs, windows, runtime, storage, scripting, webNavigation, notifications, contextMenus, alarms, idle, i18n, permissions, action, commands, … |
| Background | A hidden view per extension running the service worker script | A real service worker can replace it later. |
| UI | `UI/Qt` and `UI/Qt/Arc` | Toolbar button and popup, install from the Web Store, extension settings, keyboard commands. |

## Tests

Each layer has tests written before the code: unit tests in `Tests/LibWebExtensions`, LibURL and LibWeb tests for the
scheme and worlds, and test extensions under `Tests/LibWebExtensions/Fixtures` driven through the browser.
