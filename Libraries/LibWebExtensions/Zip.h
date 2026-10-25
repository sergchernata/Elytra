/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace WebExtensions {

// Read-only ZIP reader for untrusted archives (stored and deflate entries, ZIP64).
class ZipArchive {
public:
    static constexpr size_t max_entry_count = 50'000;
    static constexpr u64 max_total_uncompressed_size = 512 * MiB;
    // Entries above this size must not compress better than max_compression_ratio.
    static constexpr u64 compression_ratio_check_threshold = 4 * MiB;
    static constexpr u64 max_compression_ratio = 250;

    enum class Method : u16 {
        Stored = 0,
        Deflate = 8,
    };

    struct Entry {
        ByteString name;
        u16 method { 0 };
        u16 flags { 0 };
        u16 version_made_by { 0 };
        u32 crc32 { 0 };
        u32 external_attributes { 0 };
        u64 compressed_size { 0 };
        u64 uncompressed_size { 0 };
        u64 local_header_offset { 0 };

        bool is_directory() const { return name.ends_with('/'); }
        bool is_symlink() const;
    };

    // The bytes must outlive the returned archive.
    static ErrorOr<ZipArchive> parse(ReadonlyBytes);

    Vector<Entry> const& entries() const { return m_entries; }
    Optional<Entry const&> find(StringView name) const;
    u64 total_uncompressed_size() const { return m_total_uncompressed_size; }

    // Decompresses an entry and checks its size and CRC-32.
    ErrorOr<ByteBuffer> read(Entry const&) const;

    // Extracts into a new directory at destination, which must not exist yet. Entries are written to a temporary
    // sibling directory that is renamed into place, so a failed extraction leaves nothing behind.
    static ErrorOr<void> extract_to_directory(ReadonlyBytes zip, StringView destination);

    // Checks that an entry name is a safe relative path (no absolute paths, "..", backslashes, NUL bytes, etc.).
    static ErrorOr<void> validate_entry_name(StringView);

private:
    ZipArchive(ReadonlyBytes data, Vector<Entry> entries, u64 total_uncompressed_size)
        : m_data(data)
        , m_entries(move(entries))
        , m_total_uncompressed_size(total_uncompressed_size)
    {
    }

    ReadonlyBytes m_data;
    Vector<Entry> m_entries;
    u64 m_total_uncompressed_size { 0 };
};

u32 crc32(ReadonlyBytes);

}
