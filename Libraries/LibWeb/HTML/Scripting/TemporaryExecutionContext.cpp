/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

namespace Web::HTML {

TemporaryExecutionContext::TemporaryExecutionContext(JS::Realm& realm, CallbacksEnabled callbacks_enabled)
    : TemporaryExecutionContext(principal_realm_settings_object(realm), callbacks_enabled)
{
    // Elytra: See m_world_execution_context.
    if (realm.host_defined() && !Bindings::host_defined_wrapper_world(realm).is_main_world()) {
        m_world_execution_context = JS::ExecutionContext::create(0, ReadonlySpan<JS::Value> {}, 0);
        m_world_execution_context->realm = &realm;
        realm.vm().push_execution_context(*m_world_execution_context);
    }
}

TemporaryExecutionContext::TemporaryExecutionContext(EnvironmentSettingsObject& settings, CallbacksEnabled callbacks_enabled)
    : m_settings(settings)
    , m_callbacks_enabled(callbacks_enabled)
{
    prepare_to_run_script(m_settings);
    if (m_callbacks_enabled == CallbacksEnabled::Yes)
        prepare_to_run_callback(m_settings);
}

TemporaryExecutionContext::~TemporaryExecutionContext()
{
    if (m_world_execution_context)
        m_settings->realm().vm().pop_execution_context();
    clean_up_after_running_script(m_settings);
    if (m_callbacks_enabled == CallbacksEnabled::Yes)
        clean_up_after_running_callback(m_settings);
}

}
