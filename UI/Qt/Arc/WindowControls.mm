/*
 * Shows or hides the native window controls (macOS traffic lights) for the Arc sidebar.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/WindowControls.h>

#include <QWidget>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

// Qt refreshes the title bar buttons whenever the window's flags or layout change, which un-hides them.
// This watches each button's "hidden" flag and puts it back while the Arc sidebar wants them hidden.
@interface ArcWindowControlsHider : NSObject
{
    NSButton* m_buttons[3];
    BOOL m_hidden;
    BOOL m_applying;
}
- (void)setControlsHidden:(BOOL)hidden window:(NSWindow*)window;
@end

@implementation ArcWindowControlsHider

- (void)observeButtonsOfWindow:(NSWindow*)window
{
    constexpr NSWindowButton types[] = { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton };
    for (size_t i = 0; i < 3; ++i) {
        auto* button = [window standardWindowButton:types[i]];
        if (button == m_buttons[i])
            continue;
        // AppKit can replace the buttons (e.g. around full screen); follow the new ones.
        if (m_buttons[i]) {
            [m_buttons[i] removeObserver:self forKeyPath:@"hidden"];
            [m_buttons[i] release];
        }
        m_buttons[i] = [button retain];
        [m_buttons[i] addObserver:self forKeyPath:@"hidden" options:NSKeyValueObservingOptionNew context:nil];
    }
}

- (void)apply
{
    m_applying = YES;
    for (auto* button : m_buttons) {
        if (button && button.hidden != m_hidden)
            button.hidden = m_hidden;
    }
    m_applying = NO;
}

- (void)setControlsHidden:(BOOL)hidden window:(NSWindow*)window
{
    m_hidden = hidden;
    [self observeButtonsOfWindow:window];
    [self apply];
}

- (void)observeValueForKeyPath:(NSString*)keyPath ofObject:(id)object change:(NSDictionary*)change context:(void*)context
{
    (void)keyPath;
    (void)change;
    (void)context;
    if (m_applying || [(NSButton*)object isHidden] == m_hidden)
        return;
    // Undo the change after the current layout pass, which may set it again otherwise.
    dispatch_async(dispatch_get_main_queue(), ^{
        [self apply];
    });
}

- (void)dealloc
{
    for (auto* button : m_buttons) {
        if (!button)
            continue;
        [button removeObserver:self forKeyPath:@"hidden"];
        [button release];
    }
    [super dealloc];
}

@end

namespace Ladybird::Arc {

QList<QRectF> native_window_control_rects(QWidget& window)
{
    QList<QRectF> rects;
    auto* view = reinterpret_cast<NSView*>(window.winId());
    auto* ns_window = view.window;
    if (!ns_window)
        return rects;
    auto window_height = ns_window.frame.size.height;
    for (auto type : { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton }) {
        auto* button = [ns_window standardWindowButton:type];
        if (!button)
            return {};
        // Window coordinates have a bottom-left origin; Qt's (with the content under the title bar) top-left.
        auto frame = [button convertRect:button.bounds toView:nil];
        rects.append(QRectF(frame.origin.x, window_height - frame.origin.y - frame.size.height, frame.size.width, frame.size.height));
    }
    return rects;
}

static char s_hider_key;

void set_window_controls_hidden(QWidget& window, bool hidden)
{
    auto* view = reinterpret_cast<NSView*>(window.winId());
    auto* ns_window = view.window;
    if (!ns_window)
        return;

    ArcWindowControlsHider* hider = objc_getAssociatedObject(ns_window, &s_hider_key);
    if (!hider) {
        hider = [[ArcWindowControlsHider alloc] init];
        objc_setAssociatedObject(ns_window, &s_hider_key, hider, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [hider release];
    }
    // Only visibility changes; upstream's offset observer (MacWindow.mm) still owns their position.
    [hider setControlsHidden:hidden window:ns_window];
}

}
