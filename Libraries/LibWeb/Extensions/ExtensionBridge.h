/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Extensions {

// One extension context's line to the browser: an extension page, its background, or a content script's world.
class WEB_API ExtensionBridge final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(ExtensionBridge, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(ExtensionBridge);

public:
    static GC::Ref<ExtensionBridge> create(HTML::Window&, u64 context_id, bool is_content_script, Utf16String info);
    virtual ~ExtensionBridge() override;

    u64 context_id() const { return m_context_id; }

    Utf16String const& info() const { return m_info; }
    void post_message(Utf16String const&);
    void set_message_handler(WebIDL::CallbackType&);
    GC::Ptr<DOM::ShadowRoot> open_or_closed_shadow_root(DOM::Element&);

    // A message from the browser, for the handler.
    void deliver(Utf16String const&);

    // The context is gone: nothing more is sent or delivered.
    void close() { m_closed = true; }

private:
    ExtensionBridge(HTML::Window&, u64 context_id, bool is_content_script, Utf16String info);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<HTML::Window> m_window;
    u64 m_context_id { 0 };
    bool m_is_content_script { false };
    bool m_closed { false };
    Utf16String m_info;
    GC::Ptr<WebIDL::CallbackType> m_handler;
};

}
