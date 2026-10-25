/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionRuntime.h>

namespace WebView {

// chrome.permissions, chrome.management, chrome.privacy and the rest of chrome.runtime.
class ManagementAPI final : public ExtensionAPI {
public:
    explicit ManagementAPI(ExtensionRuntime& runtime)
        : ExtensionAPI(runtime)
    {
    }
};

NonnullOwnPtr<ExtensionAPI> create_management_api(ExtensionRuntime& runtime)
{
    return make<ManagementAPI>(runtime);
}

}
