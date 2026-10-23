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

// Keeps a native child widget (the floating sidebar) above the page, whose layer the web view re-attaches
// to the window as it resizes, which would otherwise cover it mid-animation.
void keep_native_widget_above_page(QWidget& native_widget);

// Sets a native child widget's layer opacity, which the system compositor applies on the GPU (no repaint).
// Returns false where that isn't available.
bool set_native_widget_opacity(QWidget& native_widget, qreal opacity);

}
