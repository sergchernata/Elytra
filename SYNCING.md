# Arc fork: staying in sync with upstream Ladybird

This branch (`arc`) adds an Arc-style UI on top of Ladybird's Qt frontend.
All Arc code lives in `UI/Qt/Arc/`. Upstream files carry only small hooks, each marked
`fork-only` or `Fork hook (Arc)`:

| File | Hook |
| --- | --- |
| `UI/Qt/CMakeLists.txt` | `include(Arc/Arc.cmake)` after the main `target_sources` block |
| `UI/Qt/main.cpp` | `#include <UI/Qt/Arc/ArcChrome.h>` |
| `UI/Qt/main.cpp` | `Ladybird::Arc::install();` just before the first `app->new_window(...)` |
| `UI/Qt/TabBar.h` | `TabWidget::set_vertical_sidebar_widget()`, the `tabs_changed()` signal, and `m_vertical_sidebar_widget` |
| `UI/Qt/TabBar.cpp` | `set_vertical_sidebar_widget()`; `emit tabs_changed()` at the end of `insert_tab()` and `take_tab()`; the sidebar branch at the top of `rebuild_layout_for_vertical_tabs()` and `rebuild_layout_for_horizontal_tabs()`; the Arc layout branch in `rebuild_layout()` (full-height sidebar, toolbar above the page); `m_vertical_sidebar_widget` checks in `vertical_tabs_chrome_rect()`, `should_show_window_controls_in_tab_toolbar()` and `update_toolbar_placement()` |
| `UI/Qt/TabBar.h/.cpp` (hidden sidebar) | `collapsed_vertical_tabs_width()` (a thin hover edge instead of an icon column), `vertical_sidebar_expanded_width()`, upstream's hover-expand switched off in `can_expand_vertical_tabs_on_hover()`, `on_vertical_sidebar_expanded_will_change` (called at the top of `set_vertical_tabs_expanded()`), and `set_vertical_sidebar_reserved_width()` with its checks at the top of `vertical_tabs_layout_width()` and `current_vertical_tabs_width()` (lets the dock/hide animation move the page edge, and the sidebar column with it) |
| `UI/Qt/TabBar.h/.cpp` (Little Arc) | `set_minimal_chrome_bar()` and `m_minimal_chrome_bar`: the branch at the top of `rebuild_layout()` (only that bar above the page), and checks in `update_web_view_frame()`, `update_vertical_tabs_overlay_geometry()`, `update_vertical_tabs_resize_handle()` and the content separator's visibility |
| `Tests/UI/Qt/CMakeLists.txt` | `include(Arc/ArcTests.cmake)` just before the `if (NOT LINUX) return()` near the end |
| `UI/Qt/ChromeStyle.h/.cpp` | `set_chrome_background_tint()`, and `chrome_background()` applying it (the space color across the window frame) |
| `UI/Qt/Tab.h/.cpp`, `BrowserWindow.cpp`, `Application.cpp` (returning tabs) | `Tab::create_returning_view`, wrapped around making the view of a tab that comes back: `Tab::ensure_loaded()` (a deferred restored tab), `BrowserWindow::create_restored_tab()` (one restored with its view) and `Application::reopen_recently_closed_tab()`. The sidebar makes it in the tab's own profile, so its page never loads with another profile's cookies |
| `UI/Qt/Tab.h/.cpp` (toolbar margin) | `Tab::toolbar_left_margin`, used where the toolbar's margins are set (constructor, `set_toolbar_window_controls_visible()`): the Arc toolbar's left margin from the start, so its buttons never jump |
| `UI/Qt/Tab.h/.cpp` (spinner) | `Tab::animates_loading_spinner`, checked in `set_loading()`: the Arc sidebar shows no spinner, so the 80 ms timer stays off |
| `UI/Qt/Tab.h/.cpp` (page corner) | `Tab::sidebar_is_chrome_colored`, checked in `paintEvent()`: the Arc sidebar is the chrome's color, so the page frame's bottom corner isn't painted the tab strip's |
| `UI/Qt/TabBar.cpp` (sheen) | `play_load_sheen()` returns early while the tab bar is hidden |
| `UI/Qt/Tab.h/.cpp` | `Tab::on_focus_location_editor`, checked at the top of `focus_location_editor()` (Cmd+L and upstream's other routes into the location field open the command bar instead; the field itself stays, hidden behind Arc's address bar) |
| `UI/Qt/Icon.cpp` | `create_chrome_icon()` caches what it draws (`draw_chrome_icon()` is upstream's body): every new tab builds a bookmarks-bar menu with an icon per bookmark, which took seconds with a large imported library and froze the first visit to each space |
| `UI/Qt/BrowserWindow.h/.cpp` | `on_tab_will_close`, called in `definitely_close_tab()` before the tab is removed (the sidebar picks the next tab from the same space, or leaves an empty page so closing a space's last tab doesn't close the window), with the early returns around it for a tab already gone; `is_popup_window()` (popups get no sidebar) |

### Profiles (LibWebView)

Arc profiles keep separate cookies and site data in one window. Upstream picks a tab's browsing session by
private-or-not; these hooks carry the session itself instead. All are marked `Fork hook (Arc profiles)`.

| File | Hook |
| --- | --- |
| `BrowsingSession.h/.cpp` | `create_persistent()` (a profile's own cookie, storage and HSTS database, and its own History database for history and favicons), `history_database_directory` (for the omnibox's connection) and `records()` (only tab restore stays the default session's: windows hold tabs of several profiles) |
| `Application.h/.cpp` | `set_session_for_new_views()`, `session_for_new_web_view()`, `did_create_profile_session()`, `request_server_client(BrowsingSession&)`; `create_web_content_client()`, `open_page_for_new_tab()` and `launch_child_frame_web_content_process()` take a session (the spare process is only the default session's); `clear_browsing_data()` clears profiles' site data and history too; the default session's `history_database_directory` is set where its History database opens |
| `ViewImplementation.cpp` | the session comes from `session_for_new_web_view()`, follows the process showing the view (popups), and tab restore goes through `records()`; a profile's favicons are also added to the default session's store (bookmarks look icons up there by hash); context-menu downloads pass the session |
| `WebContentClient.h/.cpp` | constructed with a session (the private-or-not constructor stays, forwarding) |
| `HelperProcess.h/.cpp` | `launch_web_content_process()` and `launch_web_worker_process()` take a session |
| `WebWorkerClient.h/.cpp`, `WorkerProcessManager.h/.cpp` | workers take their owner's session; shared-worker keys, per-site worker counts and BroadcastChannel delivery are partitioned by session |
| `FileDownloader.h/.cpp` | downloads remember the session they started in (`m_download_sessions`, kept while the download is listed) for their requests, and fail rather than fall back to the default session once it's gone; a profile's downloads aren't persisted for resuming after a restart (`belongs_to_a_profile_session()`), since the record can't say whose cookies to use |
| `CanonicalNavigable.cpp`, `CanonicalTraversable.cpp` | child-frame processes get the page's session |
| `WebContentPage.cpp` | downloads and BroadcastChannel by session (history and favicons are the session's own, as upstream has them) |
| `CookieJar.h/.cpp` | `TransientStorage::owner`: cookie-change notifications go only to views using that jar |
| `RequestServerManager.cpp` | profile sessions (`is_profile_session()`) keep no HTTP disk cache, like private ones |
| `Autocomplete.cpp` | remote suggestions use `session_for_new_web_view()`'s cookies (the current space's profile); queries and engagements name that profile's history (`history_source()`; private windows read the default one's) |
| `AutocompleteService.h/.cpp` | `HistorySource` on `query()` and `record_engagement()`; the worker keeps one connection per History database, opened when first asked for, and an empty store for none |

`Tests/UI/Qt/Arc/TestArcProfileSessions.cpp` checks all of this with real processes, headlessly; run it, and
upstream's `Tests/LibWebView` tests, after a merge that touches these files. `BrowsingSession::create_persistent()`
takes `Persist::No` for SQL-disabled runs (and as the fallback when a profile's database can't be opened).


### Extensions

Chrome extensions (Documentation/Elytra/WebExtensions.md) live in `Libraries/LibWebExtensions`, `LibWeb/Extensions`,
`LibWebView/Extension*` and `UI/Qt/Arc/Extensions*`. Upstream files carry these hooks, each commented where it is.

| File | Hook |
| --- | --- |
| `LibURL/Rust/src/origin.rs` | `chrome-extension://<id>` URLs have a tuple origin (only for extension IDs), and so do blob URLs they make |
| `LibWebCommon/Fetch/Infrastructure/URL.h`, `SecureContexts/AbstractOperations.cpp` | `chrome-extension` is a fetch scheme, and potentially trustworthy |
| `LibWeb/Fetch/Fetching/Fetching.cpp` | CORS requests to `chrome-extension` go on to scheme fetch, which hands them to `ResourceLoader`; `LoadRequest` gets the requester's origin |
| `LibWeb/Fetch/Fetching/Fetching.cpp` (host access) | `is_extension_host_access` in main fetch's same-origin branch: an extension's own pages read what its host permissions cover without CORS (`Extensions::reads_without_cors()`) |
| `LibWeb/ContentSecurityPolicy/FetchIntegration.cpp` | `bypasses_content_security_policy()` at the top of the request and response checks: `chrome-extension` files aren't held to a document's CSP, as in Chrome (1Password's connect-src lists only its servers) |
| `LibWeb/Loader/{FileRequest.h,LoadRequest.h,ResourceLoader.cpp}` | `chrome-extension` loads are file requests naming an `ExtensionFile`; responses carry the extension's CSP |
| `LibWeb/DOM/Document.cpp` | `Extensions::document_is_ready_to_run_scripts()` in `set_ready_to_run_scripts()`, `document_readiness_changed()` in `update_readiness()`, `document_is_going_away()` in `destroy()` and `did_stop_being_active_document_in_navigable()` |
| `LibWeb/Page/Page.h`, `Services/WebContent/{PageClient,ConnectionFromClient}`, `WebContentClient.ipc`, `WebContentServer.ipc` | extension contexts' messages and files; installed extensions; scripts the browser runs |
| `Services/WebWorker/{ConnectionFromClient.cpp,WebWorkerClient.ipc}`, `LibWebView/{WebWorkerClient,WorkerProcessManager}` | workers' extension file requests, answered for the worker's own origin |
| `LibWebView/{CanonicalDocument,CanonicalNavigable,WebContentClient}` | extension documents get processes of their own (`extension_host()`, `hosted_extension()`, `may_speak_for_extension()`), as WebUI pages do |
| `LibWebView/WebContentPage.{h,cpp}` | the extension handlers; `extension_file_for_renderer()`, which takes the requester from the UI process (`requester_of_navigation_to_extension_file()`, `hosted_requester()`) and gives a navigation's fetch an empty stand-in |
| `LibWebView/NavigationLoader.{h,cpp}` | `take_extension_response_from_the_browser()` in `did_finish_navigation_params_creation()`: a response at a `chrome-extension` URL is read by the browser, with the extension's CSP (the sandbox CSP and an opaque origin for `sandbox.pages`), sandboxing flags and referrer, or refused; `take_about_response_from_the_browser()`: an `about:blank`/`about:srcdoc` response only for a navigation admitted to that URL, with the browser's body; a redirect's replacement document state is rebuilt from the UI's (no initiator); `refuse_an_extension_origin_the_browser_did_not_give()`; `create_for_history_step()` takes the UI's entry over the echoed one; `set_document()` recomputes an extension file's reserved environment; `set_extension_file_requester()` (no initiator is an opaque requester); an empty referrer policy a renderer reports becomes the default (it would crash the hosting process) |
| `LibWebView/{CanonicalNavigation.h,CanonicalNavigable.{h,cpp},CanonicalSessionHistoryEntry.h,CanonicalTraversable.{h,cpp}}` | `is_browser_ui_navigation` (from `begin_navigation()`'s `IsBrowserUINavigation`, which only the UI's own call sites pass); `held_extension_content` (set in `populate_document()`); a history population's echoed `chrome-extension` URL must be its entry's, its loader is `create_for_history_step()`, and `extension_file_requester_for()` is opaque unless the entry held the extension's page; `document_may_take_the_url()`: a same-document navigation to a `chrome-extension` URL keeps the old URL unless the document has that origin; a populated entry keeps its admitted URL; a new top-level traversable's initial entry is never a `chrome-extension`/`file` URL; a `javascript:` URL's document is no extension's unless the active one is |
| `LibWebView/ViewImplementation.{h,cpp}` (navigation) | `load(URL)` and `load_for_webdriver_navigation()` begin browser-UI navigations (`load(PreparedNavigationDescriptor, IsBrowserUINavigation)`); a page's `load(PreparedNavigationDescriptor)` never is one |
| `LibWebView/WebContentPage.cpp` (navigation) | `navigation_from_page()` refuses a `chrome-extension` URL without a fetch client; `did_create_child_frame()` refuses an initial entry at a `chrome-extension`/`file` URL |
| `LibWeb/Extensions/ExtensionContexts.cpp` | `content_security_policies_for()` returns the browser's policies (`contentSecurityPolicies` from `serialize_for_renderers()`) |
| `LibWebView/{Application,ViewImplementation}` | the registry and runtime; installed extensions are loaded at startup and sent to every page |
| `UI/cmake/ResourceFiles.cmake` | `Base/res/ladybird/extensions` (the `chrome.*` script) |
| `UI/Qt/Arc/ArcSidebar.cpp`, `ArcChrome.cpp` | the extension buttons; `install_extensions_ui()`; the menu item and keyboard commands |
| `LibWeb/Fetch/Fetching/Fetching.cpp` | webRequest and declarativeNetRequest (`LibWeb/Extensions/WebRequests`): `ensure_request_id()` at the top of HTTP-network-or-cache fetch; `authentication_entry_for()` for the spec's "authentication entry" FIXME (the entry functions take the request: entries are per top-level site, origin and realm); the onAuthRequired branch before the 401 prompt step (and `forget_authentication_entry()` when an authentication fetch gets a 401); in the non-standard network fetch, `will_send_request()` (block, a 307 redirect of the browser's own, changed headers), `did_receive_response_headers()` before the headers are added to the response, and `did_finish_request()` at the top of `on_complete` |
| `LibWeb/Fetch/Infrastructure/HTTP/Requests.{h,cpp}` | `extension_request_id`, copied by `clone()` |
| `Services/WebContent/ConnectionFromClient.cpp` | `deliver_extension_message()` hands messages for context 0 to `Extensions::page_message_from_browser()` |
| `LibWebView/ViewImplementation.cpp` | `ExtensionRuntime::page_created()` at the end of `send_preferences_to_page()` (a page learns the rules before it loads anything); the installed extensions come from `ExtensionRuntime::installed_extensions_for()` (none for a private page) |
| `LibWebView/Application.cpp` (extensions) | the registry's `on_change` sends each page `ExtensionRuntime::installed_extensions_for()` (none for a private page) |
| `LibWebView/WebContentPage.{h,cpp}` (private windows, webRequest) | `extension_file_for_renderer()` refuses a private page; `hosts_a_document_with_origin()`, `hosts_a_document_same_site_with()` and `url_of_navigation_populated_in()`, which webRequest checks a renderer's reports against |
| `LibWebView/NavigationLoader.cpp` (private windows) | `take_extension_response_from_the_browser()` refuses an extension's page in a private session |
| `LibWebView/ViewImplementation.{h,cpp}` (context menus) | `ContextMenuTarget` / `context_menu_target()`: what the context menu being opened is for (link, image or media URL, selection, editable), set in each `did_request_*_context_menu()`, for extensions' `chrome.contextMenus` items |
| `UI/Qt/Tab.cpp` (context menus) | `Arc::add_extension_items_to_context_menus()` after the page's context menus are made (`UI/Qt/Arc/ExtensionServices.cpp` adds the extensions' items as each opens), except for a private view |
| `LibWeb/Bindings/WrapperWorld.{h,cpp}`, `Meta/Generators/libweb_bindings/{attributes,to_js_value}.py` | world isolation: `value_for_world()` (another world's object as this world's: a platform object or WindowProxy as this world's wrapper, a promise as this world's promise of its result, an array or plain object copied, anything else structured-cloned), `promise_for_world()`, `completion_for_world()`; the generator applies them to every `any`/`object`/callback/`Promise` value it converts for script and to `[ReturnsJSValue]` getters; `current_non_main_world()` |
| `LibWeb/Bindings/RustBindingsServices.cpp`, `Bindings/Rust/src/bindings/{services,conversions}.rs` | world isolation: `web_bindings_value_for_world()`, applied by the Rust bindings' `ToJs` of `any`, `object`, promises, callbacks and C++ interfaces |
| `LibWeb/WebIDL/{AbstractOperations.cpp,ExceptionOrUtils.h}` | world isolation: a callback's arguments and this value are converted for its own world (`arguments_for_world()`, in invoking, constructing and calling a user object's operation); what an operation throws is converted for the caller's world (`throw_dom_exception_if_needed()`) |
| `LibWeb/HTML/{Window.{h,cpp},WindowProxy.cpp}` | world isolation: `set_opener()` defines `opener` on the setting world's global; `frames[i]` and named frames are the asking world's WindowProxies |
| `LibWeb/HTML/WindowOrWorkerGlobalScope.cpp` (timers) | world isolation: a string handler from an isolated world is compiled and run in that world's realm (`isolated_world_realm`) |
| `LibWeb/DOM/EventTarget.{h,cpp}` | world isolation: other worlds' event handler IDL attributes (`other_world_event_handlers`), set and read only by their own world; `callback_world()` |
| `LibWeb/DOM/{Event.h,Event.cpp,EventDispatcher.cpp}`, `HTML/Scripting/ExceptionReporter.cpp`, `HTML/{WindowOrWorkerGlobalScope,UniversalGlobalScope}.cpp`, `Bindings/MainThreadVM.cpp` | world isolation: `Event::restrict_to_world()`; error, unhandledrejection and rejectionhandled events go only to their world's listeners |
| `LibWeb/DOM/AdoptedStyleSheets.cpp` | world isolation: the adopted style sheets list keeps the main world's wrappers of the sheets |
| `LibWeb/HTML/Window.{h,cpp,idl}`, `HTML/CustomElements/CustomElementRegistry.cpp` | world isolation: `customElements` is null outside the main world, and `define()` throws there |

