/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <AK/kmalloc.h>
#include <LibWebView/Export.h>

namespace WebView {

class ExtensionRuntime;
class WebContentPage;

// One part of the chrome.* API (a namespace or a few), with whatever it keeps. Its constructor registers its functions
// with the runtime (ExtensionRuntime::register_function()), which owns it.
class WEBVIEW_API ExtensionAPI {
    AK_MAKE_NONCOPYABLE(ExtensionAPI);
    AK_MAKE_NONMOVABLE(ExtensionAPI);
    AK_ALLOC_WITH_KMALLOC;

public:
    virtual ~ExtensionAPI() = default;

    // A context of an extension is gone (its page closed or navigated away).
    virtual void context_destroyed(u64 /* context_id */) { }
    // A context added or removed a listener to the event.
    virtual void listeners_changed(u64 /* context_id */, StringView /* event */) { }
    // An extension was installed, replaced or removed.
    virtual void extensions_changed() { }
    // The user removed an extension: forget what's kept for it.
    virtual void extension_uninstalled(StringView /* extension_id */) { }
    // The browser is shutting down, while the rest of the application is still whole.
    virtual void shut_down() { }
    // A page (in a page process) was made, or its process changed: what it needs to know before it loads anything.
    virtual void page_created(WebContentPage&) { }
    // A page closed: forget what's kept for it.
    virtual void page_gone(WebContentPage&) { }

protected:
    explicit ExtensionAPI(ExtensionRuntime& runtime)
        : m_runtime(runtime)
    {
    }

    ExtensionRuntime& runtime() const { return m_runtime; }

private:
    ExtensionRuntime& m_runtime;
};

// The parts, each in ExtensionAPI/<Name>.cpp.
NonnullOwnPtr<ExtensionAPI> create_storage_api(ExtensionRuntime&);
NonnullOwnPtr<ExtensionAPI> create_action_api(ExtensionRuntime&);
NonnullOwnPtr<ExtensionAPI> create_alarms_api(ExtensionRuntime&);
NonnullOwnPtr<ExtensionAPI> create_native_messaging_api(ExtensionRuntime&);
NonnullOwnPtr<ExtensionAPI> create_tabs_api(ExtensionRuntime&);
NonnullOwnPtr<ExtensionAPI> create_management_api(ExtensionRuntime&);
NonnullOwnPtr<ExtensionAPI> create_notifications_api(ExtensionRuntime&);
NonnullOwnPtr<ExtensionAPI> create_context_menus_api(ExtensionRuntime&);
NonnullOwnPtr<ExtensionAPI> create_downloads_api(ExtensionRuntime&);
NonnullOwnPtr<ExtensionAPI> create_web_request_api(ExtensionRuntime&);

// For tests (ExtensionAPI/Alarms.cpp): until called, chrome.alarms keeps Chrome's 30-second minimum and chrome.idle
// asks the system.
struct IdleState {
    AK::Duration idle_time;
    bool screen_locked { false };
};
WEBVIEW_API void set_minimum_alarm_delay_for_testing(AK::Duration);
WEBVIEW_API void set_idle_state_source_for_testing(Function<IdleState()>, AK::Duration poll_interval);

// For tests (ExtensionAPI/WebRequest.cpp): what the page is told of webRequest and declarativeNetRequest.
WEBVIEW_API String web_request_config_for_testing(WebContentPage&);
// For tests (ExtensionRuntime.cpp), which run where any page counts as having the user's activation: until cleared,
// whether a context has it (and uses it up) is the source's answer.
WEBVIEW_API void set_user_activation_for_testing(Function<bool()>);

}
