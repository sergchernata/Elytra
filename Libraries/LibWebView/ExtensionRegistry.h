/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <LibIPC/File.h>
#include <LibURL/Origin.h>
#include <LibWebExtensions/Manifest.h>
#include <LibWebView/Export.h>

namespace WebView {

// An installed extension: its files are served at chrome-extension://<id>/.
class WEBVIEW_API Extension {
    AK_ALLOC_WITH_KMALLOC;

public:
    Extension(String id, ByteString root, WebExtensions::Manifest);

    String const& id() const { return m_id; }
    ByteString const& root() const { return m_root; }
    WebExtensions::Manifest const& manifest() const { return m_manifest; }

    URL::Origin origin() const;

    // Whether a page or script of the requester's origin may load the file (a path below the root, starting with "/").
    // The extension itself may load any of its files; others only what it makes web accessible to them.
    bool may_be_loaded_by(StringView path, Optional<URL::Origin> const& requester) const;

private:
    String m_id;
    ByteString m_root;
    WebExtensions::Manifest m_manifest;
};

class WEBVIEW_API ExtensionRegistry {
public:
    // Reads an unpacked extension's folder. An extension with the same ID is replaced.
    ErrorOr<Extension const*> load_unpacked(ByteString const& directory);

    Extension const* find(StringView id) const;

    // Opens a file of an extension for a renderer that asked for it on behalf of the requester.
    ErrorOr<IPC::File> open_file(StringView id, StringView path, Optional<URL::Origin> const& requester) const;

private:
    HashMap<String, NonnullOwnPtr<Extension>> m_extensions;
};

}