### Picture-in-Picture as a tab is left

Arc shows a playing video in upstream's floating Picture-in-Picture window when its tab is left, and puts it back on
return (`ArcSidebar::update_auto_picture_in_picture()`). The request is the browser's own, not the page's, so it needs
no user activation; these hooks carry it. All are marked `Fork hook (Arc)`.

| File | Hook |
| --- | --- |
| `LibWeb/HTML/HTMLVideoElement.h/.cpp` | `request_picture_in_picture(PictureInPictureRequester)`: `UserAgent` skips the transient-activation step |
| `LibWeb/Page/Page.h/.cpp` | `enter_picture_in_picture_for_playing_video()` (the largest playing video) and `exit_picture_in_picture()` |
| `Services/WebContent/WebContentServer.ipc`, `ConnectionFromClient.h/.cpp` | the two messages above |
| `LibWebView/WebContentPage.h/.cpp` | `enter_picture_in_picture_for_playing_video()` grants one browser-initiated window (`consume_user_agent_picture_in_picture_grant()`, checked beside `consume_transient_user_activation()` where a window is opened) |
| `LibWebView/ViewImplementation.h/.cpp` | `enter_picture_in_picture_for_playing_video()`, `exit_picture_in_picture()`, `has_picture_in_picture_window()`, `toggle_playing_media()`, `set_picture_in_picture_controls_caption()` |

