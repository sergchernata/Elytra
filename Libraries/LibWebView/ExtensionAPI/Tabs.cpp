/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionRuntime.h>

namespace WebView {

// chrome.tabs, chrome.windows, chrome.scripting and chrome.webNavigation.
class TabsAPI final : public ExtensionAPI {
public:
    explicit TabsAPI(ExtensionRuntime& runtime)
        : ExtensionAPI(runtime)
    {
    }
};

NonnullOwnPtr<ExtensionAPI> create_tabs_api(ExtensionRuntime& runtime)
{
    return make<TabsAPI>(runtime);
}

}
