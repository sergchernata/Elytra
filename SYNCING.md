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
| `UI/Qt/TabBar.h/.cpp` (hidden sidebar) | `collapsed_vertical_tabs_width()` (a thin hover edge instead of an icon column), `vertical_sidebar_expanded_width()`, upstream's hover-expand switched off in `can_expand_vertical_tabs_on_hover()`, `on_vertical_sidebar_expanded_will_change` (called at the top of `set_vertical_tabs_expanded()`), and `set_vertical_sidebar_reserved_width()` with its check at the top of `vertical_tabs_layout_width()` (lets the dock/hide animation move the page edge) |
| `Tests/UI/Qt/CMakeLists.txt` | `include(Arc/ArcTests.cmake)` just before the `if (NOT LINUX) return()` near the end |
| `UI/Qt/ChromeStyle.h/.cpp` | `set_chrome_background_tint()`, and `chrome_background()` applying it (the space color across the window frame) |
| `UI/Qt/BrowserWindow.h/.cpp` | `on_tab_will_close`, called in `definitely_close_tab()` before the tab is removed (the sidebar picks the next tab from the same space, or leaves an empty page so closing a space's last tab doesn't close the window), with the early returns around it for a tab already gone; `is_popup_window()` (popups get no sidebar) |

### Profiles (LibWebView)

Arc profiles keep separate cookies and site data in one window. Upstream picks a tab's browsing session by
private-or-not; these hooks carry the session itself instead. All are marked `Fork hook (Arc profiles)`.

| File | Hook |
| --- | --- |
| `BrowsingSession.h/.cpp` | `create_persistent()` (a profile's own cookie, storage and HSTS database) and `records()` (history, favicons and tab restore stay the default session's) |
| `Application.h/.cpp` | `set_session_for_new_views()`, `session_for_new_web_view()`, `did_create_profile_session()`, `request_server_client(BrowsingSession&)`; `create_web_content_client()`, `open_page_for_new_tab()` and `launch_child_frame_web_content_process()` take a session (the spare process is only the default session's); `clear_browsing_data()` clears profiles' site data too |
| `ViewImplementation.cpp` | the session comes from `session_for_new_web_view()`, follows the process showing the view (popups), and records go through `records()`; context-menu downloads pass the session |
| `WebContentClient.h/.cpp` | constructed with a session (the private-or-not constructor stays, forwarding) |
| `HelperProcess.h/.cpp` | `launch_web_content_process()` and `launch_web_worker_process()` take a session |
| `WebWorkerClient.h/.cpp`, `WorkerProcessManager.h/.cpp` | workers take their owner's session; shared-worker keys, per-site worker counts and BroadcastChannel delivery are partitioned by session |
| `FileDownloader.h/.cpp` | downloads remember the session they started in (`m_download_sessions`) for their requests |
| `CanonicalNavigable.cpp`, `CanonicalTraversable.cpp` | child-frame processes get the page's session |
| `WebContentPage.cpp`, `ProcessSnapshot.cpp`, `WebUI/HistoryUI.cpp` | history and favicons through `records()`; downloads and BroadcastChannel by session |
| `CookieJar.h/.cpp` | `TransientStorage::owner`: cookie-change notifications go only to views using that jar |
| `RequestServerManager.cpp` | profile sessions (`is_profile_session()`) keep no HTTP disk cache, like private ones |
| `Autocomplete.cpp` | remote suggestions use `session_for_new_web_view()`'s cookies (the current space's profile) |

`Tests/UI/Qt/Arc/TestArcProfileSessions.cpp` checks all of this with real processes, headlessly; run it, and
upstream's `Tests/LibWebView` tests, after a rebase that touches these files. `BrowsingSession::create_persistent()`
takes `Persist::No` for SQL-disabled runs (and as the fallback when a profile's database can't be opened).


The TabBar hooks let the Arc sidebar (spaces, folders, pinned tabs, favorites) replace the
contents of upstream's vertical tab column while reusing its sizing, collapsing and window controls.

Check the full list any time with:

```sh
git diff upstream/master --stat -- . ':!UI/Qt/Arc' ':!SYNCING.md'
```

## Updating

```sh
git fetch upstream
git rebase upstream/master          # replays the arc commits on top of the latest upstream
./Meta/ladybird.py build --preset Release
./Meta/ladybird.py run --preset Release
```

### If the rebase conflicts

Conflicts can only happen at the hooks above. Re-add the hook line in the
equivalent spot in the new upstream code, then `git add` the file and run `git rebase --continue`.

### If the build breaks after a rebase

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
# after each rebase:
git push --force-with-lease origin arc
```