### Arc's Picture-in-Picture window and sidebar player

The window looks like Arc's, and its Minimize leaves the video playing with a player in the sidebar (`MiniPlayer`).

| File | Hook |
| --- | --- |
| `LibCompositing/PictureInPictureControls.h` | Arc's bar of controls: `PictureInPictureControl::Minimize`, and labelled buttons along the top (icons only in a narrow window) |
| `LibCompositing/UIOverlays.h/.cpp` | `PictureInPictureControlsState::font_family` and `caption` (the site's name), and their IPC |
| `Services/Compositor/PictureInPictureControls.cpp` | rewritten to paint Arc's frosted buttons, labels and caption |
| `LibWebView/ViewImplementation.cpp` | Minimize's tooltip; the caption and font in `update_ui_overlays()` |
| `UI/Qt/PictureInPictureWindow.cpp` | Minimize (`on_close(VideoKeepsPlaying::Yes)`), the caption, and on macOS a clear, shadowed window with a rounded page |
| `UI/Qt/MacWindow.h/.mm` | `make_appkit_window_clear_with_shadow()`, `invalidate_appkit_window_shadow()`, and `set_appkit_window_buttons_grey()` `appkit_window_buttons_look_inactive()`, `refresh_appkit_window_buttons_grey()` and `appkit_window_button_dots_visible()` (every window's buttons look as an inactive window's: the real ones hidden by a filter under drawn dots, in colour under the pointer; the dots go when the buttons are hidden) |
| `LibWeb/HTML/MediaControls.html/.css/.cpp` | the player's skip buttons (`data-option="picture-in-picture-player"`), a rounded pause icon, and the `picture-in-picture-player` layout |
| `LibWeb/Page/Page.h/.cpp`, `WebContentServer.ipc`, `ConnectionFromClient.h/.cpp` | `toggle_playing_media()` (pauses what plays, plays what it paused) |
| `LibWebView/PictureInPictureManager.h/.cpp` | `on_windows_changed`, called as a window opens or closes |
| `Tests/LibCompositing/TestPictureInPictureControls.cpp`, `TestUIOverlays.cpp`, `Tests/LibWebView/TestPictureInPictureWindowControls.cpp` | updated for the new controls |

### Other

| File | Hook |
| --- | --- |
| `README.md` | Elytra's introduction above upstream's README |
| `UI/Qt/Tab.h`, `BrowserWindow.cpp` (restored titles) | `Tab::session_for_returning_tab`: a deferred restored tab's title and icon come from its profile's history |
| `UI/Qt/BrowserWindow.h` | `friend class Arc::SelfTest` (the self-test restores tabs as a launch does) |
| `UI/Qt/BrowserWindow.cpp` | `tab_audio_play_state_changed()` returns for a tab that's gone (closed while playing), which crashed |
| `UI/Qt/WebContentView.h/.cpp` | `set_paints_chrome_behind_corners()` (off in the Picture-in-Picture window, which is itself round), and `friend class Arc::SelfTest` |

The TabBar hooks let the Arc sidebar (spaces, folders, pinned tabs, favorites) replace the
contents of upstream's vertical tab column while reusing its sizing, collapsing and window controls.

Check the full list any time with:

```sh
git diff upstream/master --stat -- . ':!UI/Qt/Arc' ':!SYNCING.md'
```

## Updating

```sh
Meta/elytra-sync-upstream.sh             # fetch, merge, build, run every test
Meta/elytra-sync-upstream.sh --continue  # after resolving conflicts by hand
git push origin arc
```

The `arc` branch is published (and releases are built from it), so upstream is **merged** in, never rebased onto: a
rebase rewrites every Elytra commit and needs a force push. The script turns on Git's `rerere`, so a conflict
resolved once is resolved the same way the next time it comes up.

Between merges, a scheduled GitHub workflow (`elytra-upstream-check.yml`) test-merges upstream every three days and
fails, listing the conflicting files, once it no longer merges cleanly. Merging often keeps each merge small.

### If the merge conflicts

Conflicts happen at the hooks above. Keep upstream's new code and re-add the hook line in the equivalent spot, then
`git add` the file and run the script with `--continue`. Spots that conflicted before:

- `UI/Qt/main.cpp`: `Ladybird::Arc::install();` goes right before upstream's startup code (which now restores the
  previous session) opens the first window.
