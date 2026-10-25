/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Installing extensions from CRX files, unpacked folders and the Chrome Web Store; remembering, updating, disabling
// and removing them.

#include "ExtensionTestHarness.h"

#include <LibWebExtensions/Manifest.h>

#include <AK/Base64.h>
#include <AK/Hex.h>
#include <AK/ScopeGuard.h>
#include <LibCore/DirIterator.h>
#include <LibWebExtensions/Zip.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <sys/stat.h>

namespace ExtensionTests {

namespace {

using WebView::ExtensionRegistry;
using WebView::InstalledExtension;

class Key {
public:
    Key()
        : m_key(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<size_t>(2048)))
    {
        VERIFY(m_key);
        unsigned char* der = nullptr;
        auto length = i2d_PUBKEY(m_key, &der);
        VERIFY(length > 0);
        m_spki = MUST(ByteBuffer::copy(ReadonlyBytes { der, static_cast<size_t>(length) }));
        OPENSSL_free(der);
    }
    ~Key() { EVP_PKEY_free(m_key); }

    ByteBuffer const& spki() const { return m_spki; }
    String base64_key() const { return MUST(encode_base64(m_spki)); }

    ByteBuffer sign(ReadonlyBytes message) const
    {
        auto* context = EVP_MD_CTX_new();
        VERIFY(context);
        ScopeGuard free_context = [&] { EVP_MD_CTX_free(context); };
        VERIFY(EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, m_key) == 1);
        size_t length = 0;
        VERIFY(EVP_DigestSign(context, nullptr, &length, message.data(), message.size()) == 1);
        auto signature = MUST(ByteBuffer::create_uninitialized(length));
        VERIFY(EVP_DigestSign(context, signature.data(), &length, message.data(), message.size()) == 1);
        signature.resize(length);
        return signature;
    }

    ByteBuffer crx_id() const
    {
        u8 digest[32];
        unsigned int length = 0;
        VERIFY(EVP_Digest(m_spki.data(), m_spki.size(), digest, &length, EVP_sha256(), nullptr) == 1);
        return MUST(ByteBuffer::copy(ReadonlyBytes { digest, 16 }));
    }

    String extension_id() const
    {
        StringBuilder builder;
        auto id = crx_id();
        for (auto byte : id.bytes()) {
            builder.append(static_cast<char>('a' + (byte >> 4)));
            builder.append(static_cast<char>('a' + (byte & 0xf)));
        }
        return MUST(builder.to_string());
    }

private:
    EVP_PKEY* m_key { nullptr };
    ByteBuffer m_spki;
};

// Keys are slow to make: share them.
Key const& first_key()
{
    static auto* key = new Key;
    return *key;
}

Key const& second_key()
{
    static auto* key = new Key;
    return *key;
}

class Writer {
public:
    void u16_(u16 value)
    {
        m_buffer.append(value & 0xff);
        m_buffer.append(value >> 8);
    }
    void u32_(u32 value)
    {
        u16_(value & 0xffff);
        u16_(value >> 16);
    }
    void bytes(ReadonlyBytes bytes) { m_buffer.append(bytes); }
    size_t size() const { return m_buffer.size(); }
    ByteBuffer take() { return move(m_buffer); }

private:
    ByteBuffer m_buffer;
};

struct File {
    ByteString name;
    ByteString contents;
};

// A ZIP of stored entries.
ByteBuffer zip_of(Vector<File> const& files)
{
    Writer writer;
    Vector<u32> offsets;
    for (auto const& file : files) {
        offsets.append(writer.size());
        writer.u32_(0x04034b50);
        writer.u16_(20);
        writer.u16_(0);
        writer.u16_(0);
        writer.u32_(0);
        writer.u32_(WebExtensions::crc32(file.contents.bytes()));
        writer.u32_(file.contents.length());
        writer.u32_(file.contents.length());
        writer.u16_(file.name.length());
        writer.u16_(0);
        writer.bytes(file.name.bytes());
        writer.bytes(file.contents.bytes());
    }
    auto central_directory_offset = writer.size();
    for (size_t i = 0; i < files.size(); ++i) {
        auto const& file = files[i];
        writer.u32_(0x02014b50);
        writer.u16_(20);
        writer.u16_(20);
        writer.u16_(0);
        writer.u16_(0);
        writer.u32_(0);
        writer.u32_(WebExtensions::crc32(file.contents.bytes()));
        writer.u32_(file.contents.length());
        writer.u32_(file.contents.length());
        writer.u16_(file.name.length());
        writer.u16_(0);
        writer.u16_(0);
        writer.u16_(0);
        writer.u16_(0);
        writer.u32_(0);
        writer.u32_(offsets[i]);
        writer.bytes(file.name.bytes());
    }
    auto central_directory_size = writer.size() - central_directory_offset;
    writer.u32_(0x06054b50);
    writer.u16_(0);
    writer.u16_(0);
    writer.u16_(files.size());
    writer.u16_(files.size());
    writer.u32_(central_directory_size);
    writer.u32_(central_directory_offset);
    writer.u16_(0);
    return writer.take();
}

void append_varint(ByteBuffer& buffer, u64 value)
{
    while (value >= 0x80) {
        buffer.append(static_cast<u8>(value | 0x80));
        value >>= 7;
    }
    buffer.append(static_cast<u8>(value));
}

void append_bytes_field(ByteBuffer& buffer, u32 field, ReadonlyBytes bytes)
{
    append_varint(buffer, (field << 3) | 2);
    append_varint(buffer, bytes.size());
    buffer.append(bytes);
}

void append_u32le(ByteBuffer& buffer, u32 value)
{
    for (int i = 0; i < 4; ++i)
        buffer.append(static_cast<u8>(value >> (8 * i)));
}

// A CRX3 of the files, signed with the key.
ByteBuffer crx_of(Key const& key, Vector<File> const& files)
{
    auto archive = zip_of(files);
    ByteBuffer signed_header_data;
    auto crx_id = key.crx_id();
    append_bytes_field(signed_header_data, 1, crx_id);

    ByteBuffer message;
    message.append("CRX3 SignedData\0"sv.bytes());
    append_u32le(message, signed_header_data.size());
    message.append(signed_header_data);
    message.append(archive);

    ByteBuffer proof;
    append_bytes_field(proof, 1, key.spki());
    auto signature = key.sign(message);
    append_bytes_field(proof, 2, signature);
    ByteBuffer header;
    append_bytes_field(header, 2, proof);
    append_bytes_field(header, 10000, signed_header_data);

    ByteBuffer crx;
    crx.append("Cr24"sv.bytes());
    append_u32le(crx, 3);
    append_u32le(crx, header.size());
    crx.append(header);
    crx.append(archive);
    return crx;
}

ByteString manifest(StringView version, StringView extra = {})
{
    return ByteString::formatted(R"({{ "manifest_version": 3, "name": "Install test", "version": "{}"{} }})", version, extra);
}

ByteBuffer extension_crx(Key const& key, StringView version, StringView extra_manifest = {})
{
    return crx_of(key, { { "manifest.json", manifest(version, extra_manifest) }, { "sub/a.txt", ByteString::formatted("a {}", version) } });
}

