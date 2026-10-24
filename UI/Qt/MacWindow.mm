/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/MacWindow.h>

#include <AK/ScopeGuard.h>
#include <LibGfx/Color.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QDrag>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPointer>
#include <QWidget>
#include <QWindow>
#include <UI/Qt/WebContentView.h>

#import <Cocoa/Cocoa.h>
#import <CoreImage/CoreImage.h>
#import <UI/Qt/DictionaryLookup.h>
#import <objc/runtime.h>

// Fork hook (Arc): grey window buttons, as Arc's, in colour only while the pointer is over them (as AppKit shows an
// inactive window's). A Core Image filter paints each button one flat grey, keeping its shape; hovering lifts it.
@interface LadybirdGreyWindowButtons : NSObject
{
    NSWindow* m_window; // Weak: this lives as long as the window.
    NSButton* m_buttons[3];
    CIFilter* m_filter;
    bool m_hovered;
}
- (instancetype)initWithWindow:(NSWindow*)window;
- (void)setTint:(CGFloat)white alpha:(CGFloat)alpha;
- (void)apply;
- (void)findButtons;
@end

@implementation LadybirdGreyWindowButtons

- (instancetype)initWithWindow:(NSWindow*)window
{
    self = [super init];
    if (!self)
        return nil;
    m_window = window;
    [self findButtons];
    return self;
}

// AppKit can replace the buttons (when it lays the title bar out again), so they're looked up each time.
- (void)findButtons
{
    NSWindowButton const kinds[] = { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton };
    for (int i = 0; i < 3; ++i) {
        auto* button = [m_window standardWindowButton:kinds[i]];
        m_buttons[i] = button;
        if (!button)
            continue;
        button.wantsLayer = YES;
        bool tracked = false;
        for (NSTrackingArea* area in button.trackingAreas)
            tracked |= area.owner == self;
        if (tracked)
            continue;
        auto* area = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                  options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                    owner:self
                                                 userInfo:nil];
        [button addTrackingArea:area];
        [area release];
    }
}

- (void)dealloc
{
    [m_filter release];
    [super dealloc];
}

- (void)setTint:(CGFloat)white alpha:(CGFloat)alpha
{
    // Every button becomes `white` (0 black, 1 white) at `alpha`, keeping its round shape: as AppKit draws an inactive
    // window's buttons, a translucent dot over the title bar.
    [m_filter release];
    m_filter = [[CIFilter filterWithName:@"CIColorMatrix"] retain];
    [m_filter setDefaults];
    [m_filter setValue:[CIVector vectorWithX:0 Y:0 Z:0 W:0] forKey:@"inputRVector"];
    [m_filter setValue:[CIVector vectorWithX:0 Y:0 Z:0 W:0] forKey:@"inputGVector"];
    [m_filter setValue:[CIVector vectorWithX:0 Y:0 Z:0 W:0] forKey:@"inputBVector"];
    [m_filter setValue:[CIVector vectorWithX:0 Y:0 Z:0 W:alpha] forKey:@"inputAVector"];
    [m_filter setValue:[CIVector vectorWithX:white Y:white Z:white W:0] forKey:@"inputBiasVector"];
    [self apply];
}

- (void)apply
{
    [self findButtons];
    for (auto* button : m_buttons) {
        if (!button)
            continue;
        button.contentFilters = (m_hovered || !m_filter) ? @[] : @[ m_filter ];
    }
}

- (bool)pointerIsOverAButton
{
    auto location = [NSEvent mouseLocation];
    for (auto* button : m_buttons) {
        if (!button || !button.window)
            continue;
        auto frame = [button.window convertRectToScreen:[button convertRect:button.bounds toView:nil]];
        if (NSPointInRect(location, frame))
            return true;
    }
    return false;
}

- (void)mouseEntered:(NSEvent*)event
{
    (void)event;
    m_hovered = true;
    [self apply];
}

- (void)mouseExited:(NSEvent*)event
{
    (void)event;
    // Moving between buttons leaves one before entering the next.
    m_hovered = [self pointerIsOverAButton];
    [self apply];
}

