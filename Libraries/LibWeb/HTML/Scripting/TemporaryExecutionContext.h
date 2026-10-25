/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// When JS is run from outside the context of any user script, we currently do not have a running execution context.
// This results in a crash when we access VM::running_execution_context(). This is a spec issue. Until it is resolved,
// this is a workaround to temporarily push an execution context.
class [[nodiscard]] WEB_API TemporaryExecutionContext {
public:
    enum class CallbacksEnabled {
        No,
        Yes,
    };

    explicit TemporaryExecutionContext(EnvironmentSettingsObject&, CallbacksEnabled = CallbacksEnabled::No);
    explicit TemporaryExecutionContext(JS::Realm&, CallbacksEnabled = CallbacksEnabled::No);
    ~TemporaryExecutionContext();

private:
    GC::Ref<EnvironmentSettingsObject> m_settings;
    CallbacksEnabled m_callbacks_enabled { CallbacksEnabled::No };
    // Elytra: The realm of another world than the main one (an extension's isolated world) shares the principal
    //         realm's settings object; this makes it the current realm, so that what the steps create is its own.
    OwnPtr<JS::ExecutionContext> m_world_execution_context;
};

}
