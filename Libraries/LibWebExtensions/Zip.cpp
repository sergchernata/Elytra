/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/HashTable.h>
#include <AK/MemoryStream.h>
#include <AK/ScopeGuard.h>
#include <AK/Utf8View.h>
#include <LibCompress/Deflate.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebExtensions/Zip.h>
#include <stdlib.h>
#include <sys/stat.h>

namespace WebExtensions {

static constexpr u32 local_file_header_signature = 0x04034b50;
static constexpr u32 central_directory_signature = 0x02014b50;
static constexpr u32 end_of_central_directory_signature = 0x06054b50;
static constexpr u32 zip64_end_of_central_directory_signature = 0x06064b50;
static constexpr u32 zip64_locator_signature = 0x07064b50;

static constexpr size_t end_of_central_directory_size = 22;
static constexpr size_t zip64_locator_size = 20;
static constexpr size_t zip64_end_of_central_directory_size = 56;
static constexpr size_t central_directory_entry_size = 46;
static constexpr size_t local_file_header_size = 30;
static constexpr size_t max_comment_size = 0xffff;
static constexpr size_t max_entry_name_length = 1024;

static constexpr u16 flag_encrypted = 1 << 0;
static constexpr u16 flag_strong_encryption = 1 << 6;
static constexpr u16 host_unix = 3;

static constexpr Array<u32, 256> crc32_table = [] {
    Array<u32, 256> table {};
    for (u32 i = 0; i < 256; ++i) {
        u32 value = i;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1) ? (0xedb88320 ^ (value >> 1)) : (value >> 1);
        table[i] = value;
    }
    return table;
}();

u32 crc32(ReadonlyBytes bytes)
{
    u32 value = 0xffffffff;
    for (auto byte : bytes)
        value = crc32_table[(value ^ byte) & 0xff] ^ (value >> 8);
    return ~value;
}

namespace {

// Bounds-checked little-endian reader over a byte span.
class Reader {
public:
    explicit Reader(ReadonlyBytes data)
        : m_data(data)
    {
    }

    static bool range_fits(u64 offset, u64 length, u64 size)
    {
        return offset <= size && length <= size - offset;
    }

    ErrorOr<u16> u16_at(u64 offset) const
    {
        if (!range_fits(offset, 2, m_data.size()))
            return Error::from_string_literal("ZIP: read out of bounds");
        return static_cast<u16>(m_data[offset] | (m_data[offset + 1] << 8));
    }

    ErrorOr<u32> u32_at(u64 offset) const
    {
        auto low = TRY(u16_at(offset));
        auto high = TRY(u16_at(offset + 2));
        return static_cast<u32>(low) | (static_cast<u32>(high) << 16);
    }

    ErrorOr<u64> u64_at(u64 offset) const
    {
        auto low = TRY(u32_at(offset));
        auto high = TRY(u32_at(offset + 4));
        return static_cast<u64>(low) | (static_cast<u64>(high) << 32);
    }

    ErrorOr<ReadonlyBytes> bytes_at(u64 offset, u64 length) const
    {
        if (!range_fits(offset, length, m_data.size()))
            return Error::from_string_literal("ZIP: read out of bounds");
        return m_data.slice(offset, length);
    }

private:
    ReadonlyBytes m_data;
};

struct EndOfCentralDirectory {
    u64 offset { 0 };
    u64 entry_count { 0 };
    u64 central_directory_size { 0 };
    u64 central_directory_offset { 0 };
};

}