- `UI/Qt/Tab.cpp`, `focus_location_editor()`: the `on_focus_location_editor` hook goes after upstream's checks.
- `Services/Compositor/PictureInPictureControls.cpp`: Arc rewrote this file; take upstream's include changes (it
  moved `PaintingSurface` and the Skia helpers from LibGfx into the Compositor) and keep Arc's painting.

### Rules that keep merges easy

- Arc code lives in `UI/Qt/Arc/`. An upstream file gets only a small hook, marked `Fork hook (Arc)`, preferably a
  static callback or flag on an upstream class (`Tab::toolbar_left_margin`, `Tab::animates_loading_spinner`) that Arc
  sets from `ArcChrome.cpp`, rather than Arc logic inside upstream code. List each one above.
- A tab may not have loaded: upstream restores tabs deferred, without a view until shown. Arc code must not call
  `Tab::view()` on a tab that isn't `is_loaded()`; use `Tab::url()` for its address.

### If the build breaks after a merge

Upstream renamed or reworked something the Arc code uses (for example a method on
`BrowserWindow`, `Tab`, `LocationEdit` or the web view). To fix it:

1. Find the commit that broke it: `git log upstream/master -S'<old name>' -- UI/Qt`
2. See how upstream updated its own Qt code in that commit (`git show <hash> -- UI/Qt`).
3. Make the same change in `UI/Qt/Arc/`.