@end

@interface LadybirdWindowControlOffsetObserver : NSObject
{
    int m_x_offset;
    int m_y_offset;
    NSButton* m_buttons[3];
    NSPoint m_target_origins[3];
    bool m_update_scheduled;
    bool m_is_applying_offset;
}

- (instancetype)initWithWindow:(NSWindow*)window xOffset:(int)x_offset yOffset:(int)y_offset;
- (void)applyWindowControlOffsets;
- (void)updateTargetOriginForButton:(NSButton*)button;
- (void)scheduleWindowControlOffsetUpdate:(NSButton*)button;
- (void)windowControlFrameDidChange:(NSNotification*)notification;
- (void)windowDidResize:(NSNotification*)notification;

@end

@implementation LadybirdWindowControlOffsetObserver

- (instancetype)initWithWindow:(NSWindow*)window xOffset:(int)x_offset yOffset:(int)y_offset
{
    self = [super init];
    if (!self)
        return nil;

    m_x_offset = x_offset;
    m_y_offset = y_offset;

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(windowDidResize:) name:NSWindowDidResizeNotification object:window];

    // NB: Lay out the title bar now, so the controls are in their default place to offset them from, and move them
    //     right away. Before a window is first shown, this keeps it from ever appearing with its controls unmoved.
    [window.contentView.superview layoutSubtreeIfNeeded];

    constexpr NSWindowButton button_types[] = { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton };
    for (size_t i = 0; i < std::size(button_types); ++i) {
        auto* button = [window standardWindowButton:button_types[i]];
        m_buttons[i] = [button retain];
        button.postsFrameChangedNotifications = YES;
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(windowControlFrameDidChange:) name:NSViewFrameDidChangeNotification object:button];
        [self updateTargetOriginForButton:button];
    }
    [self applyWindowControlOffsets];

    return self;
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];
    for (auto* button : m_buttons)
        [button release];
    [super dealloc];
}

- (void)applyWindowControlOffsets
{
    m_is_applying_offset = true;
    for (size_t i = 0; i < std::size(m_buttons); ++i)
        [m_buttons[i] setFrameOrigin:m_target_origins[i]];
    [m_buttons[0].superview updateTrackingAreas];
    m_is_applying_offset = false;
    m_update_scheduled = false;
}

- (void)updateTargetOriginForButton:(NSButton*)button
{
    for (size_t i = 0; i < std::size(m_buttons); ++i) {
        if (m_buttons[i] != button)
            continue;

        auto origin = button.frame.origin;
        origin.x += m_x_offset;
        origin.y -= m_y_offset;
        m_target_origins[i] = origin;
        break;
    }
}

- (void)scheduleWindowControlOffsetUpdate:(NSButton*)button
{
    [self updateTargetOriginForButton:button];

    if (m_update_scheduled)
        return;

    // NB: AppKit ignores frame changes made while its titlebar layout is in progress.
    //     Apply the offset after that layout pass returns to the main event loop.
    m_update_scheduled = true;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self applyWindowControlOffsets];
    });
}

- (void)windowControlFrameDidChange:(NSNotification*)notification
{
    if (m_is_applying_offset)
        return;
    [self scheduleWindowControlOffsetUpdate:notification.object];
}

- (void)windowDidResize:(NSNotification*)notification
{
    [self applyWindowControlOffsets];
}

@end

