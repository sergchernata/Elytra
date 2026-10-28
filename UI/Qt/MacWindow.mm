/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/MacWindow.h>

#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QDrag>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPointer>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>
#include <UI/Qt/WebContentView.h>

#import <Cocoa/Cocoa.h>
#import <CoreImage/CoreImage.h>
#import <QuartzCore/QuartzCore.h>
#import <UI/Qt/DictionaryLookup.h>
#import <dlfcn.h>
#import <objc/runtime.h>

// Fork hook (Arc): every window's buttons look as an inactive window's do (a faint dot with a glassy rim), in colour
// only while the pointer is over them. AppKit has no setting for it: a Core Image filter hides each real button (it
// still takes clicks) and a drawn dot sits over it; hovering lifts both.
@interface LadybirdInactiveButtonLook : NSView
- (void)setDark:(bool)dark;
@end

@implementation LadybirdInactiveButtonLook
{
    CAGradientLayer* m_rim;
    CAShapeLayer* m_rim_mask;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    self.wantsLayer = YES;
    m_rim = [[CAGradientLayer alloc] init];
    m_rim_mask = [[CAShapeLayer alloc] init];
    m_rim_mask.fillColor = nil;
    m_rim_mask.strokeColor = NSColor.blackColor.CGColor;
    m_rim_mask.lineWidth = 1;
    m_rim.mask = m_rim_mask;
    [self.layer addSublayer:m_rim];
    return self;
}

- (void)dealloc
{
    [m_rim release];
    [m_rim_mask release];
    [super dealloc];
}

// Clicks go to the real button underneath.
- (NSView*)hitTest:(NSPoint)point
{
    (void)point;
    return nil;
}

- (void)layout
{
    [super layout];
    auto bounds = self.bounds;
    self.layer.cornerRadius = bounds.size.width / 2;
    m_rim.frame = bounds;
    auto* path = CGPathCreateWithEllipseInRect(CGRectInset(bounds, 0.5, 0.5), nullptr);
    m_rim_mask.path = path;
    CGPathRelease(path);
}

- (void)setDark:(bool)dark
{
    // Sampled from AppKit's own inactive buttons: a faint fill, and a rim that catches the light at the top and bottom.
    auto* tint = dark ? NSColor.whiteColor : NSColor.blackColor;
    self.layer.backgroundColor = [tint colorWithAlphaComponent:dark ? 0.10 : 0.10].CGColor;
    auto edge = [tint colorWithAlphaComponent:dark ? 0.19 : 0.14].CGColor;
    auto side = [tint colorWithAlphaComponent:dark ? 0.03 : 0.04].CGColor;
    m_rim.colors = @[ (id)edge, (id)side, (id)edge ];
    m_rim.locations = @[ @0.0, @0.5, @1.0 ];
    self.needsLayout = YES;
}

@end

@interface LadybirdGreyWindowButtons : NSObject
{
    NSWindow* m_window; // Weak: this lives as long as the window.
    NSButton* m_buttons[3];
    LadybirdInactiveButtonLook* m_looks[3];
    CIFilter* m_hide;
    bool m_dark;
    bool m_hovered;
}
- (instancetype)initWithWindow:(NSWindow*)window;
- (void)setDark:(bool)dark;
- (void)apply;
- (void)findButtons;
- (bool)looksInactive;
- (bool)dotsVisible;
@end

@implementation LadybirdGreyWindowButtons

- (instancetype)initWithWindow:(NSWindow*)window
{
    self = [super init];
    if (!self)
        return nil;
    m_window = window;
    m_hide = [[CIFilter filterWithName:@"CIColorMatrix"] retain];
    [m_hide setDefaults];
    [m_hide setValue:[CIVector vectorWithX:0 Y:0 Z:0 W:0] forKey:@"inputAVector"];
    [self findButtons];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    for (auto* look : m_looks) {
        [look removeFromSuperview];
        [look release];
    }
    [m_hide release];
    [super dealloc];
}

// AppKit can replace the buttons (when it lays the title bar out again), so they're looked up each time.
- (void)findButtons
{
    NSWindowButton const kinds[] = { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton };
    for (int i = 0; i < 3; ++i) {
        auto* button = [m_window standardWindowButton:kinds[i]];
        if (button != m_buttons[i]) {
            if (m_buttons[i])
                [[NSNotificationCenter defaultCenter] removeObserver:self name:NSViewFrameDidChangeNotification object:m_buttons[i]];
            m_buttons[i] = button;
            if (button) {
                button.postsFrameChangedNotifications = YES;
                [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(buttonMoved:) name:NSViewFrameDidChangeNotification object:button];
            }
        }
        if (!button)
            continue;
        button.wantsLayer = YES;
        bool tracked = false;
        for (NSTrackingArea* area in button.trackingAreas)
            tracked |= area.owner == self;
        if (!tracked) {
            auto* area = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                      options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                        owner:self
                                                     userInfo:nil];
            [button addTrackingArea:area];
            [area release];
        }
        if (!m_looks[i])
            m_looks[i] = [[LadybirdInactiveButtonLook alloc] initWithFrame:button.frame];
        if (m_looks[i].superview != button.superview || button.superview.subviews.lastObject != m_looks[i]) {
            [m_looks[i] removeFromSuperview];
            [button.superview addSubview:m_looks[i] positioned:NSWindowAbove relativeTo:nil];
        }
        m_looks[i].frame = button.frame;
    }
}

