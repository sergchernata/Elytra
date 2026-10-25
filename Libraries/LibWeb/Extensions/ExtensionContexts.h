/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DocumentReadyState.h>

// The extension contexts of this process: the chrome.* API in installed extensions' own pages, and their content
// scripts, each extension's in an isolated world of its own in the pages they match.
namespace Web::Extensions {

// The installed extensions, as the browser describes them (WebView::ExtensionRegistry::serialize_for_renderers()).
WEB_API void set_installed_extensions(StringView json);

// A message from the browser to a context.
WEB_API void deliver_message(u64 context_id, String const& message);

// The document lifecycle points where contexts start, run their scripts, and end.
void document_is_ready_to_run_scripts(DOM::Document&);
void document_readiness_changed(DOM::Document&, HTML::DocumentReadyState);
void document_is_going_away(DOM::Document&);

}