namespace Ladybird {

static NSEvent* s_latest_window_drag_event;
static NSInteger s_last_pressure_stage;

static bool is_window_drag_on_gesture_enabled()
{
    return [[NSUserDefaults standardUserDefaults] boolForKey:@"NSWindowShouldDragOnGesture"];
}

static bool is_appkit_event_type(QByteArray const& event_type)
{
    return event_type == QByteArrayLiteral("NSEvent")
        || event_type == QByteArrayLiteral("mac_generic_NSEvent");
}

static bool is_window_drag_event(NSEvent* event)
{
    if (!event)
        return false;

    switch (event.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseDragged:
        return true;
    default:
        return false;
    }
}

static bool should_start_window_drag_for_gesture(NSEvent* event)
{
    if (!event || event.type != NSEventTypeLeftMouseDown)
        return false;

    if (!is_window_drag_on_gesture_enabled())
        return false;

    auto modifier_flags = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if ((modifier_flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != (NSEventModifierFlagCommand | NSEventModifierFlagControl))
        return false;

    auto* window = event.window;
    if (!window || !window.isMovable || (window.styleMask & NSWindowStyleMaskFullScreen))
        return false;

    return true;
}

static NSPoint widget_position_to_ns_point(QWidget& widget, Gfx::IntPoint position)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return {};

    auto device_pixel_ratio = widget.devicePixelRatioF();
    auto point = NSMakePoint(position.x() / device_pixel_ratio, position.y() / device_pixel_ratio);
    if (![view isFlipped])
        point.y = NSHeight(view.bounds) - point.y;
    return point;
}

static Optional<QPointF> widget_point_for_appkit_event(QWidget& widget, NSEvent* event)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !event.window)
        return {};

    auto point = [view convertPoint:event.locationInWindow fromView:nil];
    if (![view isFlipped])
        point.y = NSHeight(view.bounds) - point.y;
    return QPointF { point.x, point.y };
}

static Optional<Gfx::IntPoint> widget_position_for_appkit_event(QWidget& widget, NSEvent* event)
{
    auto point = widget_point_for_appkit_event(widget, event);
    if (!point.has_value())
        return {};

    auto device_pixel_ratio = widget.devicePixelRatioF();
    return Gfx::IntPoint {
        static_cast<int>(point->x() * device_pixel_ratio),
        static_cast<int>(point->y() * device_pixel_ratio),
    };
}

static WebContentView* web_content_view_for_appkit_event(NSEvent* event)
{
    if (!event.window)
        return nullptr;

    auto* content_view = event.window.contentView;
    if (!content_view)
        return nullptr;

    auto point = [content_view convertPoint:event.locationInWindow fromView:nil];
    for (auto* view = [content_view hitTest:point]; view; view = view.superview) {
        auto* widget = QWidget::find(reinterpret_cast<WId>(view));
        if (!widget)
            continue;
        if (auto* web_content_view = qobject_cast<WebContentView*>(widget))
            return web_content_view;
    }

    return nullptr;
}

static bool should_perform_dictionary_lookup_for_event(NSEvent* event)
{
    if (!event)
        return false;

    switch (event.type) {
    case NSEventTypeQuickLook:
        return true;
    case NSEventTypePressure: {
        auto stage = event.stage;
        auto should_lookup = s_last_pressure_stage == 1
            && stage == 2
            && [[NSUserDefaults standardUserDefaults] integerForKey:@"com.apple.trackpad.forceClick"] == 1;
        s_last_pressure_stage = stage;
        return should_lookup;
    }
    default:
        return false;
    }
}

static bool perform_dictionary_lookup_for_event(NSEvent* event)
{
    if (!should_perform_dictionary_lookup_for_event(event))
        return false;

    auto* view = web_content_view_for_appkit_event(event);
    if (!view)
        return false;

    auto position = widget_position_for_appkit_event(*view, event);
    if (!position.has_value())
        return false;

    return view->look_up_selected_text_at(*position);
}

static QPointer<WebContentView> s_view_following_mouse_while_inactive;

// Qt ignores mouse movement while Ladybird is inactive, so it is passed on here to a view that follows it anyway.
static void follow_mouse_while_inactive(NSEvent* event)
{
    if (!event || NSApp.active)
        return;

    if (event.type == NSEventTypeMouseExited) {
        auto view = s_view_following_mouse_while_inactive;
        if (!view || event.window != reinterpret_cast<NSView*>(view->winId()).window)
            return;
        s_view_following_mouse_while_inactive = nullptr;
        QEvent leave_event { QEvent::Leave };
        QCoreApplication::sendEvent(view, &leave_event);
        return;
    }

    if (event.type != NSEventTypeMouseMoved)
        return;

    auto* view = web_content_view_for_appkit_event(event);
    if (!view || !view->follows_mouse_while_inactive())
        return;
    auto point = widget_point_for_appkit_event(*view, event);
    if (!point.has_value())
        return;

    s_view_following_mouse_while_inactive = view;
    QMouseEvent move_event { QEvent::MouseMove, *point, view->mapToGlobal(*point), Qt::NoButton, Qt::NoButton, Qt::NoModifier };
    QCoreApplication::sendEvent(view, &move_event);
}

