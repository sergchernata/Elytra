/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionRuntime.h>

namespace WebView {

// runtime.connectNative and runtime.sendNativeMessage.
class NativeMessagingAPI final : public ExtensionAPI {
public:
    explicit NativeMessagingAPI(ExtensionRuntime& runtime)
        : ExtensionAPI(runtime)
    {
    }
};

NonnullOwnPtr<ExtensionAPI> create_native_messaging_api(ExtensionRuntime& runtime)
{
    return make<NativeMessagingAPI>(runtime);
}

}
