/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DocumentReadyState.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/HTML/CrossProcessId.h>

// The extension contexts of this process: the chrome.* API in installed extensions' own pages, and their content
// scripts, each extension's in an isolated world of its own in the pages they match.
namespace Web::Extensions {

// The installed extensions, as the browser describes them (WebView::ExtensionRegistry::serialize_for_renderers()).
WEB_API void set_installed_extensions(StringView json);

// The content security policies an installed extension's pages and workers run under: the manifest's (or Chrome's
// default), and Chrome's minimum, which applies whatever the manifest says. Nothing for an extension that isn't installed.
WEB_API Vector<String> content_security_policies_for(StringView extension_id);

// As in Chrome, an extension's own pages and background read what its host permissions cover without CORS: whether a
// request from `requester` to `url` does (only in the process of that extension's pages, which the browser tells).
bool reads_without_cors(URL::Origin const& requester, URL::URL const& url);

// A message from the browser to a context.
WEB_API void deliver_message(u64 context_id, String const& message);

// Runs a script the browser was asked to run (chrome.scripting.executeScript, insertCSS) in the active document of a
// frame of the page: in the extension's isolated world, made if need be, or in the page's own (the "MAIN" world). The
// page reports its completion value (awaited, as JSON) to the browser, as a message for no context (ID 0):
// { type: "script_result", request, result? } or { type: "script_result", request, error }.
WEB_API void run_extension_script(Page&, u64 request_id, HTML::CrossProcessId navigable_id, String const& extension_id, bool main_world, String const& source, String const& filename);

// The document lifecycle points where contexts start, run their scripts, and end.
void document_is_ready_to_run_scripts(DOM::Document&);
void document_readiness_changed(DOM::Document&, HTML::DocumentReadyState);
void document_is_going_away(DOM::Document&);

}
