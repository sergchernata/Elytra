/*
 * Cmd+Option+N opens a Little Arc from any app, as in Arc: a system-wide hot key.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/LittleArc.h>

#include <QCoreApplication>
#include <QMetaObject>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

namespace Ladybird::Arc {

static OSStatus hot_key_pressed(EventHandlerCallRef, EventRef, void*)
{
    QMetaObject::invokeMethod(QCoreApplication::instance(), [] {
        [NSApp activateIgnoringOtherApps:YES];
        open_little_arc();
    }, Qt::QueuedConnection);
    return noErr;
}

void register_little_arc_hot_key()
{
    static bool registered = false;
    if (registered)
        return;
    registered = true;

    EventTypeSpec spec { kEventClassKeyboard, kEventHotKeyPressed };
    InstallApplicationEventHandler(&hot_key_pressed, 1, &spec, nullptr, nullptr);
    EventHotKeyID id { 'LArc', 1 };
    EventHotKeyRef ref = nullptr;
    // Another app may hold it already; then it works only while Ladybird is in front (see ArcChrome.cpp).
    RegisterEventHotKey(kVK_ANSI_N, cmdKey | optionKey, id, GetApplicationEventTarget(), 0, &ref);
}

}