`UI/Qt/Arc/CommandBar.cpp` copies one piece of upstream logic,
`Tab::location_edit_return_pressed()`. If that function changes upstream, mirror the change.

The sidebar also relies on upstream behavior that could change silently:
- `TabWidget::take_tab()` removes the page from the stack before the tab bar picks a new current tab
  (used to tell a closed tab apart from a tab switch).
- The hidden sidebar's floating panel is a native child window embedded in the browser window, the same
  trick upstream's address-bar suggestions use to stack above the page.
- While the sidebar is hidden, the macOS traffic lights are hidden (and kept hidden against Qt re-showing
  them) and the floating panel draws its own; the toolbar's left margin is trimmed by finding
  `LadybirdNavigationToolbar` by object name.
- The web view gives pages first claim on most key presses, so Arc's space shortcuts are caught in an
  app-wide event filter (`ArcChrome.cpp`).
- Upstream leaves the window's background to the native window. The sidebar paints the (tinted) chrome color
  under the whole window from its event filter on the window's `Paint` events, so nothing shows the untinted native
  color; the self-test `every_chrome_surface_takes_the_space_color` renders the window under two tints and fails on
  any patch that doesn't change. Run it after a merge that touches chrome styling.
- Arc's other shortcuts (`ArcShortcuts.h`) take some keys upstream binds: Cmd+D (bookmark), Cmd+Shift+C (dev
  tools, still on Cmd+Shift+I and F12), and next/previous tab (which upstream steps through in tab order, across
  spaces). The same filter claims the `ShortcutOverride` for those keys, so Qt hands them over as plain key presses
  instead of firing upstream's actions. If upstream rebinds an action onto one of Arc's keys, Arc's wins.

