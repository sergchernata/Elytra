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
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibIPC/File.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
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
    // The same, as bytes (an icon), when it's no larger than the maximum.
    Optional<ByteBuffer> read_file_bytes(StringView path, size_t maximum_size = 64 * MiB) const;

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

// What the browser remembers of an extension the user installed, enabled or not.
struct WEBVIEW_API InstalledExtension {
    // chrome.management's ExtensionInstallType: "normal" from a CRX (the Web Store), "development" when unpacked.
    enum class InstallType : u8 {
        Normal,
        Development,
    };

    String id;
    String version;
    String name;
    ByteString path; // The folder it's loaded from.
    InstallType install_type { InstallType::Normal };
    bool enabled { true };
    bool allows_file_access { false };
    UnixDateTime install_time;

    StringView install_type_name() const { return install_type == InstallType::Normal ? "normal"sv : "development"sv; }
};

class WEBVIEW_API ExtensionRegistry {
public:
    // Reads an unpacked extension's folder. An extension with the same ID is replaced. It isn't remembered.
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

    // Called when an extension is installed, replaced, enabled, disabled or removed.
    Function<void()> on_change;
    // Others to tell, after on_change (the UI).
    void add_change_observer(Function<void()> observer) { m_change_observers.append(move(observer)); }

    // Opens a file of an extension for a renderer that asked for it on behalf of the requester.
    ErrorOr<IPC::File> open_file(StringView id, StringView path, Optional<URL::Origin> const& requester) const;

    // Installs: the profile's folder for them, <profile data>/Extensions. CRX installs unpack into <id>/<version>_<n>/
    // there (as Chrome), and Installed.json remembers what's installed. Nothing is installed or remembered without it.
    void set_extensions_directory(ByteString);
    Optional<ByteString> const& extensions_directory() const { return m_extensions_directory; }

    // Loads the enabled extensions Installed.json remembers. One that fails to load stays installed, unloaded.
    ErrorOr<void> load_installed_extensions();

    // Verifies, unpacks and loads a CRX. An extension already installed under the CRX's ID is updated: the new version
    // replaces it only once it has loaded, and an older version than the installed one is refused. When the caller
    // expects an ID, a CRX of another ID is refused.
    ErrorOr<Extension const*> install_crx(ReadonlyBytes crx, Optional<StringView> expected_id = {});

    // A development install: loads the user's folder where it is, and remembers it by its path.
    ErrorOr<Extension const*> install_unpacked(ByteString const& directory);

    // A disabled extension stays installed, but isn't loaded, served or run.
    ErrorOr<void> set_enabled(StringView id, bool);

    // Forgets an installed extension, and removes its files when it was installed from a CRX (not an unpacked folder).
    ErrorOr<void> uninstall(StringView id);

    Vector<InstalledExtension> installed_extensions() const;
    Optional<InstalledExtension const&> installed_extension(StringView id) const;

    // The Chrome Web Store. Requests go through the fetcher, one response per request (redirects aren't followed by
    // it); by default the browser's RequestServer.
    struct FetchResponse {
        u32 status { 0 };
        Optional<String> location;
        ByteBuffer body;
    };
    using FetchCallback = Function<void(ErrorOr<FetchResponse>)>;
    using Fetcher = Function<void(URL::URL const&, size_t maximum_size, FetchCallback)>;
    void set_fetcher(Fetcher fetcher) { m_fetcher = move(fetcher); }

    static constexpr size_t default_maximum_download_size = 200 * MiB;
    void set_maximum_download_size(size_t size) { m_maximum_download_size = size; }

    // An extension ID, or a Web Store page's URL (chromewebstore.google.com/detail/<slug>/<id>, or the old
    // chrome.google.com/webstore/detail/...): the ID, or nothing.
    static Optional<String> extension_id_from_web_store_input(StringView);

    // Downloads the extension from the Web Store and installs it (or updates it).
    void install_from_web_store(StringView id_or_url, Function<void(ErrorOr<Extension const*>)>);

    // Asks the Web Store for newer versions of the installed CRX extensions, and installs them.
    struct UpdateResult {
        String id;
        Optional<String> updated_to;
        Optional<String> error;
    };
    void check_for_updates(Function<void(Vector<UpdateResult>)>);

    static constexpr StringView web_store_product_version = "141.0"sv;

private:
    ErrorOr<NonnullOwnPtr<Extension>> read_extension(ByteString const& directory, Optional<String> const& id) const;
    Extension const* add_loaded_extension(NonnullOwnPtr<Extension>);
    void notify_change();
    ErrorOr<void> save_installed_extensions() const;
    void save_installed_extensions_or_complain() const;
    ErrorOr<ByteString> private_extensions_directory() const;

    void fetch_following_redirects(URL::URL, size_t maximum_size, u32 redirects_left, Function<void(ErrorOr<ByteBuffer>)>);
    void download_and_install(URL::URL const&, String id, Optional<ByteBuffer> expected_sha256, Function<void(ErrorOr<Extension const*>)>);

    HashMap<String, NonnullOwnPtr<Extension>> m_extensions;
    HashTable<String> m_file_access_allowed;
    Vector<Function<void()>> m_change_observers;
    mutable Optional<String> m_serialized_for_renderers;

    Optional<ByteString> m_extensions_directory;
    HashMap<String, InstalledExtension> m_installed;
    Fetcher m_fetcher;
    size_t m_maximum_download_size { default_maximum_download_size };
};

}
