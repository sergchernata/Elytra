/*
 * Little Arc: a small window for one page, for quick lookups and links from other apps, as in Arc. It has no sidebar,
 * just a slim bar: back, the page's title and site, and "Open in <space>" (with a menu of the other spaces). Cmd+O moves
 * its page into the browser window, in the most recent space, and closes it; Cmd+Option+O picks the space.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibURL/URL.h>
#include <QString>

namespace Ladybird {
class BrowserWindow;
}

namespace Ladybird::Arc {

// Opens a Little Arc for `url`; without one, it opens with the command bar, to type a search or address into.
BrowserWindow& open_little_arc(Optional<URL::URL> const& = {});
bool is_little_arc(BrowserWindow const&);

// Cmd+Option+N from any app (macOS); elsewhere a no-op, and the shortcut works while Ladybird is in front.
void register_little_arc_hot_key();

// Moves a Little Arc's page into the browser window, as a tab in `space_id` (the most recent space if empty), and
// closes the Little Arc. Returns false if there's no browser window to move it to.
bool move_little_arc_to_space(BrowserWindow& little_arc, QString space_id = {});
// The space menu, under the bar's "Open in" button (or, from the keyboard, at its middle).
void choose_space_for_little_arc(BrowserWindow& little_arc);

}
