/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/CharacterTypes.h>
#include <AK/StringBuilder.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibWebExtensions/ExtensionId.h>

namespace WebExtensions {

static String id_from_bytes(ReadonlyBytes bytes)
{
    auto digest = Crypto::Hash::SHA256::hash(bytes);
    StringBuilder builder;
    for (size_t i = 0; i < 16; ++i) {
        u8 byte = digest.data[i];
        builder.append(static_cast<char>('a' + (byte >> 4)));
        builder.append(static_cast<char>('a' + (byte & 0xf)));
    }
    return builder.to_string_without_validation();
}

ErrorOr<String> extension_id_from_public_key(ReadonlyBytes der_spki)
{
    if (der_spki.is_empty())
        return Error::from_string_literal("Extension public key is empty");
    return id_from_bytes(der_spki);
}

ErrorOr<String> extension_id_from_base64_key(StringView key)
{
    // Drop PEM armor lines and all whitespace.
    StringBuilder base64;
    key.for_each_split_view('\n', SplitBehavior::Nothing, [&](StringView line) {
        if (line.trim_whitespace().starts_with("-----"sv))
            return;
        for (auto c : line) {
            if (!is_ascii_space(c))
                base64.append(c);
        }
    });
    if (base64.is_empty())
        return Error::from_string_literal("Extension key is empty");
    auto der = TRY(decode_base64(base64.string_view()));
    return extension_id_from_public_key(der.bytes());
}

String extension_id_from_path(StringView absolute_path)
{
    return id_from_bytes(absolute_path.bytes());
}

bool is_valid_extension_id(StringView id)
{
    if (id.length() != 32)
        return false;
    for (auto c : id) {
        if (c < 'a' || c > 'p')
            return false;
    }
    return true;
}

}