ByteString new_directory(ByteString const& parent, StringView name)
{
    auto path = ByteString::formatted("{}/{}", parent, name);
    MUST(Core::Directory::create(path, Core::Directory::CreateDirectories::Yes));
    return MUST(FileSystem::real_path(path));
}

ByteString extensions_directory_in(ByteString const& parent, StringView name)
{
    return ByteString::formatted("{}/Extensions", new_directory(parent, name));
}

mode_t mode_of(ByteString const& path)
{
    auto st = Core::System::lstat(path);
    if (st.is_error())
        return 0xffff;
    return st.value().st_mode & 07777;
}

// Whether every folder below the path is 0700 and every file 0600.
bool is_private_tree(ByteString const& path)
{
    if (mode_of(path) != 0700)
        return false;
    bool all_private = true;
    Core::DirIterator iterator(path, Core::DirIterator::SkipParentAndBaseDir);
    while (iterator.has_next()) {
        auto child = iterator.next_full_path();
        if (FileSystem::is_directory(child)) {
            all_private = all_private && is_private_tree(child);
        } else if (mode_of(child) != 0600) {
            warnln("{} is {:o}", child, mode_of(child));
            all_private = false;
        }
    }
    return all_private;
}

size_t entries_in(ByteString const& path)
{
    size_t count = 0;
    Core::DirIterator iterator(path, Core::DirIterator::SkipParentAndBaseDir);
    while (iterator.has_next()) {
        (void)iterator.next_path();
        ++count;
    }
    return count;
}

ErrorOr<WebView::Extension const*> install(ExtensionRegistry& registry, ByteBuffer const& crx, Optional<String> expected_id = {})
{
    return registry.install_crx(crx, expected_id.has_value() ? Optional<StringView> { expected_id->bytes_as_string_view() } : OptionalNone {});
}

ErrorOr<WebView::Extension const*> install(ExtensionRegistry& registry, ReadonlyBytes crx)
{
    return registry.install_crx(crx);
}

void crx_files_install_into_the_profile(ByteString const& directory)
{
    auto const& key = first_key();
    auto extensions_directory = extensions_directory_in(directory, "crx"sv);
    ExtensionRegistry registry;
    size_t changes = 0;
    registry.on_change = [&] { ++changes; };

    // Nowhere to install yet.
    CHECK(install(registry, extension_crx(key, "1.0"sv)).is_error());

    registry.set_extensions_directory(extensions_directory);
    auto installed = install(registry, extension_crx(key, "1.0"sv));
    CHECK(!installed.is_error());
    if (installed.is_error()) {
        warnln("install_crx: {}", installed.error());
        return;
    }
    auto const* extension = installed.value();
    auto version_directory = ByteString::formatted("{}/{}/1.0_0", extensions_directory, key.extension_id());
    CHECK_EQ(extension->id(), key.extension_id());
    CHECK_EQ(extension->root(), version_directory);
    CHECK_EQ(extension->read_file("sub/a.txt"sv).value_or("(none)"_string), "a 1.0"sv);
    CHECK(registry.find(key.extension_id()) == extension);
    CHECK_EQ(changes, 1u);

    auto record = registry.installed_extension(key.extension_id());
    CHECK(record.has_value());
    if (record.has_value()) {
        CHECK_EQ(record->version, "1.0"sv);
        CHECK_EQ(record->name, "Install test"sv);
        CHECK_EQ(record->path, version_directory);
        CHECK(record->install_type == InstalledExtension::InstallType::Normal);
        CHECK_EQ(record->install_type_name(), "normal"sv);
        CHECK(record->enabled);
        CHECK(!record->allows_file_access);
        CHECK(record->install_time <= UnixDateTime::now());
    }
    CHECK_EQ(registry.installed_extensions().size(), 1u);

    // Only the user may read what's installed.
    CHECK_EQ(mode_of(extensions_directory), 0700u);
    CHECK(is_private_tree(ByteString::formatted("{}/{}", extensions_directory, key.extension_id())));
    CHECK_EQ(mode_of(ByteString::formatted("{}/Installed.json", extensions_directory)), 0600u);

    // An extension's own "key" must be the CRX's.
    auto const& other_key = second_key();
    auto with_other_key = extension_crx(other_key, "1.0"sv, ByteString::formatted(R"(, "key": "{}")", key.base64_key()));
    CHECK(install(registry, with_other_key).is_error());
    CHECK(!FileSystem::exists(ByteString::formatted("{}/{}", extensions_directory, other_key.extension_id())));
    auto with_own_key = extension_crx(other_key, "1.0"sv, ByteString::formatted(R"(, "key": "{}")", other_key.base64_key()));
    auto own_key_result = install(registry, with_own_key);
    CHECK(!own_key_result.is_error());
    if (!own_key_result.is_error())
        CHECK_EQ(own_key_result.value()->id(), other_key.extension_id());
    CHECK(!registry.uninstall(other_key.extension_id()).is_error());

    // A CRX of another ID than expected.
    CHECK(install(registry, extension_crx(other_key, "1.0"sv), key.extension_id()).is_error());
    CHECK(registry.find(other_key.extension_id()) == nullptr);

    // Broken CRXs.
    auto tampered = extension_crx(other_key, "1.0"sv);
    tampered[tampered.size() - 30] ^= 1;
    CHECK(install(registry, tampered).is_error());
    CHECK(install(registry, "Cr24"sv.bytes()).is_error());
    CHECK(install(registry, ReadonlyBytes {}).is_error());
    CHECK(install(registry, crx_of(other_key, { { "readme.txt", "no manifest" } })).is_error());
    CHECK(install(registry, crx_of(other_key, { { "manifest.json", "{ not json" } })).is_error());
    CHECK(install(registry, crx_of(other_key, { { "manifest.json", manifest("1.0"sv) }, { "../escaped.txt", "out" } })).is_error());
    CHECK(install(registry, crx_of(other_key, { { "manifest.json", manifest("1.0"sv) }, { "/absolute.txt", "out" } })).is_error());
    CHECK(install(registry, crx_of(other_key, { { "manifest.json", manifest("../../1"sv) } })).is_error());
    CHECK(!FileSystem::exists(ByteString::formatted("{}/escaped.txt", extensions_directory)));
    CHECK(!FileSystem::exists(ByteString::formatted("{}/{}", extensions_directory, other_key.extension_id())));
    CHECK(registry.find(other_key.extension_id()) == nullptr);
    CHECK_EQ(registry.installed_extensions().size(), 1u);
}