- (void)buttonMoved:(NSNotification*)notification
{
    for (int i = 0; i < 3; ++i) {
        if (m_buttons[i] == notification.object && m_looks[i])
            m_looks[i].frame = m_buttons[i].frame;
    }
}

- (void)setDark:(bool)dark
{
    m_dark = dark;
    for (auto* look : m_looks)
        [look setDark:dark];
    [self apply];
}

- (void)apply
{
    [self findButtons];
    for (int i = 0; i < 3; ++i) {
        if (!m_buttons[i])
            continue;
        m_buttons[i].contentFilters = m_hovered ? @[] : @[ m_hide ];
        [m_looks[i] setDark:m_dark];
        // A dot only over a button that's there: hidden, or made transparent (as the Arc sidebar does when it hides),
        // takes its dot with it.
        m_looks[i].hidden = m_hovered || m_buttons[i].hidden || m_buttons[i].alphaValue == 0;
    }
}

// For the tests: each button hidden by the filter, with its drawn dot over it.
- (bool)looksInactive
{
    [self findButtons];
    for (int i = 0; i < 3; ++i) {
        auto* button = m_buttons[i];
        if (!button || button.hidden)
            continue;
        if (button.contentFilters.count != 1 || !m_looks[i] || m_looks[i].hidden || m_looks[i].superview != button.superview
            || !NSEqualRects(m_looks[i].frame, button.frame) || !m_looks[i].layer.backgroundColor)
            return false;
    }
    return m_buttons[0] != nil;
}

