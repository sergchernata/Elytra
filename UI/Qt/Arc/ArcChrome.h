/*
 * Entry point for the Arc-style chrome layered on top of the Qt UI.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Ladybird::Arc {

// Called once from main() after the Application exists and before the first window opens.
void install();

}
