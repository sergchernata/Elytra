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
| `UI/Qt/ChromeStyle.h/.cpp` | `set_chrome_background_tint()`, and `chrome_background()` applying it (the space color across the window frame) |

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
- The web view gives pages first claim on most key presses, so Arc's space shortcuts are caught in an
  app-wide event filter (`ArcChrome.cpp`).

## Where Arc data is stored

Spaces, folders, pinned tabs and favorites live in `ArcSpaces.json` inside the Ladybird profile's data
directory, never in this repo. A local pre-commit hook (`.git/hooks/pre-commit`, not pushed) refuses
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