- (bool)dotsVisible
{
    [self findButtons];
    for (int i = 0; i < 3; ++i) {
        if (m_looks[i] && !m_looks[i].hidden && m_looks[i].superview)
            return true;
    }
    return false;
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

// AppKit reports no more than whether a scroll event ends the fingers' part of a gesture, but the HID event behind it
// says whether momentum follows. The functions that read it are private, so they are looked up rather than linked.
static bool appkit_scroll_momentum_will_begin(NSEvent* event)
{
    using CopyIOHIDEventFunction = CFTypeRef (*)(CGEventRef);
    using GetScrollMomentumFunction = u8 (*)(CFTypeRef);
    static auto copy_iohid_event = reinterpret_cast<CopyIOHIDEventFunction>(dlsym(RTLD_DEFAULT, "CGEventCopyIOHIDEvent"));
    static auto get_scroll_momentum = reinterpret_cast<GetScrollMomentumFunction>(dlsym(RTLD_DEFAULT, "IOHIDEventGetScrollMomentum"));
    static constexpr u8 scroll_momentum_will_begin = 1 << 3;

    if (!copy_iohid_event || !get_scroll_momentum || !event.CGEvent)
        return false;
    auto iohid_event = copy_iohid_event(event.CGEvent);
    if (!iohid_event)
        return false;
    ScopeGuard release_iohid_event = [&] { CFRelease(iohid_event); };
    return (get_scroll_momentum(iohid_event) & scroll_momentum_will_begin) != 0;
}

struct RecordedAppKitScrollMomentumTransition {
    u64 timestamp_in_milliseconds { 0 };
    AppKitScrollMomentumTransition transition;
};
static Vector<RecordedAppKitScrollMomentumTransition> s_recorded_appkit_scroll_momentum_transitions;

// Qt delivers a wheel event after AppKit dispatched the scroll event it came from, and stamps it with that event's
// time in milliseconds.
static u64 qt_timestamp_for_appkit_event(NSEvent* event)
{
    return static_cast<u64>(event.timestamp * 1000);
}

static void record_appkit_scroll_momentum_transition(NSEvent* event)
{
    if (event.type != NSEventTypeScrollWheel)
        return;

    Optional<AppKitScrollMomentumTransition> transition;
    if (event.momentumPhase == NSEventPhaseBegan)
        transition = AppKitScrollMomentumTransition::Began;
    else if (event.phase == NSEventPhaseEnded && event.momentumPhase == NSEventPhaseNone && appkit_scroll_momentum_will_begin(event))
        transition = AppKitScrollMomentumTransition::WillBegin;
    if (!transition.has_value())
        return;

    static constexpr size_t maximum_recorded_transitions = 8;
    if (s_recorded_appkit_scroll_momentum_transitions.size() == maximum_recorded_transitions)
        s_recorded_appkit_scroll_momentum_transitions.remove(0);
    s_recorded_appkit_scroll_momentum_transitions.append({
        .timestamp_in_milliseconds = qt_timestamp_for_appkit_event(event),
        .transition = transition.value(),
    });
}

// Qt makes a ScrollEnd of the lift, unless it saw the momentum coming, and a ScrollBegin of the momentum's first event,
// unless it did. The fingers' last step can share the lift's millisecond, and takes neither.
static bool qt_wheel_event_is_made_of(QWheelEvent const& wheel_event, AppKitScrollMomentumTransition transition)
{
    switch (transition) {
    case AppKitScrollMomentumTransition::WillBegin:
        return wheel_event.phase() == Qt::ScrollEnd;
    case AppKitScrollMomentumTransition::Began:
        return wheel_event.phase() == Qt::ScrollBegin;
    }
    VERIFY_NOT_REACHED();
}

Optional<AppKitScrollMomentumTransition> take_appkit_scroll_momentum_transition_for(QWheelEvent const& wheel_event)
{
    // The transitions recorded before this one went to other windows, or were dropped by Qt.
    auto timestamp = static_cast<u64>(wheel_event.timestamp());
    while (!s_recorded_appkit_scroll_momentum_transitions.is_empty() && s_recorded_appkit_scroll_momentum_transitions.first().timestamp_in_milliseconds < timestamp)
        s_recorded_appkit_scroll_momentum_transitions.remove(0);
    for (size_t index = 0; index < s_recorded_appkit_scroll_momentum_transitions.size(); index++) {
        auto const& recorded = s_recorded_appkit_scroll_momentum_transitions[index];
        if (recorded.timestamp_in_milliseconds > timestamp)
            break;
        if (!qt_wheel_event_is_made_of(wheel_event, recorded.transition))
            continue;
        auto transition = recorded.transition;
        s_recorded_appkit_scroll_momentum_transitions.remove(index);
        return transition;
    }
    return {};
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
        record_appkit_scroll_momentum_transition(event);

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

void disable_appkit_window_animations(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return;
    view.window.animationBehavior = NSWindowAnimationBehaviorNone;
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
    [buttons setDark:tint.red() > 127];
}

// Fork hook (Arc): after the buttons are hidden or shown again, their dots follow.
void refresh_appkit_window_buttons_grey(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return;
    auto* buttons = static_cast<LadybirdGreyWindowButtons*>(objc_getAssociatedObject(view.window, &s_grey_window_buttons_key));
    [buttons apply];
}

bool appkit_window_button_dots_visible(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return false;
    auto* buttons = static_cast<LadybirdGreyWindowButtons*>(objc_getAssociatedObject(view.window, &s_grey_window_buttons_key));
    return buttons && [buttons dotsVisible];
}

bool appkit_window_buttons_look_inactive(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return false;
    auto* buttons = static_cast<LadybirdGreyWindowButtons*>(objc_getAssociatedObject(view.window, &s_grey_window_buttons_key));
    return buttons && [buttons looksInactive];
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

void round_appkit_view_corners(QWidget& widget, qreal radius, QColor const& border_color)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;
    view.wantsLayer = YES;
    auto* layer = view.layer;
    layer.cornerRadius = radius;
    layer.cornerCurve = kCACornerCurveContinuous;
    layer.masksToBounds = YES;
    layer.borderWidth = 1;
    auto* color = CGColorCreateSRGB(border_color.redF(), border_color.greenF(), border_color.blueF(), border_color.alphaF());
    layer.borderColor = color;
    CGColorRelease(color);
}

void stack_appkit_view_below(QWidget& widget, QWidget& sibling)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    auto* sibling_view = reinterpret_cast<NSView*>(sibling.winId());
    if (!view || !sibling_view || !sibling_view.superview)
        return;
    [sibling_view.superview addSubview:view positioned:NSWindowBelow relativeTo:sibling_view];
}

void round_appkit_view_bottom_corners(QWidget& widget, qreal radius)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;
    view.wantsLayer = YES;
    auto* layer = view.layer;
    layer.cornerRadius = radius;
    layer.cornerCurve = kCACornerCurveContinuous;
    // NB: Qt's views are flipped, so the bottom of the view is at its layer's largest y.
    layer.maskedCorners = view.isFlipped ? (kCALayerMinXMaxYCorner | kCALayerMaxXMaxYCorner) : (kCALayerMinXMinYCorner | kCALayerMaxXMinYCorner);
    layer.masksToBounds = YES;
}

void set_appkit_view_opacity(QWidget& widget, qreal opacity)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;
    view.alphaValue = opacity;
}

}

