/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Parser.h>
#include <LibWebExtensions/ExtensionId.h>
#include <LibWebExtensions/MatchPattern.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/Utilities.h>

namespace WebView {

Extension::Extension(String id, ByteString root, WebExtensions::Manifest manifest)
    : m_id(move(id))
    , m_root(move(root))
    , m_manifest(move(manifest))
{
}

URL::Origin Extension::origin() const
{
    return URL::Parser::basic_parse(MUST(String::formatted("chrome-extension://{}/", m_id)))->origin();
}

// The URL the "matches" of web_accessible_resources are compared with. Local files have opaque origins here; Chrome
// matches them as file:///.
static Optional<URL::URL> url_for_origin(URL::Origin const& origin)
{
    if (origin.is_opaque()) {
        if (origin.is_file_origin())
            return URL::Parser::basic_parse("file:///"sv);
        return {};
    }
    return URL::Parser::basic_parse(MUST(String::formatted("{}/", origin.serialize())));
}

bool Extension::may_be_loaded_by(StringView path, Optional<URL::Origin> const& requester) const
{
    // No requester: the browser itself, opening one of the extension's pages.
    // FIXME: Once extension pages get processes of their own, also refuse a renderer that hosts none of this extension's.
    if (!requester.has_value() || requester->is_same_origin(origin()))
        return true;

    // Resources are named relative to the root.
    auto relative_path = path.trim("/"sv, TrimMode::Left);

    for (auto const& entry : m_manifest.web_accessible_resources) {
        bool listed = any_of(entry.resources, [&](auto const& resource) {
            return WebExtensions::glob_matches(resource.bytes_as_string_view().trim("/"sv, TrimMode::Left), relative_path);
        });
        if (!listed)
            continue;

        // Other extensions, by ID.
        if (!requester->is_opaque() && requester->scheme() == "chrome-extension"sv) {
            auto requester_id = requester->host().serialize();
            if (any_of(entry.extension_ids, [&](auto const& id) { return id == "*"sv || id == requester_id; }))
                return true;
            continue;
        }

        auto requester_url = url_for_origin(*requester);
        for (auto const& match : entry.matches) {
            auto pattern = WebExtensions::MatchPattern::parse(match);
            if (pattern.is_error())
                continue;
            if (requester_url.has_value() ? pattern.value().matches_ignoring_path(*requester_url) : pattern.value().matches_all_urls())
                return true;
        }
    }
    return false;
}

ErrorOr<Extension const*> ExtensionRegistry::load_unpacked(ByteString const& directory)
{
    auto root = TRY(FileSystem::real_path(directory));
    auto manifest_file = TRY(Core::File::open(ByteString::formatted("{}/manifest.json", root), Core::File::OpenMode::Read));
    auto manifest_json = TRY(manifest_file->read_until_eof());
    auto manifest = TRY(WebExtensions::Manifest::parse(manifest_json));

    auto id = manifest.key.has_value()
        ? TRY(WebExtensions::extension_id_from_base64_key(*manifest.key))
        : WebExtensions::extension_id_from_path(root);

    auto extension = make<Extension>(id, move(root), move(manifest));
    auto const* stored = extension.ptr();
    m_extensions.set(move(id), move(extension));
    return stored;
}

Extension const* ExtensionRegistry::find(StringView id) const
{
    auto it = m_extensions.find(id);
    return it == m_extensions.end() ? nullptr : it->value.ptr();
}

ErrorOr<IPC::File> ExtensionRegistry::open_file(StringView id, StringView path, Optional<URL::Origin> const& requester) const
{
    auto const* extension = find(id);
    if (!extension)
        return Error::from_errno(ENOENT);

    if (!path.starts_with('/') || path.contains('\0') || path.contains('\\'))
        return Error::from_errno(EACCES);
    for (auto part : path.split_view('/')) {
        if (part == ".."sv)
            return Error::from_errno(EACCES);
    }

    if (!extension->may_be_loaded_by(path, requester))
        return Error::from_errno(EACCES);

    // Resolve links, and serve nothing outside the extension's folder.
    auto real_path = TRY(FileSystem::real_path(ByteString::formatted("{}{}", extension->root(), path)));
    if (!real_path.starts_with(ByteString::formatted("{}/", extension->root())))
        return Error::from_errno(EACCES);

    return open_local_file_for_renderer(real_path);
}

}
