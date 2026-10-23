/*
 * Shows or hides the native window controls (macOS traffic lights) for the Arc sidebar.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/WindowControls.h>

#include <QWidget>

#import <AppKit/AppKit.h>

namespace Ladybird::Arc {

void set_window_controls_hidden(QWidget& window, bool hidden)
{
    auto* view = reinterpret_cast<NSView*>(window.winId());
    auto* ns_window = view.window;
    if (!ns_window)
        return;

    // Only visibility changes; upstream's offset observer (MacWindow.mm) still owns their position.
    for (auto type : { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton }) {
        if (auto* button = [ns_window standardWindowButton:type]; button && button.hidden != hidden)
            button.hidden = hidden;
    }
}

}
