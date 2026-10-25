/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Extensions/ExtensionBridge.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::Extensions {

GC_DEFINE_ALLOCATOR(ExtensionBridge);

GC::Ref<ExtensionBridge> ExtensionBridge::create(HTML::Window& window, u64 context_id, bool is_content_script, Utf16String info)
{
    return GC::Heap::the().allocate<ExtensionBridge>(window, context_id, is_content_script, move(info));
}

ExtensionBridge::ExtensionBridge(HTML::Window& window, u64 context_id, bool is_content_script, Utf16String info)
    : m_window(window)
    , m_context_id(context_id)
    , m_is_content_script(is_content_script)
    , m_info(move(info))
{
}

ExtensionBridge::~ExtensionBridge() = default;

void ExtensionBridge::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
    visitor.visit(m_handler);
}

void ExtensionBridge::post_message(Utf16String const& message)
{
    if (m_closed)
        return;
    m_window->page().client().page_did_post_extension_message(m_context_id, message.to_utf8());
}

void ExtensionBridge::set_message_handler(WebIDL::CallbackType& handler)
{
    m_handler = handler;
}

GC::Ptr<DOM::ShadowRoot> ExtensionBridge::open_or_closed_shadow_root(DOM::Element& element)
{
    // Only content scripts may see into closed shadow roots, as Chrome's chrome.dom does.
    if (!m_is_content_script)
        return element.open_shadow_root();
    return element.shadow_root();
}

void ExtensionBridge::deliver(Utf16String const& message)
{
    if (m_closed || !m_handler)
        return;

    auto& realm = m_handler->callback->shape().realm();
    auto result = WebIDL::invoke_callback(*m_handler, {}, { { JS::PrimitiveString::create(realm.vm(), message) } });
    if (result.is_error())
        HTML::report_exception(result, realm);
}

}