## Tests

`Tests/UI/Qt/Arc/` holds the fork's tests (spaces data, reading Arc's sidebar data from a made-up fixture,
and sidebar widget styling: centered icons, uniform symbol sizes, drawn window controls, row geometry,
downloads wording). They run headless:

```sh
ninja -C Build/release TestArcSpaceStore TestArcWidgets TestArcImportParser TestArcProfileSessions
Build/release/bin/TestArcSpaceStore && Build/release/bin/TestArcWidgets && Build/release/bin/TestArcImportParser
LADYBIRD_SOURCE_DIR=$PWD Build/release/bin/TestArcProfileSessions
```

`TestArcEdgeCases` and `TestArcReleaseInfo` run the same way. The sidebar's integration tests run in a real window
(they need a desktop session):

```sh
LADYBIRD_SOURCE_DIR=$PWD Tests/UI/Qt/Arc/run-arc-self-test.sh Build/release/bin/Ladybird.app/Contents/MacOS/Ladybird
```

To watch the dock/hide animations without touching the mouse, run with `ARC_DEV_AUTOTOGGLE=1600`
(milliseconds): the sidebar docks and hides on that interval. `ARC_DEV_IMPORT=1` imports from Arc at
startup without dialogs and logs the summary (the same as File › Import from Arc…).