static ErrorOr<Optional<EndOfCentralDirectory>> parse_end_of_central_directory_at(Reader const& reader, size_t size, size_t offset)
{
    if (TRY(reader.u32_at(offset)) != end_of_central_directory_signature)
        return OptionalNone {};
    auto comment_length = TRY(reader.u16_at(offset + 20));
    if (offset + end_of_central_directory_size + comment_length != size)
        return OptionalNone {};

    auto disk_number = TRY(reader.u16_at(offset + 4));
    auto central_directory_disk = TRY(reader.u16_at(offset + 6));
    auto entries_on_disk = TRY(reader.u16_at(offset + 8));
    auto entry_count = TRY(reader.u16_at(offset + 10));
    auto central_directory_size = TRY(reader.u32_at(offset + 12));
    auto central_directory_offset = TRY(reader.u32_at(offset + 16));

    EndOfCentralDirectory result {
        .offset = offset,
        .entry_count = entry_count,
        .central_directory_size = central_directory_size,
        .central_directory_offset = central_directory_offset,
    };

    bool needs_zip64 = disk_number == 0xffff || central_directory_disk == 0xffff || entries_on_disk == 0xffff
        || entry_count == 0xffff || central_directory_size == 0xffffffff || central_directory_offset == 0xffffffff;

    if (needs_zip64 && offset >= zip64_locator_size && TRY(reader.u32_at(offset - zip64_locator_size)) == zip64_locator_signature) {
        auto locator = offset - zip64_locator_size;
        if (TRY(reader.u32_at(locator + 4)) != 0 || TRY(reader.u32_at(locator + 16)) != 1)
            return Error::from_string_literal("ZIP: multi-disk archives are not supported");
        auto record = TRY(reader.u64_at(locator + 8));
        if (!Reader::range_fits(record, zip64_end_of_central_directory_size, locator))
            return Error::from_string_literal("ZIP: invalid ZIP64 end of central directory offset");
        if (TRY(reader.u32_at(record)) != zip64_end_of_central_directory_signature)
            return Error::from_string_literal("ZIP: invalid ZIP64 end of central directory signature");

        disk_number = TRY(reader.u32_at(record + 16)) == 0 ? 0 : 1;
        central_directory_disk = TRY(reader.u32_at(record + 20)) == 0 ? 0 : 1;
        auto entries_on_disk64 = TRY(reader.u64_at(record + 24));
        result.entry_count = TRY(reader.u64_at(record + 32));
        result.central_directory_size = TRY(reader.u64_at(record + 40));
        result.central_directory_offset = TRY(reader.u64_at(record + 48));
        // Treat the ZIP64 record as the end of the data that precedes it.
        result.offset = record;
        if (entries_on_disk64 != result.entry_count)
            return Error::from_string_literal("ZIP: multi-disk archives are not supported");
    } else if (entries_on_disk != entry_count) {
        return Error::from_string_literal("ZIP: multi-disk archives are not supported");
    }

    if (disk_number != 0 || central_directory_disk != 0)
        return Error::from_string_literal("ZIP: multi-disk archives are not supported");

    return result;
}

static ErrorOr<EndOfCentralDirectory> find_end_of_central_directory(Reader const& reader, size_t size)
{
    if (size < end_of_central_directory_size)
        return Error::from_string_literal("ZIP: archive too small");

    // The record sits at the end, followed only by a comment of up to 64 KiB.
    size_t last = size - end_of_central_directory_size;
    size_t first = last > max_comment_size ? last - max_comment_size : 0;
    for (size_t offset = last + 1; offset-- > first;) {
        if (auto result = TRY(parse_end_of_central_directory_at(reader, size, offset)); result.has_value())
            return result.release_value();
    }
    return Error::from_string_literal("ZIP: end of central directory not found");
}

static ErrorOr<void> apply_zip64_extra_field(Reader const& reader, u64 extra_offset, u64 extra_length, ZipArchive::Entry& entry, bool need_uncompressed, bool need_compressed, bool need_offset)
{
    u64 position = extra_offset;
    u64 end = extra_offset + extra_length;
    while (position + 4 <= end) {
        auto id = TRY(reader.u16_at(position));
        auto length = TRY(reader.u16_at(position + 2));
        position += 4;
        if (length > end - position)
            return Error::from_string_literal("ZIP: extra field out of bounds");
        if (id == 0x0001) {
            u64 field = position;
            u64 field_end = position + length;
            auto read_next = [&](u64& out) -> ErrorOr<void> {
                if (field + 8 > field_end)
                    return Error::from_string_literal("ZIP: truncated ZIP64 extra field");
                out = TRY(reader.u64_at(field));
                field += 8;
                return {};
            };
            if (need_uncompressed)
                TRY(read_next(entry.uncompressed_size));
            if (need_compressed)
                TRY(read_next(entry.compressed_size));
            if (need_offset)
                TRY(read_next(entry.local_header_offset));
            return {};
        }
        position += length;
    }
    if (need_uncompressed || need_compressed || need_offset)
        return Error::from_string_literal("ZIP: missing ZIP64 extra field");
    return {};
}