void updates_replace_the_old_version_once_the_new_one_loads(ByteString const& directory)
{
    auto const& key = first_key();
    auto id = key.extension_id();
    auto extensions_directory = extensions_directory_in(directory, "update"sv);
    ExtensionRegistry registry;
    registry.set_extensions_directory(extensions_directory);
    size_t changes = 0;
    registry.on_change = [&] { ++changes; };

    MUST(install(registry, extension_crx(key, "1.0"sv)));
    auto install_time = registry.installed_extension(id)->install_time;
    registry.set_allows_file_access(id, true);

    auto updated = install(registry, extension_crx(key, "2.0"sv));
    CHECK(!updated.is_error());
    auto extension_directory = ByteString::formatted("{}/{}", extensions_directory, id);
    CHECK_EQ(registry.find(id)->manifest().version, "2.0"sv);
    CHECK_EQ(registry.find(id)->root(), ByteString::formatted("{}/2.0_0", extension_directory));
    CHECK_EQ(registry.find(id)->read_file("sub/a.txt"sv).value_or("(none)"_string), "a 2.0"sv);
    CHECK(!FileSystem::exists(ByteString::formatted("{}/1.0_0", extension_directory)));
    CHECK_EQ(entries_in(extension_directory), 1u);
    CHECK_EQ(registry.installed_extension(id)->version, "2.0"sv);
    CHECK(registry.installed_extension(id)->install_time == install_time);
    CHECK(registry.allows_file_access(id));
    CHECK_EQ(changes, 3u);

    // A version that doesn't load (its default locale has no catalog) leaves the installed one alone.
    CHECK(install(registry, extension_crx(key, "3.0"sv, R"(, "default_locale": "en")"sv)).is_error());
    CHECK_EQ(registry.find(id)->manifest().version, "2.0"sv);
    CHECK_EQ(registry.installed_extension(id)->version, "2.0"sv);
    CHECK(FileSystem::exists(ByteString::formatted("{}/2.0_0/sub/a.txt", extension_directory)));
    CHECK_EQ(entries_in(extension_directory), 1u);

    // No downgrades.
    CHECK(install(registry, extension_crx(key, "1.5"sv)).is_error());
    CHECK_EQ(registry.find(id)->manifest().version, "2.0"sv);

    // The same version again reinstalls it.
    auto reinstalled = install(registry, extension_crx(key, "2.0"sv));
    CHECK(!reinstalled.is_error());
    CHECK_EQ(registry.find(id)->manifest().version, "2.0"sv);
    CHECK_EQ(entries_in(extension_directory), 1u);
    CHECK(FileSystem::exists(registry.find(id)->root()));

    // A disabled extension is updated, and stays disabled.
    MUST(registry.set_enabled(id, false));
    CHECK(!install(registry, extension_crx(key, "4.0"sv)).is_error());
    CHECK(registry.find(id) == nullptr);
    CHECK_EQ(registry.installed_extension(id)->version, "4.0"sv);
    CHECK(!registry.installed_extension(id)->enabled);
    MUST(registry.set_enabled(id, true));
    CHECK_EQ(registry.find(id)->manifest().version, "4.0"sv);
}

