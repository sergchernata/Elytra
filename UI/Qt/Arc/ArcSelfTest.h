/*
 * Integration tests of the Arc sidebar against real browser windows, tabs and WebContent processes.
 *
 * Inert unless ARC_SELF_TEST is set: then, once the first window is up, it runs its scenarios in that window
 * (seeding its own spaces, pinned tabs and favorites, with data: pages, so nothing touches the network), prints
 * PASS/FAIL lines, and exits with 0 only if everything passed. Run it with a throwaway profile, as
 * Tests/UI/Qt/Arc/run-arc-self-test.sh does.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Ladybird::Arc {

void schedule_self_test_if_requested();

}