ErrorOr<ZipArchive> ZipArchive::parse(ReadonlyBytes data)
{
    Reader reader { data };
    auto end_record = TRY(find_end_of_central_directory(reader, data.size()));

    if (end_record.entry_count > max_entry_count)
        return Error::from_string_literal("ZIP: too many entries");
    if (!Reader::range_fits(end_record.central_directory_offset, end_record.central_directory_size, end_record.offset))
        return Error::from_string_literal("ZIP: central directory out of bounds");
    // Each entry needs at least its fixed-size record.
    if (end_record.entry_count > end_record.central_directory_size / central_directory_entry_size)
        return Error::from_string_literal("ZIP: entry count does not fit central directory");

    Vector<Entry> entries;
    TRY(entries.try_ensure_capacity(end_record.entry_count));
    HashTable<ByteString> names;
    u64 total_uncompressed_size = 0;

    u64 position = end_record.central_directory_offset;
    u64 end = end_record.central_directory_offset + end_record.central_directory_size;
    for (u64 i = 0; i < end_record.entry_count; ++i) {
        if (!Reader::range_fits(position, central_directory_entry_size, end))
            return Error::from_string_literal("ZIP: truncated central directory");
        if (TRY(reader.u32_at(position)) != central_directory_signature)
            return Error::from_string_literal("ZIP: invalid central directory entry signature");

        Entry entry;
        entry.version_made_by = TRY(reader.u16_at(position + 4));
        entry.flags = TRY(reader.u16_at(position + 8));
        entry.method = TRY(reader.u16_at(position + 10));
        entry.crc32 = TRY(reader.u32_at(position + 16));
        u32 compressed_size = TRY(reader.u32_at(position + 20));
        u32 uncompressed_size = TRY(reader.u32_at(position + 24));
        u16 name_length = TRY(reader.u16_at(position + 28));
        u16 extra_length = TRY(reader.u16_at(position + 30));
        u16 comment_length = TRY(reader.u16_at(position + 32));
        u16 disk_start = TRY(reader.u16_at(position + 34));
        entry.external_attributes = TRY(reader.u32_at(position + 38));
        u32 local_header_offset = TRY(reader.u32_at(position + 42));

        u64 record_size = central_directory_entry_size + name_length + extra_length + comment_length;
        if (!Reader::range_fits(position, record_size, end))
            return Error::from_string_literal("ZIP: truncated central directory entry");
        if (disk_start != 0 && disk_start != 0xffff)
            return Error::from_string_literal("ZIP: multi-disk archives are not supported");
        if (name_length == 0 || name_length > max_entry_name_length)
            return Error::from_string_literal("ZIP: invalid entry name length");

        entry.name = ByteString(TRY(reader.bytes_at(position + central_directory_entry_size, name_length)));
        entry.compressed_size = compressed_size;
        entry.uncompressed_size = uncompressed_size;
        entry.local_header_offset = local_header_offset;

        bool need_uncompressed = uncompressed_size == 0xffffffff;
        bool need_compressed = compressed_size == 0xffffffff;
        bool need_offset = local_header_offset == 0xffffffff;
        TRY(apply_zip64_extra_field(reader, position + central_directory_entry_size + name_length, extra_length, entry, need_uncompressed, need_compressed, need_offset));

        if (entry.local_header_offset >= end_record.central_directory_offset)
            return Error::from_string_literal("ZIP: local header offset out of bounds");
        if (entry.uncompressed_size > max_total_uncompressed_size)
            return Error::from_string_literal("ZIP: entry too large");
        total_uncompressed_size += entry.uncompressed_size;
        if (total_uncompressed_size > max_total_uncompressed_size)
            return Error::from_string_literal("ZIP: archive too large when uncompressed");

        if (names.set(entry.name) != HashSetResult::InsertedNewEntry)
            return Error::from_string_literal("ZIP: duplicate entry name");

        entries.unchecked_append(move(entry));
        position += record_size;
    }

    return ZipArchive { data, move(entries), total_uncompressed_size };
}

bool ZipArchive::Entry::is_symlink() const
{
    if ((version_made_by >> 8) != host_unix)
        return false;
    return ((external_attributes >> 16) & S_IFMT) == S_IFLNK;
}

