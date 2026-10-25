/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/String.h>

namespace WebExtensions {

// A verified CRX3 package (https://chromium.googlesource.com/chromium/src/+/main/components/crx_file/).
struct CrxFile {
    static constexpr u32 max_header_size = 16 * MiB;

    // Parses the CRX3 header and verifies every signature over the archive. At least one proof must use the key
    // whose SHA-256 prefix is the declared crx_id.
    static ErrorOr<CrxFile> parse_and_verify(ReadonlyBytes);

    String extension_id;
    ByteBuffer public_key; // DER SubjectPublicKeyInfo of the key matching crx_id.
    ByteBuffer zip_archive;
};

}
