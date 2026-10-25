/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionRuntime.h>

namespace WebView {

// chrome.alarms, chrome.idle and chrome.i18n's browser-side functions.
class AlarmsAPI final : public ExtensionAPI {
public:
    explicit AlarmsAPI(ExtensionRuntime& runtime)
        : ExtensionAPI(runtime)
    {
    }
};

NonnullOwnPtr<ExtensionAPI> create_alarms_api(ExtensionRuntime& runtime)
{
    return make<AlarmsAPI>(runtime);
}

}