Optional<ZipArchive::Entry const&> ZipArchive::find(StringView name) const
{
    for (auto const& entry : m_entries) {
        if (entry.name == name)
            return entry;
    }
    return {};
}

static ErrorOr<ByteBuffer> inflate_with_limit(ReadonlyBytes compressed, u64 expected_size)
{
    auto input = make<FixedMemoryStream>(compressed);
    auto decompressor = TRY(Compress::DeflateDecompressor::create(MaybeOwned<Stream>(move(input))));

    auto output = TRY(ByteBuffer::create_uninitialized(expected_size));
    size_t filled = 0;
    size_t idle_reads = 0;
    while (filled < expected_size && !decompressor->is_eof()) {
        auto chunk = TRY(decompressor->read_some(output.bytes().slice(filled)));
        filled += chunk.size();
        idle_reads = chunk.is_empty() ? idle_reads + 1 : 0;
        if (idle_reads > 64)
            return Error::from_string_literal("ZIP: deflate stream made no progress");
    }
    if (filled != expected_size)
        return Error::from_string_literal("ZIP: entry is smaller than its declared size");

    // Anything left over means the declared size was a lie.
    idle_reads = 0;
    while (!decompressor->is_eof()) {
        u8 extra;
        auto chunk = TRY(decompressor->read_some({ &extra, 1 }));
        if (!chunk.is_empty())
            return Error::from_string_literal("ZIP: entry is larger than its declared size");
        if (++idle_reads > 64)
            return Error::from_string_literal("ZIP: deflate stream made no progress");
    }
    return output;
}

ErrorOr<ByteBuffer> ZipArchive::read(Entry const& entry) const
{
    if (entry.flags & (flag_encrypted | flag_strong_encryption))
        return Error::from_string_literal("ZIP: encrypted entries are not supported");
    if (entry.method != to_underlying(Method::Stored) && entry.method != to_underlying(Method::Deflate))
        return Error::from_string_literal("ZIP: unsupported compression method");
    if (entry.uncompressed_size > max_total_uncompressed_size)
        return Error::from_string_literal("ZIP: entry too large");
    if (entry.uncompressed_size > compression_ratio_check_threshold
        && entry.uncompressed_size / max(entry.compressed_size, static_cast<u64>(1)) > max_compression_ratio)
        return Error::from_string_literal("ZIP: suspicious compression ratio");

    Reader reader { m_data };
    auto offset = entry.local_header_offset;
    if (TRY(reader.u32_at(offset)) != local_file_header_signature)
        return Error::from_string_literal("ZIP: invalid local file header signature");
    auto name_length = TRY(reader.u16_at(offset + 26));
    auto extra_length = TRY(reader.u16_at(offset + 28));
    auto local_name = TRY(reader.bytes_at(offset + local_file_header_size, name_length));
    if (local_name != entry.name.bytes())
        return Error::from_string_literal("ZIP: local file name does not match central directory");

    // Sizes and CRC come from the central directory, which is also correct when a data descriptor is used.
    auto data_offset = offset + local_file_header_size + name_length + extra_length;
    auto compressed = TRY(reader.bytes_at(data_offset, entry.compressed_size));

    ByteBuffer contents;
    if (entry.method == to_underlying(Method::Stored)) {
        if (entry.compressed_size != entry.uncompressed_size)
            return Error::from_string_literal("ZIP: stored entry size mismatch");
        contents = TRY(ByteBuffer::copy(compressed));
    } else {
        contents = TRY(inflate_with_limit(compressed, entry.uncompressed_size));
    }

    if (crc32(contents) != entry.crc32)
        return Error::from_string_literal("ZIP: CRC-32 mismatch");
    return contents;
}

ErrorOr<void> ZipArchive::validate_entry_name(StringView name)
{
    if (name.is_empty() || name.length() > max_entry_name_length)
        return Error::from_string_literal("ZIP: invalid entry name length");
    if (!Utf8View { name }.validate())
        return Error::from_string_literal("ZIP: entry name is not valid UTF-8");
    for (auto c : name) {
        if (c == '\0' || c == '\\' || c == ':' || static_cast<u8>(c) < 0x20)
            return Error::from_string_literal("ZIP: entry name contains a forbidden character");
    }
    if (name.starts_with('/'))
        return Error::from_string_literal("ZIP: entry name is an absolute path");

    // A single trailing slash marks a directory; every component must be a real name.
    auto path = name.ends_with('/') ? name.substring_view(0, name.length() - 1) : name;
    bool ok = true;
    path.for_each_split_view('/', SplitBehavior::KeepEmpty, [&](StringView component) {
        if (component.is_empty() || component == "."sv || component == ".."sv)
            ok = false;
    });
    if (!ok)
        return Error::from_string_literal("ZIP: entry name contains an invalid path component");
    return {};
}