class LadybirdAppKitEventCaptureFilter final : public QAbstractNativeEventFilter {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual bool nativeEventFilter(QByteArray const& event_type, void* message, qintptr*) override
    {
        if (!is_appkit_event_type(event_type))
            return false;

        auto* event = static_cast<NSEvent*>(message);
        follow_mouse_while_inactive(event);

        if (perform_dictionary_lookup_for_event(event))
            return true;

        if (should_start_window_drag_for_gesture(event)) {
            [event.window performWindowDragWithEvent:event];
            return true;
        }

        if (!is_window_drag_event(event))
            return false;

        [s_latest_window_drag_event release];
        s_latest_window_drag_event = [event copy];
        return false;
    }
};

void install_appkit_event_capture()
{
    static auto* filter = new LadybirdAppKitEventCaptureFilter;
    static bool installed = false;
    if (installed)
        return;

    if (auto* application = QCoreApplication::instance()) {
        application->installNativeEventFilter(filter);
        installed = true;
    }
}

Optional<u64> appkit_display_id_for_window(QWidget& widget)
{
    NSScreen* screen = nil;
    // winId() would create the native window, so only a window that already has one is asked for its screen.
    if (auto* window_widget = widget.window(); window_widget && window_widget->windowHandle()) {
        if (auto* view = reinterpret_cast<NSView*>(window_widget->winId()))
            screen = view.window.screen;
    }
    if (screen == nil)
        screen = NSScreen.mainScreen;
    if (screen == nil)
        return {};

    NSNumber* screen_number = screen.deviceDescription[@"NSScreenNumber"];
    if (screen_number == nil)
        return {};
    return static_cast<u64>(screen_number.unsignedLongLongValue);
}

void hide_appkit_window_title(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;

    auto* window = view.window;
    if (!window)
        return;

    window.titleVisibility = NSWindowTitleHidden;
}

void make_appkit_window_resizable(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return;
    view.window.styleMask |= NSWindowStyleMaskResizable;
}

// Fork hook (Arc): see the header.
void make_appkit_window_clear_with_shadow(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return;
    view.window.opaque = NO;
    view.window.backgroundColor = NSColor.clearColor;
    view.window.hasShadow = YES;
    [view.window invalidateShadow];
}

void invalidate_appkit_window_shadow(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (view && view.window)
        [view.window invalidateShadow];
}

void keep_appkit_window_visible_while_inactive(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return;
    view.window.hidesOnDeactivate = NO;
}

void ensure_appkit_window_is_exposed(QWidget& widget)
{
    auto* window = widget.windowHandle();
    if (!widget.isVisible() || !window || window->isExposed())
        return;

    // Qt restores a native view's exposed state from its display callback. A view whose backing layer is already
    // clean can remain unexposed after being shown or raised, causing Qt to route hover events to the view below it.
    [reinterpret_cast<NSView*>(widget.winId()) setNeedsDisplay:YES];
}

void set_appkit_window_content_aspect_ratio(QWidget& widget, Gfx::IntSize aspect_ratio)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return;
    view.window.contentAspectRatio = NSMakeSize(aspect_ratio.width(), aspect_ratio.height());
}

static char s_window_control_offset_observer_key;

void offset_appkit_window_controls(QWidget& widget, int x_offset, int y_offset)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;

    auto* window = view.window;
    if (!window)
        return;
    if (objc_getAssociatedObject(window, &s_window_control_offset_observer_key))
        return;

    auto* observer = [[LadybirdWindowControlOffsetObserver alloc] initWithWindow:window xOffset:x_offset yOffset:y_offset];
    objc_setAssociatedObject(window, &s_window_control_offset_observer_key, observer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [observer release];
}

