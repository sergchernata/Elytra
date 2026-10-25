/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <LibIPC/File.h>
#include <LibURL/Origin.h>
#include <LibWebExtensions/Manifest.h>
#include <LibWebExtensions/Messages.h>
#include <LibWebView/Export.h>

namespace WebView {

// An installed extension: its files are served at chrome-extension://<id>/.
class WEBVIEW_API Extension {
    AK_ALLOC_WITH_KMALLOC;

public:
    Extension(String id, ByteString root, WebExtensions::Manifest, WebExtensions::MessageCatalog);

    String const& id() const { return m_id; }
    ByteString const& root() const { return m_root; }
    WebExtensions::Manifest const& manifest() const { return m_manifest; }
    WebExtensions::MessageCatalog const& messages() const { return m_messages; }

    // The extension's file, read whole, or nothing when it isn't one of its files.
    Optional<String> read_file(StringView path) const;

    URL::Origin origin() const;

    // The page the background runs in, when the extension has a background: Chrome's service worker (or MV2's
    // scripts) in a page of its own, at chrome-extension://<id>/_generated_background_page.html.
    static constexpr StringView background_page_path = "/_generated_background_page.html"sv;
    Optional<URL::URL> background_page_url() const;
    Optional<String> background_page_html() const;

    // Whether a page or script of the requester's origin may load the file (a path below the root, starting with "/").
    // The extension itself may load any of its files; others only what it makes web accessible to them.
    bool may_be_loaded_by(StringView path, Optional<URL::Origin> const& requester) const;

private:
    String m_id;
    ByteString m_root;
    WebExtensions::Manifest m_manifest;
    WebExtensions::MessageCatalog m_messages;
};

class WEBVIEW_API ExtensionRegistry {
public:
    // Reads an unpacked extension's folder. An extension with the same ID is replaced.
    ErrorOr<Extension const*> load_unpacked(ByteString const& directory);

    Extension const* find(StringView id) const;

    // Whether the user lets the extension's content scripts run in local files, as Chrome's "Allow access to file URLs".
    void set_allows_file_access(StringView id, bool);
    bool allows_file_access(StringView id) const;

    template<typename Callback>
    void for_each_extension(Callback callback) const
    {
        for (auto const& [id, extension] : m_extensions)
            callback(*extension);
    }

    // What page processes need to know of the installed extensions: their manifests and messages, and their content
    // scripts with the scripts' sources (LibWeb/Extensions/ExtensionContexts).
    String serialize_for_renderers() const;

    // Called when an extension is installed or replaced.
    Function<void()> on_change;

    // Opens a file of an extension for a renderer that asked for it on behalf of the requester.
    ErrorOr<IPC::File> open_file(StringView id, StringView path, Optional<URL::Origin> const& requester) const;

private:
    HashMap<String, NonnullOwnPtr<Extension>> m_extensions;
    HashTable<String> m_file_access_allowed;
    mutable Optional<String> m_serialized_for_renderers;
};

}