// An update that asks for more than the user agreed to is installed, but stays off until they approve it (as Chrome).
void updates_asking_for_more_wait_for_approval(ByteString const& directory)
{
    auto const& key = first_key();
    auto id = key.extension_id();
    auto extensions_directory = extensions_directory_in(directory, "approval"sv);
    auto with = [](StringView permissions, StringView hosts, StringView matches, StringView extra = {}) {
        return ByteString::formatted(R"(, "permissions": [{}], "host_permissions": [{}], "content_scripts": [{{ "matches": [{}], "js": ["sub/a.txt"] }}]{})", permissions, hosts, matches, extra);
    };
    auto version = [&](StringView number, ByteString const& extra) { return extension_crx(key, number, extra); };
    {
        ExtensionRegistry registry;
        registry.set_extensions_directory(extensions_directory);
        MUST(install(registry, version("1.0"sv, with(R"("storage", "tabs")"sv, R"("https://a.example/*")"sv, R"("https://b.example/*")"sv))));
        CHECK(!registry.installed_extension(id)->needs_approval());

        // Optional permissions, and permissions without warnings, don't need approval.
        MUST(install(registry, version("2.0"sv, with(R"("storage", "tabs", "alarms")"sv, R"("https://a.example/*")"sv, R"("https://b.example/*")"sv, R"(, "optional_permissions": ["history"], "optional_host_permissions": ["<all_urls>"])"sv))));
        CHECK(registry.find(id) != nullptr);
        CHECK(!registry.installed_extension(id)->needs_approval());
        // Fewer don't either.
        MUST(install(registry, version("2.1"sv, with(R"("tabs")"sv, ""sv, R"("https://b.example/*")"sv))));
        CHECK(registry.find(id) != nullptr && registry.find(id)->manifest().version == "2.1"sv);
        CHECK(!registry.installed_extension(id)->needs_approval());

        // A new API permission, a new site and a new content script site.
        MUST(install(registry, version("3.0"sv, with(R"("tabs", "history")"sv, R"("https://c.example/*")"sv, R"("https://b.example/*", "https://d.example/*")"sv))));
        CHECK(registry.find(id) == nullptr);
        auto record = registry.installed_extension(id);
        CHECK(record.has_value() && record->version == "3.0"sv && !record->enabled && record->needs_approval());
        CHECK(record->permissions_to_approve == Vector<String> { "history"_string });
        CHECK(record->sites_to_approve == (Vector<String> { "https://c.example/*"_string, "https://d.example/*"_string }));
        // Turning it on isn't approving it.
        CHECK(registry.set_enabled(id, true).is_error());
        CHECK(registry.find(id) == nullptr);
    }
    {
        // It's remembered.
        ExtensionRegistry registry;
        registry.set_extensions_directory(extensions_directory);
        MUST(registry.load_installed_extensions());
        CHECK(registry.find(id) == nullptr);
        auto record = registry.installed_extension(id);
        CHECK(record.has_value() && !record->enabled && record->sites_to_approve.size() == 2 && record->permissions_to_approve.size() == 1);

        // Another update before approval keeps what's still to approve, and adds to it: all sites, this time.
        MUST(install(registry, version("4.0"sv, with(R"("tabs", "history", "downloads.open")"sv, R"("https://c.example/*", "http://*/*")"sv, R"("https://b.example/*")"sv))));
        record = registry.installed_extension(id);
        CHECK(record.has_value() && !record->enabled);
        CHECK(record->permissions_to_approve == (Vector<String> { "history"_string, "downloads.open"_string }));
        CHECK(record->sites_to_approve == (Vector<String> { "https://c.example/*"_string, "http://*/*"_string }));

        CHECK(registry.approve_permissions("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv).is_error());
        MUST(registry.approve_permissions(id));
        CHECK(registry.find(id) != nullptr && registry.find(id)->manifest().version == "4.0"sv);
        CHECK(registry.installed_extension(id)->enabled && !registry.installed_extension(id)->needs_approval());

        // Once all sites are granted, more sites need nothing.
        MUST(install(registry, version("5.0"sv, with(R"("tabs", "history", "downloads.open")"sv, R"("http://*/*", "https://e.example/*")"sv, R"("https://b.example/*")"sv))));
        CHECK(registry.find(id) != nullptr && !registry.installed_extension(id)->needs_approval());
    }
    {
        ExtensionRegistry registry;
        registry.set_extensions_directory(extensions_directory);
        MUST(registry.load_installed_extensions());
        CHECK(registry.find(id) != nullptr && !registry.installed_extension(id)->needs_approval());
        MUST(registry.uninstall(id));
    }
}

// Removing a folder removes the links in it, never what they name.
void removing_never_follows_links(ByteString const& directory)
{
    auto outside = ByteString::formatted("{}/outside", directory);
    MUST(Core::Directory::create(ByteString::formatted("{}/inner", outside), Core::Directory::CreateDirectories::Yes));
    write_temporary_file(outside, "keep.txt"sv, "keep"sv);
    auto doomed = ByteString::formatted("{}/doomed", directory);
    MUST(Core::Directory::create(ByteString::formatted("{}/a/b/c", doomed), Core::Directory::CreateDirectories::Yes));
    write_temporary_file(ByteString::formatted("{}/a/b/c", doomed), "file.txt"sv, "x"sv);
    MUST(Core::System::symlink(outside, ByteString::formatted("{}/a/link-to-folder", doomed)));
    MUST(Core::System::symlink(ByteString::formatted("{}/keep.txt", outside), ByteString::formatted("{}/a/b/link-to-file", doomed)));
    MUST(Core::System::symlink("/nonexistent"sv, ByteString::formatted("{}/dangling", doomed)));

    CHECK(!ExtensionRegistry::remove_without_following_links(doomed).is_error());
    CHECK(!FileSystem::exists(doomed) && !FileSystem::is_link(doomed));
    CHECK(FileSystem::exists(ByteString::formatted("{}/keep.txt", outside)));
    CHECK(FileSystem::is_directory(ByteString::formatted("{}/inner", outside)));

    // A link given itself is removed, not followed.
    auto link = ByteString::formatted("{}/link", directory);
    MUST(Core::System::symlink(outside, link));
    CHECK(!ExtensionRegistry::remove_without_following_links(link).is_error());
    CHECK(!FileSystem::is_link(link));
    CHECK(FileSystem::exists(ByteString::formatted("{}/keep.txt", outside)));
    CHECK(ExtensionRegistry::remove_without_following_links(ByteString::formatted("{}/missing", directory)).is_error());
}

// What the install and permission prompts say an extension can do.
void permissions_are_described()
{
    using WebView::summarize_permissions;
    // Every permission the browser gates something on has words.
    for (auto permission : { "alarms"sv, "contextMenus"sv, "declarativeNetRequest"sv, "declarativeNetRequestWithHostAccess"sv,
             "declarativeNetRequestFeedback"sv, "downloads"sv, "downloads.open"sv, "management"sv, "nativeMessaging"sv,
             "notifications"sv, "privacy"sv, "scripting"sv, "storage"sv, "tabs"sv, "unlimitedStorage"sv, "webNavigation"sv,
             "webRequest"sv, "webRequestAuthProvider"sv, "activeTab"sv }) {
        if (!WebView::permission_description(permission).has_value())
            warnln("No description for {}", permission);
        CHECK(WebView::permission_description(permission).has_value());
    }
    CHECK(!WebView::permission_description("madeUp"sv).has_value());

    for (auto pattern : { "<all_urls>"sv, "*://*/*"sv, "https://*/*"sv, "http://*/*"sv, "https://*/path"sv }) {
        auto summary = summarize_permissions({}, Vector<String> { MUST(String::from_utf8(pattern)) });
        CHECK(summary.all_sites && summary.sites.is_empty());
    }
    // Every site is listed, once.
    Vector<String> sites;
    for (int i = 0; i < 8; ++i)
        sites.append(MUST(String::formatted("https://site{}.example/*", i)));
    sites.append("https://site0.example/*"_string);
    auto summary = summarize_permissions(Vector<String> { "tabs"_string, "madeUp"_string, "tabs"_string }, sites);
    CHECK(!summary.all_sites);
    CHECK_EQ(summary.sites.size(), 8u);
    CHECK_EQ(summary.descriptions.size(), 1u);
    CHECK(summary.undescribed == Vector<String> { "madeUp"_string });
}

void installs_are_remembered(ByteString const& directory)
{
    auto const& key = first_key();
    auto id = key.extension_id();
    auto unpacked_id = "gfjfknicjgjhdegnhefddbcndkiphpci"sv;
    auto extensions_directory = extensions_directory_in(directory, "remembered"sv);
    {
        ExtensionRegistry registry;
        registry.set_extensions_directory(extensions_directory);
        MUST(install(registry, extension_crx(key, "1.0"sv)));
        auto unpacked = registry.install_unpacked(fixture("install-unpacked"sv));
        CHECK(!unpacked.is_error());
        if (unpacked.is_error())
            return;
        CHECK_EQ(unpacked.value()->id(), unpacked_id);
        CHECK_EQ(registry.installed_extension(unpacked_id)->install_type_name(), "development"sv);
        CHECK_EQ(registry.installed_extension(unpacked_id)->path, MUST(FileSystem::real_path(fixture("install-unpacked"sv))));
        registry.set_allows_file_access(id, true);
        MUST(registry.set_enabled(unpacked_id, false));
    }

    ExtensionRegistry registry;
    registry.set_extensions_directory(extensions_directory);
    CHECK(registry.find(id) == nullptr);
    CHECK(!registry.load_installed_extensions().is_error());
    CHECK(registry.find(id) != nullptr);
    CHECK(registry.allows_file_access(id));
    CHECK_EQ(registry.installed_extensions().size(), 2u);
    CHECK(registry.find(unpacked_id) == nullptr);
    auto unpacked = registry.installed_extension(unpacked_id);
    CHECK(unpacked.has_value() && !unpacked->enabled && unpacked->install_type == InstalledExtension::InstallType::Development);
    MUST(registry.set_enabled(unpacked_id, true));
    CHECK(registry.find(unpacked_id) != nullptr);
    CHECK_EQ(registry.find(unpacked_id)->read_file("file.txt"sv).value_or("(none)"_string), "unpacked\n"sv);

    // A state file that isn't one, and records that point out of the profile.
    auto state_file = ByteString::formatted("{}/Installed.json", extensions_directory);
    {
        auto file = MUST(Core::File::open(state_file, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(file->write_until_depleted("{ not json"sv.bytes()));
    }
    ExtensionRegistry broken;
    broken.set_extensions_directory(extensions_directory);
    CHECK(broken.load_installed_extensions().is_error());
    CHECK(broken.installed_extensions().is_empty());

    {
        auto outside = new_directory(directory, "outside"sv);
        MUST(FileSystem::copy_file_or_directory(ByteString::formatted("{}/copy", outside), registry.installed_extension(id)->path));
        auto json = ByteString::formatted(R"({{ "extensions": [
            {{ "id": "{}", "version": "1.0", "path": "{}/copy", "installType": "normal", "enabled": true, "allowsFileAccess": false, "installTime": 0 }},
            {{ "id": "../../etc", "version": "1.0", "path": "/etc", "installType": "development", "enabled": true }},
            {{ "id": 5 }}, "nonsense"
        ] }})", id, outside);
        auto file = MUST(Core::File::open(state_file, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(file->write_until_depleted(json.bytes()));
    }
    ExtensionRegistry tampered;
    tampered.set_extensions_directory(extensions_directory);
    (void)tampered.load_installed_extensions();
    CHECK(tampered.find(id) == nullptr);
    CHECK(tampered.find("../../etc"sv) == nullptr);
}

void disabled_extensions_are_not_served(ByteString const& directory)
{
    auto const& key = first_key();
    auto id = key.extension_id();
    ExtensionRegistry registry;
    registry.set_extensions_directory(extensions_directory_in(directory, "disabled"sv));
    MUST(install(registry, extension_crx(key, "1.0"sv)));
    size_t changes = 0;
    registry.on_change = [&] { ++changes; };

    CHECK(!registry.open_file(id, "/sub/a.txt"sv, {}).is_error());
    CHECK(registry.serialize_for_renderers().contains(id));
    MUST(registry.set_enabled(id, false));
    CHECK_EQ(changes, 1u);
    CHECK(registry.find(id) == nullptr);
    CHECK(registry.open_file(id, "/sub/a.txt"sv, {}).is_error());
    CHECK(!registry.serialize_for_renderers().contains(id));
    MUST(registry.set_enabled(id, false));
    CHECK_EQ(changes, 1u);
    MUST(registry.set_enabled(id, true));
    CHECK_EQ(changes, 2u);
    CHECK(!registry.open_file(id, "/sub/a.txt"sv, {}).is_error());

    CHECK(registry.set_enabled("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv, false).is_error());
    CHECK(registry.set_enabled(""sv, true).is_error());
}

void uninstalling_removes_only_crx_files(ByteString const& directory)
{
    auto const& key = first_key();
    auto id = key.extension_id();
    auto extensions_directory = extensions_directory_in(directory, "uninstall"sv);
    ExtensionRegistry registry;
    registry.set_extensions_directory(extensions_directory);
    MUST(install(registry, extension_crx(key, "1.0"sv)));
    MUST(registry.install_unpacked(fixture("install-unpacked"sv)));
    size_t changes = 0;
    registry.on_change = [&] { ++changes; };

    CHECK(!registry.uninstall(id).is_error());
    CHECK(registry.find(id) == nullptr);
    CHECK(!registry.installed_extension(id).has_value());
    CHECK(!FileSystem::exists(ByteString::formatted("{}/{}", extensions_directory, id)));
    CHECK(registry.uninstall(id).is_error());

    CHECK(!registry.uninstall("gfjfknicjgjhdegnhefddbcndkiphpci"sv).is_error());
    CHECK(FileSystem::exists(ByteString::formatted("{}/manifest.json", fixture("install-unpacked"sv))));
    CHECK(registry.installed_extensions().is_empty());
    CHECK_EQ(changes, 2u);

    ExtensionRegistry reloaded;
    reloaded.set_extensions_directory(extensions_directory);
    MUST(reloaded.load_installed_extensions());
    CHECK(reloaded.installed_extensions().is_empty());

    CHECK(registry.uninstall("not an id"sv).is_error());
}

void unpacked_installs_stay_in_their_folder(ByteString const& directory)
{
    auto extensions_directory = extensions_directory_in(directory, "unpacked"sv);
    ExtensionRegistry registry;
    registry.set_extensions_directory(extensions_directory);

    auto secret = write_temporary_file(new_directory(directory, "secret"sv), "manifest.json"sv, manifest("1.0"sv));

    auto linked_manifest = new_directory(directory, "linked-manifest"sv);
    MUST(Core::System::symlink(secret, ByteString::formatted("{}/manifest.json", linked_manifest)));
    CHECK(registry.install_unpacked(linked_manifest).is_error());

    auto linked_locales = new_directory(directory, "linked-locales"sv);
    write_temporary_file(linked_locales, "manifest.json"sv, manifest("1.0"sv, R"(, "default_locale": "en")"sv));
    auto locales = new_directory(directory, "secret-locales/en"sv);
    write_temporary_file(locales, "messages.json"sv, R"({ "a": { "message": "secret" } })"sv);
    MUST(Core::System::symlink(LexicalPath::dirname(locales), ByteString::formatted("{}/_locales", linked_locales)));
    CHECK(registry.install_unpacked(linked_locales).is_error());
    // The harness's clean-up doesn't remove links to folders.
    MUST(Core::System::unlink(ByteString::formatted("{}/_locales", linked_locales)));

    CHECK(registry.install_unpacked(ByteString::formatted("{}/missing", directory)).is_error());
    CHECK(registry.installed_extensions().is_empty());

    // Unpacked and CRX installs of one ID don't replace each other.
    auto const& key = first_key();
    auto unpacked_copy = new_directory(directory, "unpacked-copy"sv);
    write_temporary_file(unpacked_copy, "manifest.json"sv, manifest("9.0"sv, ByteString::formatted(R"(, "key": "{}")", key.base64_key())));
    MUST(install(registry, extension_crx(key, "1.0"sv)));
    CHECK(registry.install_unpacked(unpacked_copy).is_error());
    CHECK_EQ(registry.find(key.extension_id())->manifest().version, "1.0"sv);
    MUST(registry.uninstall(key.extension_id()));
    MUST(registry.install_unpacked(unpacked_copy));
    CHECK(install(registry, extension_crx(key, "10.0"sv)).is_error());
    CHECK_EQ(registry.find(key.extension_id())->manifest().version, "9.0"sv);
    CHECK(FileSystem::exists(ByteString::formatted("{}/manifest.json", unpacked_copy)));

    // Reinstalling the same folder is fine.
    CHECK(!registry.install_unpacked(unpacked_copy).is_error());
}

void web_store_inputs_name_extensions()
{
    auto id = "aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv;
    auto id_of = [](StringView input) { return ExtensionRegistry::extension_id_from_web_store_input(input).value_or("(none)"_string); };
    CHECK_EQ(id_of(id), id);
    CHECK_EQ(id_of("  aeblfdkhhhdcdjpifhhbdiojplfjncoa\n"sv), id);
    CHECK_EQ(id_of("AEBLFDKHHHDCDJPIFHHBDIOJPLFJNCOA"sv), id);
    CHECK_EQ(id_of("https://chromewebstore.google.com/detail/1password-%E2%80%93-password-mana/aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv), id);
    CHECK_EQ(id_of("https://chromewebstore.google.com/detail/1password/aeblfdkhhhdcdjpifhhbdiojplfjncoa?hl=en&authuser=1"sv), id);
    CHECK_EQ(id_of("https://chromewebstore.google.com/detail/1password/aeblfdkhhhdcdjpifhhbdiojplfjncoa/"sv), id);
    CHECK_EQ(id_of("https://chromewebstore.google.com/detail/1password/aeblfdkhhhdcdjpifhhbdiojplfjncoa#reviews"sv), id);
    CHECK_EQ(id_of("https://chromewebstore.google.com/detail/aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv), id);
    CHECK_EQ(id_of("https://chromewebstore.google.com/detail/1password/aeblfdkhhhdcdjpifhhbdiojplfjncoa/reviews"sv), id);
    CHECK_EQ(id_of("chromewebstore.google.com/detail/1password/aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv), id);
    CHECK_EQ(id_of("https://chrome.google.com/webstore/detail/1password/aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv), id);

    CHECK_EQ(id_of(""sv), "(none)"sv);
    CHECK_EQ(id_of("aeblfdkhhhdcdjpifhhbdiojplfjncoz"sv), "(none)"sv);
    CHECK_EQ(id_of("aeblfdkhhhdcdjpifhhbdiojplfjnco"sv), "(none)"sv);
    CHECK_EQ(id_of("https://evil.example/detail/1password/aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv), "(none)"sv);
    CHECK_EQ(id_of("https://chromewebstore.google.com.evil.example/detail/x/aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv), "(none)"sv);
    CHECK_EQ(id_of("https://chromewebstore.google.com/detail/1password/short"sv), "(none)"sv);
    CHECK_EQ(id_of("https://chromewebstore.google.com/search/aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv), "(none)"sv);
    CHECK_EQ(id_of("file:///detail/x/aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv), "(none)"sv);
}

ByteBuffer bytes_of(StringView string)
{
    return MUST(ByteBuffer::copy(string.bytes()));
}

// Answers requests from a table, a moment later, as the network would.
struct FakeStore {
    HashMap<String, ErrorOr<ExtensionRegistry::FetchResponse>> responses;
    Vector<String> requests;
    Vector<size_t> maximum_sizes;

    void respond(StringView url, u32 status, Optional<StringView> location = {}, ByteBuffer body = {})
    {
        ExtensionRegistry::FetchResponse response;
        response.status = status;
        if (location.has_value())
            response.location = MUST(String::from_utf8(*location));
        response.body = move(body);
        responses.set(MUST(String::from_utf8(url)), move(response));
    }

    ExtensionRegistry::Fetcher fetcher()
    {
        return [this](URL::URL const& url, size_t maximum_size, ExtensionRegistry::FetchCallback callback) {
            auto serialized = url.serialize();
            requests.append(serialized);
            maximum_sizes.append(maximum_size);
            Core::deferred_invoke([this, serialized, maximum_size, callback = move(callback)]() mutable {
                auto it = responses.find(serialized);
                if (it == responses.end()) {
                    callback(Error::from_string_literal("No such page"));
                    return;
                }
                if (it->value.is_error()) {
                    callback(Error::copy(it->value.error()));
                    return;
                }
                auto const& response = it->value.value();
                if (response.body.size() > maximum_size) {
                    callback(Error::from_string_literal("Too large"));
                    return;
                }
                callback(ExtensionRegistry::FetchResponse { response.status, response.location, MUST(ByteBuffer::copy(response.body)) });
            });
        };
    }
};

String store_url_for(StringView id)
{
    return URL::Parser::basic_parse(ByteString::formatted("https://clients2.google.com/service/update2/crx?response=redirect&prodversion=141.0&acceptformat=crx3&x=id%3D{}%26uc", id))->serialize();
}

String update_url_for(StringView id, StringView version)
{
    return URL::Parser::basic_parse(ByteString::formatted("https://clients2.google.com/service/update2/crx?response=updatecheck&prodversion=141.0&acceptformat=crx3&x=id%3D{}%26v%3D{}%26uc", id, version))->serialize();
}

ErrorOr<WebView::Extension const*> install_from_store(ExtensionRegistry& registry, StringView input)
{
    Optional<ErrorOr<WebView::Extension const*>> result;
    registry.install_from_web_store(input, [&](ErrorOr<WebView::Extension const*> installed) { result = move(installed); });
    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(10);
    Core::EventLoop::current().spin_until([&] { return result.has_value() || MonotonicTime::now() > deadline; });
    if (!result.has_value())
        return Error::from_string_literal("No answer");
    return result.release_value();
}

void extensions_install_from_the_web_store(ByteString const& directory)
{
    auto const& key = first_key();
    auto const& other_key = second_key();
    auto id = key.extension_id();
    ExtensionRegistry registry;
    registry.set_extensions_directory(extensions_directory_in(directory, "store"sv));
    FakeStore store;
    registry.set_fetcher(store.fetcher());

    auto blob = "https://clients2.googleusercontent.com/crx/blobs/AZPVh/EXTENSION_1_0.crx"sv;
    auto crx = extension_crx(key, "1.0"sv);
    store.respond(store_url_for(id), 302, blob);
    store.respond(blob, 200, {}, crx);

    auto installed = install_from_store(registry, ByteString::formatted("https://chromewebstore.google.com/detail/test/{}?hl=en", id));
    CHECK(!installed.is_error());
    if (installed.is_error())
        warnln("install_from_web_store: {}", installed.error());
    else
        CHECK_EQ(installed.value()->id(), id);
    CHECK_EQ(store.requests.size(), 2u);
    if (store.requests.size() == 2) {
        CHECK_EQ(store.requests[0], store_url_for(id));
        CHECK_EQ(store.requests[1], blob);
    }
    CHECK(all_of(store.maximum_sizes, [](auto size) { return size == ExtensionRegistry::default_maximum_download_size; }));
    CHECK(registry.installed_extension(id).has_value());
    MUST(registry.uninstall(id));

    // The user sees what's asked for before anything installs, and saying no installs nothing.
    {
        store.respond(store_url_for(id), 302, blob);
        Vector<String> asked_about;
        Optional<ErrorOr<WebView::Extension const*>> result;
        auto install_answering = [&](bool answer) {
            result.clear();
            registry.install_from_web_store(id, [&](ErrorOr<WebView::Extension const*> installed) { result = move(installed); },
                [&](WebExtensions::Manifest const& manifest, Function<void(bool)> on_answer) {
                    asked_about.append(manifest.version);
                    on_answer(answer);
                });
            auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(10);
            Core::EventLoop::current().spin_until([&] { return result.has_value() || MonotonicTime::now() > deadline; });
        };
        install_answering(false);
        CHECK(result.has_value() && result->is_error());
        CHECK(!registry.installed_extension(id).has_value());
        install_answering(true);
        CHECK(result.has_value() && !result->is_error());
        CHECK(registry.installed_extension(id).has_value());
        CHECK_EQ(asked_about, (Vector<String> { "1.0"_string, "1.0"_string }));
        MUST(registry.uninstall(id));
    }

    // Downloads only over HTTPS, from Google's download hosts.
    auto refused = [&](StringView location) {
        store.respond(store_url_for(id), 302, location);
        store.respond(location, 200, {}, crx);
        auto result = install_from_store(registry, id);
        if (!result.is_error())
            warnln("Installed from {}", location);
        return result.is_error() && !registry.installed_extension(id).has_value();
    };
    CHECK(refused("http://clients2.googleusercontent.com/crx/blobs/x/A.crx"sv));
    CHECK(refused("https://evil.example/A.crx"sv));
    CHECK(refused("https://clients2.googleusercontent.com.evil.example/A.crx"sv));
    CHECK(refused("https://evil-googleusercontent.com/A.crx"sv));
    CHECK(refused("https://sites.googleusercontent.com/A.crx"sv));
    CHECK(refused("file:///etc/passwd"sv));
    CHECK(refused("data:application/octet-stream,Cr24"sv));
    CHECK(refused("https://user:password@clients2.googleusercontent.com/A.crx"sv));
    CHECK(refused("https://clients2.googleusercontent.com:8443/A.crx"sv));

    store.respond(store_url_for(id), 302, "https://edgedl.me.gvt1.com/edgedl/A.crx"sv);
    store.respond("https://edgedl.me.gvt1.com/edgedl/A.crx"sv, 200, {}, crx);
    CHECK(!install_from_store(registry, id).is_error());
    MUST(registry.uninstall(id));

    // Relative redirects resolve against the request.
    store.respond(store_url_for(id), 302, "/crx/relative.crx"sv);
    store.respond("https://clients2.google.com/crx/relative.crx"sv, 200, {}, crx);
    CHECK(!install_from_store(registry, id).is_error());
    MUST(registry.uninstall(id));

    // The store's CRX must be the extension asked for.
    store.respond(store_url_for(id), 302, blob);
    store.respond(blob, 200, {}, extension_crx(other_key, "1.0"sv));
    CHECK(install_from_store(registry, id).is_error());
    CHECK(registry.installed_extensions().is_empty());

    // Errors along the way.
    store.respond(store_url_for(id), 404);
    CHECK(install_from_store(registry, id).is_error());
    store.respond(store_url_for(id), 204);
    CHECK(install_from_store(registry, id).is_error());
    store.respond(store_url_for(id), 302);
    CHECK(install_from_store(registry, id).is_error());
    store.responses.set(store_url_for(id), Error::from_string_literal("Network down"));
    CHECK(install_from_store(registry, id).is_error());
    auto loop = store_url_for(id);
    store.respond(loop, 302, loop.bytes_as_string_view());
    CHECK(install_from_store(registry, id).is_error());
    store.respond(store_url_for(id), 200, {}, bytes_of("not a crx"sv));
    CHECK(install_from_store(registry, id).is_error());

    // Too large.
    store.respond(store_url_for(id), 302, blob);
    store.respond(blob, 200, {}, crx);
    registry.set_maximum_download_size(100);
    store.maximum_sizes.clear();
    CHECK(install_from_store(registry, id).is_error());
    CHECK(!store.maximum_sizes.is_empty() && store.maximum_sizes.last() == 100);
    registry.set_maximum_download_size(ExtensionRegistry::default_maximum_download_size);

    // Not an extension.
    store.requests.clear();
    CHECK(install_from_store(registry, "https://evil.example/"sv).is_error());
    CHECK(install_from_store(registry, ""sv).is_error());
    CHECK(store.requests.is_empty());
    CHECK(registry.installed_extensions().is_empty());
}

ByteString update_response(StringView id, StringView status, StringView codebase = {}, StringView version = {}, Optional<ByteBuffer> crx = {})
{
    ByteString hash;
    if (crx.has_value()) {
        u8 digest[32];
        unsigned int length = 0;
        VERIFY(EVP_Digest(crx->data(), crx->size(), digest, &length, EVP_sha256(), nullptr) == 1);
        hash = encode_hex({ digest, 32 });
    }
    return ByteString::formatted(R"(<?xml version="1.0" encoding="UTF-8"?><gupdate xmlns="http://www.google.com/update2/response" protocol="2.0" server="prod"><daystart elapsed_days="7237" elapsed_seconds="15778"/><app appid="{}" cohort="1::" cohortname="" status="ok"><updatecheck _esbAllowlist="true" codebase="{}" fp="1.{}" hash_sha256="{}" protected="0" size="{}" status="{}" version="{}"/></app></gupdate>)",
        id, codebase, hash, hash, crx.has_value() ? crx->size() : 0, status, version);
}

Vector<ExtensionRegistry::UpdateResult> check_for_updates(ExtensionRegistry& registry)
{
    Optional<Vector<ExtensionRegistry::UpdateResult>> results;
    registry.check_for_updates([&](Vector<ExtensionRegistry::UpdateResult> checked) { results = move(checked); });
    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(10);
    Core::EventLoop::current().spin_until([&] { return results.has_value() || MonotonicTime::now() > deadline; });
    if (!results.has_value()) {
        warnln("\033[31;1mFAIL\033[0m: check_for_updates never finished");
        ++s_failures;
        return {};
    }
    return results.release_value();
}

void installed_extensions_update_from_the_web_store(ByteString const& directory)
{
    auto const& key = first_key();
    auto const& other_key = second_key();
    auto id = key.extension_id();
    ExtensionRegistry registry;
    registry.set_extensions_directory(extensions_directory_in(directory, "updates"sv));
    FakeStore store;
    registry.set_fetcher(store.fetcher());

    // Nothing to check.
    CHECK(check_for_updates(registry).is_empty());
    CHECK(store.requests.is_empty());

    MUST(install(registry, extension_crx(key, "1.0"sv)));
    MUST(registry.install_unpacked(fixture("install-unpacked"sv)));

    auto codebase = "https://clients2.googleusercontent.com/crx/blobs/x/EXTENSION_2_0.crx"sv;
    auto version_2 = extension_crx(key, "2.0"sv);
    store.respond(update_url_for(id, "1.0"sv), 200, {}, bytes_of(update_response(id, "ok"sv, codebase, "2.0"sv, version_2)));
    store.respond(codebase, 200, {}, version_2);

    auto results = check_for_updates(registry);
    // Development installs aren't the store's.
    CHECK_EQ(store.requests.size(), 2u);
    CHECK_EQ(results.size(), 1u);
    if (results.size() == 1) {
        CHECK_EQ(results[0].id, id);
        CHECK_EQ(results[0].updated_to.value_or("(none)"_string), "2.0"sv);
        CHECK(!results[0].error.has_value());
    }
    CHECK_EQ(registry.find(id)->manifest().version, "2.0"sv);

    // No update.
    store.requests.clear();
    store.respond(update_url_for(id, "2.0"sv), 200, {}, bytes_of(update_response(id, "noupdate"sv)));
    results = check_for_updates(registry);
    CHECK_EQ(store.requests.size(), 1u);
    CHECK(results.size() == 1 && !results[0].updated_to.has_value() && !results[0].error.has_value());

    auto fails = [&](ByteString const& response, Optional<StringView> download = {}, Optional<ByteBuffer> download_crx = {}) {
        store.respond(update_url_for(id, "2.0"sv), 200, {}, bytes_of(response));
        if (download.has_value())
            store.respond(*download, 200, {}, download_crx.value_or({}));
        auto checked = check_for_updates(registry);
        return checked.size() == 1 && checked[0].error.has_value() && !checked[0].updated_to.has_value() && registry.find(id)->manifest().version == "2.0"sv;
    };
    auto version_3 = extension_crx(key, "3.0"sv);
    auto codebase_3 = "https://clients2.googleusercontent.com/crx/blobs/x/EXTENSION_3_0.crx"sv;
    // The download isn't what the response's hash names.
    CHECK(fails(update_response(id, "ok"sv, codebase_3, "3.0"sv, version_2), codebase_3, version_3));
    // From elsewhere.
    CHECK(fails(update_response(id, "ok"sv, "http://clients2.googleusercontent.com/x.crx"sv, "3.0"sv, version_3), "http://clients2.googleusercontent.com/x.crx"sv, version_3));
    CHECK(fails(update_response(id, "ok"sv, "https://evil.example/x.crx"sv, "3.0"sv, version_3), "https://evil.example/x.crx"sv, version_3));
    // Another extension's CRX.
    auto other_crx = extension_crx(other_key, "3.0"sv);
    CHECK(fails(update_response(id, "ok"sv, codebase_3, "3.0"sv, other_crx), codebase_3, other_crx));
    // Says it's newer, isn't.
    auto version_1 = extension_crx(key, "1.0"sv);
    CHECK(fails(update_response(id, "ok"sv, codebase_3, "3.0"sv, version_1), codebase_3, version_1));
    // Broken responses.
    CHECK(fails("<gupdate"));
    CHECK(fails("not xml at all"));
    CHECK(fails(update_response(id, "ok"sv, ""sv, "3.0"sv, version_3)));
    CHECK(fails(update_response(id, "ok"sv, codebase_3, "../3"sv, version_3), codebase_3, version_3));
    CHECK(fails(update_response(id, "error-unknownApplication"sv)));
    // Bytes that aren't UTF-8, in any attribute, are a broken response rather than a crash.
    CHECK(fails(update_response(id, "ok\xff"sv, codebase_3, "3.0"sv, version_3), codebase_3, version_3));
    {
        auto response = update_response(id, "ok"sv, codebase_3, "3.0"sv, version_3).replace("<app appid=\""sv, "<app appid=\"\xff"sv);
        store.respond(update_url_for(id, "2.0"sv), 200, {}, bytes_of(response));
        auto checked = check_for_updates(registry);
        CHECK(checked.size() == 1 && !checked[0].updated_to.has_value());
    }

    // An older or equal version on offer isn't downloaded.
    store.requests.clear();
    store.respond(update_url_for(id, "2.0"sv), 200, {}, bytes_of(update_response(id, "ok"sv, codebase, "2.0"sv, version_2)));
    results = check_for_updates(registry);
    CHECK_EQ(store.requests.size(), 1u);
    CHECK(results.size() == 1 && !results[0].updated_to.has_value());

    // Answers about other extensions are ignored.
    store.respond(update_url_for(id, "2.0"sv), 200, {}, bytes_of(update_response(other_key.extension_id(), "ok"sv, codebase_3, "3.0"sv, version_3)));
    store.respond(codebase_3, 200, {}, version_3);
    results = check_for_updates(registry);
    CHECK(results.size() == 1 && !results[0].updated_to.has_value());
    CHECK_EQ(registry.find(id)->manifest().version, "2.0"sv);

    // Versions compare by number.
    CHECK_EQ(registry.installed_extension(id)->version, "2.0"sv);
    auto version_10 = extension_crx(key, "10.0"sv);
    store.respond(update_url_for(id, "2.0"sv), 200, {}, bytes_of(update_response(id, "ok"sv, codebase_3, "10.0"sv, version_10)));
    store.respond(codebase_3, 200, {}, version_10);
    results = check_for_updates(registry);
    CHECK(results.size() == 1 && results[0].updated_to == "10.0"sv);
    CHECK_EQ(registry.find(id)->manifest().version, "10.0"sv);
}

void installed_extensions_run_in_the_browser(Harness& harness)
{
    Key key;
    auto id = key.extension_id();
    harness.registry.set_extensions_directory(extensions_directory_in(harness.directory, "app"sv));
    auto crx = crx_of(key, {
                               { "manifest.json", manifest("1.0"sv) },
                               { "page.html", "<!doctype html><script src=\"page.js\"></script>" },
                               { "page.js", "document.title = JSON.stringify({ origin: location.origin, id: chrome.runtime.id });" },
                           });
    auto installed = harness.registry.install_crx(crx);
    CHECK(!installed.is_error());
    if (installed.is_error())
        return;
    auto report = report_of(harness.view, url(ByteString::formatted("chrome-extension://{}/page.html", id)));
    CHECK_EQ(string_in(report, "origin"sv), MUST(String::formatted("chrome-extension://{}", id)));
    CHECK_EQ(string_in(report, "id"sv), id);
    MUST(harness.registry.uninstall(id));
}

// A real download from the Chrome Web Store, only when asked for: LADYBIRD_TEST_WEB_STORE_ID=<id or URL> and
// LADYBIRD_TEST_WEB_STORE_DIRECTORY=<an empty folder to install into>.
void a_real_extension_installs_from_the_web_store()
{
    auto input = Core::Environment::get("LADYBIRD_TEST_WEB_STORE_ID"sv);
    auto directory = Core::Environment::get("LADYBIRD_TEST_WEB_STORE_DIRECTORY"sv);
    if (!input.has_value() || !directory.has_value())
        return;
    ExtensionRegistry registry;
    registry.set_extensions_directory(ByteString::formatted("{}/Extensions", *directory));
    Optional<ErrorOr<WebView::Extension const*>> result;
    registry.install_from_web_store(*input, [&](ErrorOr<WebView::Extension const*> installed) { result = move(installed); });
    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(300);
    Core::EventLoop::current().spin_until([&] { return result.has_value() || MonotonicTime::now() > deadline; });
    CHECK(result.has_value() && !result->is_error());
    if (result.has_value() && result->is_error())
        warnln("Web Store install: {}", result->error());
    if (result.has_value() && !result->is_error()) {
        auto const* extension = result->value();
        outln("Installed {} {} ({}) at {}", extension->manifest().name, extension->manifest().version, extension->id(), extension->root());
        auto updates = check_for_updates(registry);
        for (auto const& update : updates)
            outln("Update check for {}: {}", update.id, update.error.has_value() ? *update.error : update.updated_to.value_or("up to date"_string));
    }
}

}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return ExtensionTests::run_extension_tests(arguments, [](ExtensionTests::Harness& harness) {
        using namespace ExtensionTests;
        // The browser doesn't load installs in tests.
        CHECK(!harness.registry.extensions_directory().has_value());
        CHECK(harness.registry.installed_extensions().is_empty());

        crx_files_install_into_the_profile(harness.directory);
        updates_replace_the_old_version_once_the_new_one_loads(harness.directory);
        updates_asking_for_more_wait_for_approval(harness.directory);
        permissions_are_described();
        removing_never_follows_links(harness.directory);
        installs_are_remembered(harness.directory);
        disabled_extensions_are_not_served(harness.directory);
        uninstalling_removes_only_crx_files(harness.directory);
        unpacked_installs_stay_in_their_folder(harness.directory);
        web_store_inputs_name_extensions();
        extensions_install_from_the_web_store(harness.directory);
        installed_extensions_update_from_the_web_store(harness.directory);
        installed_extensions_run_in_the_browser(harness);
        a_real_extension_installs_from_the_web_store();
    });
}