static char s_grey_window_buttons_key;

// Fork hook (Arc): see LadybirdGreyWindowButtons.
void set_appkit_window_buttons_grey(QWidget& widget, Gfx::Color tint)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return;
    auto* window = view.window;
    auto* buttons = static_cast<LadybirdGreyWindowButtons*>(objc_getAssociatedObject(window, &s_grey_window_buttons_key));
    if (!buttons) {
        buttons = [[LadybirdGreyWindowButtons alloc] initWithWindow:window];
        objc_setAssociatedObject(window, &s_grey_window_buttons_key, buttons, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [buttons release];
    }
    [buttons setTint:(tint.red() > 127 ? 1.0 : 0.0) alpha:tint.alpha() / 255.0];
}

void reapply_appkit_window_control_offsets(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.internalWinId());
    if (!view || !view.window)
        return;

    LadybirdWindowControlOffsetObserver* observer = objc_getAssociatedObject(view.window, &s_window_control_offset_observer_key);
    [observer applyWindowControlOffsets];
}

bool start_appkit_window_drag(QWidget& widget)
{
    auto* window_widget = widget.window();
    if (!window_widget)
        return false;

    auto* view = reinterpret_cast<NSView*>(window_widget->winId());
    if (!view)
        return false;

    auto* window = view.window;
    if (!window)
        return false;

    auto* current_event = NSApp.currentEvent;
    auto current_event_is_valid = is_window_drag_event(current_event);
    auto current_event_window_matches = current_event_is_valid && current_event.window == window;

    auto* event = current_event_window_matches ? current_event : s_latest_window_drag_event;
    if (!is_window_drag_event(event) || event.window != window)
        return false;

    [window performWindowDragWithEvent:event];
    return true;
}

Qt::DropAction execute_appkit_tab_drag(QWidget& widget, QDrag& drag)
{
    if (QGuiApplication::platformName() != "cocoa")
        return drag.exec(Qt::MoveAction, Qt::MoveAction);

    auto* view = reinterpret_cast<NSView*>(widget.window()->winId());
    auto* window = view.window;
    if (!window)
        return drag.exec(Qt::MoveAction, Qt::MoveAction);

    // Qt's Cocoa backend calls this synchronous AppKit API with slideBack:YES. Dropping outside a tab strip is
    // rejected by AppKit, so exec() waits for the slide-back animation before we can create the detached window.
    // Override only the source window's class, and restore it as soon as this tab drag finishes.
    auto window_class = object_getClass(window);
    auto* selector = @selector(dragImage:at:offset:event:pasteboard:source:slideBack:);
    auto* method = class_getInstanceMethod(window_class, selector);
    if (!method)
        return drag.exec(Qt::MoveAction, Qt::MoveAction);

    // Add an override if the method is inherited, so we don't replace NSWindow's implementation globally.
    class_addMethod(window_class, selector, method_getImplementation(method), method_getTypeEncoding(method));
    method = class_getInstanceMethod(window_class, selector);

    auto original_implementation = method_getImplementation(method);
    auto original_drag_image = reinterpret_cast<void (*)(id, SEL, NSImage*, NSPoint, NSSize, NSEvent*, NSPasteboard*, id, BOOL)>(original_implementation);
    auto implementation = imp_implementationWithBlock(^(NSWindow* receiver, NSImage* image, NSPoint location, NSSize offset, NSEvent* event, NSPasteboard* pasteboard, id source, BOOL slide_back) {
        original_drag_image(receiver, selector, image, location, offset, event, pasteboard, source, receiver == window ? NO : slide_back);
    });
    method_setImplementation(method, implementation);
    ScopeGuard restore_implementation = [&]() {
        method_setImplementation(method, original_implementation);
        imp_removeBlock(implementation);
    };

    return drag.exec(Qt::MoveAction, Qt::MoveAction);
}

void show_appkit_dictionary_lookup(QWidget& widget, WebView::DictionaryLookup const& lookup, Gfx::IntPoint position)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    show_dictionary_lookup(view, lookup, widget_position_to_ns_point(widget, position));
}

}