## Importing from Arc

File › Import from Arc… reads Arc's `StorableSidebar.json` (spaces, pinned folders and tabs, Today's tabs,
favorites per Arc profile), each Arc profile's Chromium `Bookmarks` file, and site icons from each profile's
`Favicons` database (copied first, opened read-only; the import links SQLite for this). Re-importing updates
the same spaces, matched by Arc's space id, and never adds a favorite twice.

## Where Arc data is stored

Spaces, folders, pinned tabs and favorites live in `ArcSpaces.json` inside the Ladybird profile's data
directory, with imported site icons in `ArcIcons/` and each Arc profile's cookies and site data in
`ArcProfiles/<profile>/` next to it, never in this repo. A local pre-commit hook (`.git/hooks/pre-commit`, not pushed) refuses
commits containing browser data files.

## Building on this Mac

An old Intel Homebrew in `/usr/local` has binaries that can't run on this machine
(for example `sha512sum`), which breaks the dependency bootstrap. Build with `/usr/local`
removed from `PATH`:

```sh
export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:$(echo "$PATH" | tr ':' '\n' | grep -v '^/usr/local' | grep -v '^/opt/homebrew' | paste -sd: -)"
```

## Backing up to GitHub

Fork `LadybirdBrowser/ladybird` on GitHub, then:

