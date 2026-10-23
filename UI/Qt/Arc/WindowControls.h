/*
 * Shows or hides the native window controls (macOS traffic lights) for the Arc sidebar:
 * when the sidebar is hidden they disappear, and they come back when it slides out.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

class QWidget;

namespace Ladybird::Arc {

void set_window_controls_hidden(QWidget& window, bool hidden);

}
