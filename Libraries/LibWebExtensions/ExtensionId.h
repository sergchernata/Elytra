/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <AK/StringView.h>

namespace WebExtensions {

// Chrome-style extension IDs: the first 16 bytes of a SHA-256 digest, with each
// nibble mapped 0..15 to 'a'..'p'.

// From a DER-encoded SubjectPublicKeyInfo.
ErrorOr<String> extension_id_from_public_key(ReadonlyBytes der_spki);

// From a manifest "key" value (base64 DER, optionally PEM armored; whitespace is ignored).
ErrorOr<String> extension_id_from_base64_key(StringView);

// For unpacked extensions without a key: hashes the path bytes.
String extension_id_from_path(StringView absolute_path);

// 32 characters in [a-p].
bool is_valid_extension_id(StringView);

}
