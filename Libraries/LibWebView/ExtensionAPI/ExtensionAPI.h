/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/kmalloc.h>
#include <LibWebView/Export.h>

namespace WebView {

class ExtensionRuntime;

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
    // An extension was installed, replaced or removed.
    virtual void extensions_changed() { }
    // The browser is shutting down, while the rest of the application is still whole.
    virtual void shut_down() { }

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
NonnullOwnPtr<ExtensionAPI> create_alarms_api(ExtensionRuntime&);
NonnullOwnPtr<ExtensionAPI> create_native_messaging_api(ExtensionRuntime&);
NonnullOwnPtr<ExtensionAPI> create_tabs_api(ExtensionRuntime&);
NonnullOwnPtr<ExtensionAPI> create_management_api(ExtensionRuntime&);

}
