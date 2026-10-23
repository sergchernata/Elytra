/*
 * Shows or hides the native window controls (macOS traffic lights) for the Arc sidebar:
 * when the sidebar is hidden they disappear, and they come back when it slides out.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QList>
#include <QRectF>

class QWidget;

namespace Ladybird::Arc {

void set_window_controls_hidden(QWidget& window, bool hidden);

// Where the native close / minimize / zoom buttons sit, in the window's logical coordinates (top-left
// origin). Valid even while hidden. Empty where there are no native controls.
QList<QRectF> native_window_control_rects(QWidget& window);

}