static ErrorOr<void> create_parent_directories(ByteString const& root, StringView relative_path)
{
    auto last_slash = relative_path.find_last('/');
    if (!last_slash.has_value())
        return {};
    auto directories = relative_path.substring_view(0, *last_slash);
    StringBuilder path;
    path.append(root);
    ErrorOr<void> result {};
    directories.for_each_split_view('/', SplitBehavior::Nothing, [&](StringView component) {
        if (result.is_error())
            return;
        path.append('/');
        path.append(component);
        auto current = path.to_byte_string();
        if (auto mkdir_result = Core::System::mkdir(current, 0755); mkdir_result.is_error()) {
            if (mkdir_result.error().code() != EEXIST) {
                result = mkdir_result.release_error();
                return;
            }
            // Only accept a real directory that we created, never a file or a link.
            auto stat = Core::System::lstat(current);
            if (stat.is_error() || !S_ISDIR(stat.value().st_mode))
                result = Error::from_string_literal("ZIP: path component is not a directory");
        }
    });
    return result;
}

static ErrorOr<void> extract_entries(ZipArchive const& archive, ByteString const& root)
{
    for (auto const& entry : archive.entries()) {
        if (entry.is_directory()) {
            TRY(create_parent_directories(root, entry.name));
            continue;
        }
        auto contents = TRY(archive.read(entry));
        TRY(create_parent_directories(root, entry.name));
        auto path = ByteString::formatted("{}/{}", root, entry.name);
        auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew | Core::File::OpenMode::NoFollow, 0644));
        TRY(file->write_until_depleted(contents));
    }
    return {};
}

ErrorOr<void> ZipArchive::extract_to_directory(ReadonlyBytes zip, StringView destination)
{
    auto archive = TRY(parse(zip));

    for (auto const& entry : archive.entries()) {
        TRY(validate_entry_name(entry.name));
        if (entry.is_symlink())
            return Error::from_string_literal("ZIP: symbolic links are not allowed");
        if ((entry.version_made_by >> 8) == host_unix) {
            auto type = (entry.external_attributes >> 16) & S_IFMT;
            if (type != 0 && type != S_IFREG && type != S_IFDIR)
                return Error::from_string_literal("ZIP: special files are not allowed");
        }
        if (entry.is_directory() && entry.uncompressed_size != 0)
            return Error::from_string_literal("ZIP: directory entry has contents");
    }

    if (destination.is_empty())
        return Error::from_string_literal("ZIP: empty destination path");
    while (destination.length() > 1 && destination.ends_with('/'))
        destination = destination.substring_view(0, destination.length() - 1);
    if (FileSystem::exists(destination) || FileSystem::is_link(destination))
        return Error::from_string_literal("ZIP: destination already exists");

    // Extract next to the destination so the final rename stays on one file system.
    auto pattern = ByteString::formatted("{}.partial-XXXXXX", destination);
    auto pattern_buffer = TRY(ByteBuffer::create_uninitialized(pattern.length() + 1));
    memcpy(pattern_buffer.data(), pattern.characters(), pattern.length() + 1);
    if (!mkdtemp(reinterpret_cast<char*>(pattern_buffer.data())))
        return Error::from_errno(errno);
    ByteString temporary_directory { reinterpret_cast<char const*>(pattern_buffer.data()) };

    bool succeeded = false;
    ScopeGuard cleanup = [&] {
        if (!succeeded)
            (void)FileSystem::remove(temporary_directory, FileSystem::RecursionMode::Allowed);
    };

    TRY(Core::System::chmod(temporary_directory, 0755));
    TRY(extract_entries(archive, temporary_directory));
    TRY(FileSystem::rename_without_replacing(destination, temporary_directory));
    succeeded = true;
    return {};
}

}