```sh
git remote add origin git@github.com:<you>/ladybird.git
git push -u origin arc
# after each merge:
git push --force-with-lease origin arc
```

## Releases and signing

Pushing a tag named `arc-v<major>.<minor>.<patch>` runs `.github/workflows/arc-release-macos.yml`, which builds the
Apple Silicon app and publishes it as a GitHub release: the zip, its `.sha256`, and its `.sig`.

The app installs an update only if its `.sig` is a valid Ed25519 signature, made with the release key, of a short
statement naming the release's tag, the file and its SHA-256 (`UI/Qt/Arc/ReleaseSignature.{h,cpp}`). Releases without
one (`arc-v0.1.0`, `arc-v0.1.1`) only get their page offered.

- The private key is in the repository secret `ARC_RELEASE_SIGNING_KEY` (PEM). The maintainer keeps a copy outside
  the repository; it is never committed.
- The public key is `RELEASE_PUBLIC_KEY_BASE64` in `ReleaseSignature.h`. The workflow checks each signature against it
  before publishing, so a key that doesn't match fails the release rather than shipping one nobody can install.
- To rotate the key, installed apps must first get the new public key in a release signed with the *old* key: put the
  new key in `ReleaseSignature.h`, and for that one release point the workflow's check at the old public key. Then
  switch the secret to the new key. If the private key is lost, installed apps can't update themselves any more;
  users have to download the next release by hand.
