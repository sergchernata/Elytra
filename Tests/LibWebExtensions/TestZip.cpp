/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Endian.h>
#include <AK/ScopeGuard.h>
#include <LibCompress/Deflate.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebExtensions/Zip.h>
#include <stdlib.h>

using WebExtensions::ZipArchive;

namespace {

struct TestEntry {
    ByteString name;
    ByteBuffer data {};
    bool deflate { false };
    bool data_descriptor { false };
    u16 version_made_by { 20 };
    u32 external_attributes { 0 };
    u16 flags { 0 };
    u16 method_override { 0xffff };
    Optional<u32> crc_override {};
    Optional<u64> uncompressed_size_override {};
};

struct BuildOptions {
    StringView comment {};
    bool zip64 { false };
};

class Writer {
public:
    void u8_(u8 value) { m_buffer.append(value); }
    void u16_(u16 value)
    {
        u8_(value & 0xff);
        u8_(value >> 8);
    }
    void u32_(u32 value)
    {
        u16_(value & 0xffff);
        u16_(value >> 16);
    }
    void u64_(u64 value)
    {
        u32_(value & 0xffffffff);
        u32_(value >> 32);
    }
    void bytes(ReadonlyBytes bytes) { m_buffer.append(bytes); }
    size_t size() const { return m_buffer.size(); }
    ByteBuffer take() { return move(m_buffer); }

private:
    ByteBuffer m_buffer;
};

ByteBuffer bytes_of(StringView string)
{
    return MUST(ByteBuffer::copy(string.bytes()));
}

ByteBuffer build_zip(Vector<TestEntry> const& entries, BuildOptions options = {})
{
    Writer writer;
    struct Written {
        u32 crc;
        u64 compressed_size;
        u64 uncompressed_size;
        u64 offset;
        u16 method;
        u16 flags;
    };
    Vector<Written> written;

    for (auto const& entry : entries) {
        auto payload = entry.deflate ? MUST(Compress::DeflateCompressor::compress_all(entry.data)) : entry.data;
        Written info;
        info.crc = entry.crc_override.value_or(WebExtensions::crc32(entry.data));
        info.compressed_size = payload.size();
        info.uncompressed_size = entry.uncompressed_size_override.value_or(entry.data.size());
        info.offset = writer.size();
        info.method = entry.method_override != 0xffff ? entry.method_override : (entry.deflate ? 8 : 0);
        info.flags = entry.flags | (entry.data_descriptor ? 0x8 : 0);

        writer.u32_(0x04034b50);
        writer.u16_(options.zip64 ? 45 : 20);
        writer.u16_(info.flags);
        writer.u16_(info.method);
        writer.u16_(0);
        writer.u16_(0);
        if (entry.data_descriptor) {
            writer.u32_(0);
            writer.u32_(0);
            writer.u32_(0);
        } else if (options.zip64) {
            writer.u32_(info.crc);
            writer.u32_(0xffffffff);
            writer.u32_(0xffffffff);
        } else {
            writer.u32_(info.crc);
            writer.u32_(info.compressed_size);
            writer.u32_(info.uncompressed_size);
        }
        writer.u16_(entry.name.length());
        writer.u16_(options.zip64 && !entry.data_descriptor ? 20 : 0);
        writer.bytes(entry.name.bytes());
        if (options.zip64 && !entry.data_descriptor) {
            writer.u16_(1);
            writer.u16_(16);
            writer.u64_(info.uncompressed_size);
            writer.u64_(info.compressed_size);
        }
        writer.bytes(payload);
        if (entry.data_descriptor) {
            writer.u32_(0x08074b50);
            writer.u32_(info.crc);
            writer.u32_(info.compressed_size);
            writer.u32_(info.uncompressed_size);
        }
        written.append(info);
    }

    auto central_directory_offset = writer.size();
    for (size_t i = 0; i < entries.size(); ++i) {
        auto const& entry = entries[i];
        auto const& info = written[i];
        writer.u32_(0x02014b50);
        writer.u16_(entry.version_made_by);
        writer.u16_(options.zip64 ? 45 : 20);
        writer.u16_(info.flags);
        writer.u16_(info.method);
        writer.u16_(0);
        writer.u16_(0);
        writer.u32_(info.crc);
        writer.u32_(options.zip64 ? 0xffffffff : info.compressed_size);
        writer.u32_(options.zip64 ? 0xffffffff : info.uncompressed_size);
        writer.u16_(entry.name.length());
        writer.u16_(options.zip64 ? 28 : 0);
        writer.u16_(0);
        writer.u16_(0);
        writer.u16_(0);
        writer.u32_(entry.external_attributes);
        writer.u32_(options.zip64 ? 0xffffffff : info.offset);
        writer.bytes(entry.name.bytes());
        if (options.zip64) {
            writer.u16_(1);
            writer.u16_(24);
            writer.u64_(info.uncompressed_size);
            writer.u64_(info.compressed_size);
            writer.u64_(info.offset);
        }
    }
    auto central_directory_size = writer.size() - central_directory_offset;

    if (options.zip64) {
        auto eocd64_offset = writer.size();
        writer.u32_(0x06064b50);
        writer.u64_(44);
        writer.u16_(45);
        writer.u16_(45);
        writer.u32_(0);
        writer.u32_(0);
        writer.u64_(entries.size());
        writer.u64_(entries.size());
        writer.u64_(central_directory_size);
        writer.u64_(central_directory_offset);
        writer.u32_(0x07064b50);
        writer.u32_(0);
        writer.u64_(eocd64_offset);
        writer.u32_(1);
    }

    writer.u32_(0x06054b50);
    writer.u16_(0);
    writer.u16_(0);
    writer.u16_(options.zip64 ? 0xffff : entries.size());
    writer.u16_(options.zip64 ? 0xffff : entries.size());
    writer.u32_(options.zip64 ? 0xffffffff : central_directory_size);
    writer.u32_(options.zip64 ? 0xffffffff : central_directory_offset);
    writer.u16_(options.comment.length());
    writer.bytes(options.comment.bytes());
    return writer.take();
}

ByteString make_temp_directory()
{
    auto const* tmp = getenv("TMPDIR");
    auto pattern = ByteString::formatted("{}/ladybird-test-zip-XXXXXX", tmp ? tmp : "/tmp");
    auto buffer = MUST(ByteBuffer::copy(pattern.bytes()));
    buffer.append('\0');
    VERIFY(mkdtemp(reinterpret_cast<char*>(buffer.data())));
    return ByteString(reinterpret_cast<char const*>(buffer.data()));
}

ByteString read_file(ByteString const& path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto contents = MUST(file->read_until_eof());
    return ByteString(contents.bytes());
}

ErrorOr<ByteString> read_string(ZipArchive const& archive, ZipArchive::Entry const& entry)
{
    auto contents = TRY(archive.read(entry));
    return ByteString(contents.bytes());
}

size_t count_directory_entries(ByteString const& path)
{
    size_t count = 0;
    MUST(Core::Directory::for_each_entry(path, Core::DirIterator::SkipParentAndBaseDir, [&](auto const&, auto const&) -> ErrorOr<IterationDecision> {
        ++count;
        return IterationDecision::Continue;
    }));
    return count;
}

}

TEST_CASE(crc32_known_value)
{
    EXPECT_EQ(WebExtensions::crc32("123456789"sv.bytes()), 0xCBF43926u);
    EXPECT_EQ(WebExtensions::crc32({}), 0u);
}

TEST_CASE(parse_stored_entries)
{
    auto zip = build_zip({
        { .name = "manifest.json", .data = bytes_of("{\"name\":\"x\"}"sv) },
        { .name = "js/", .data = {} },
        { .name = "js/background.js", .data = bytes_of("console.log(1);"sv) },
    });
    auto archive = TRY_OR_FAIL(ZipArchive::parse(zip));
    EXPECT_EQ(archive.entries().size(), 3u);
    EXPECT_EQ(archive.entries()[0].name, "manifest.json"sv);
    EXPECT(archive.entries()[1].is_directory());
    EXPECT(!archive.entries()[2].is_directory());

    auto entry = archive.find("js/background.js"sv);
    EXPECT(entry.has_value());
    auto contents = TRY_OR_FAIL(archive.read(*entry));
    EXPECT_EQ(StringView { contents.bytes() }, "console.log(1);"sv);
    EXPECT(!archive.find("missing"sv).has_value());
    EXPECT_EQ(archive.total_uncompressed_size(), 12u + 15u);
}

TEST_CASE(read_deflate_entry)
{
    StringBuilder builder;
    for (int i = 0; i < 1000; ++i)
        builder.appendff("line {}\n", i);
    auto text = builder.to_byte_string();
    auto zip = build_zip({ { .name = "big.txt", .data = bytes_of(text), .deflate = true } });
    auto archive = TRY_OR_FAIL(ZipArchive::parse(zip));
    auto const& entry = archive.entries()[0];
    EXPECT_EQ(entry.method, 8);
    EXPECT(entry.compressed_size < entry.uncompressed_size);
    auto contents = TRY_OR_FAIL(archive.read(entry));
    EXPECT_EQ(StringView { contents.bytes() }, text.view());
}

TEST_CASE(read_entry_with_data_descriptor)
{
    auto zip = build_zip({
        { .name = "a.txt", .data = bytes_of("hello hello hello"sv), .deflate = true, .data_descriptor = true },
        { .name = "b.txt", .data = bytes_of("world"sv), .data_descriptor = true },
    });
    auto archive = TRY_OR_FAIL(ZipArchive::parse(zip));
    EXPECT_EQ(TRY_OR_FAIL(read_string(archive, archive.entries()[0])), "hello hello hello"sv);
    EXPECT_EQ(TRY_OR_FAIL(read_string(archive, archive.entries()[1])), "world"sv);
}

TEST_CASE(archive_comment)
{
    auto zip = build_zip({ { .name = "a.txt", .data = bytes_of("a"sv) } }, { .comment = "a comment with PK\x05\x06 inside"sv });
    auto archive = TRY_OR_FAIL(ZipArchive::parse(zip));
    EXPECT_EQ(archive.entries().size(), 1u);
    EXPECT_EQ(TRY_OR_FAIL(read_string(archive, archive.entries()[0])), "a"sv);
}

TEST_CASE(zip64)
{
    auto zip = build_zip({
                             { .name = "a.txt", .data = bytes_of("zip64 data"sv) },
                             { .name = "b.txt", .data = bytes_of("more zip64 data more zip64 data"sv), .deflate = true },
                         },
        { .zip64 = true });
    auto archive = TRY_OR_FAIL(ZipArchive::parse(zip));
    EXPECT_EQ(archive.entries().size(), 2u);
    EXPECT_EQ(archive.entries()[0].uncompressed_size, 10u);
    EXPECT_EQ(TRY_OR_FAIL(read_string(archive, archive.entries()[0])), "zip64 data"sv);
    EXPECT_EQ(TRY_OR_FAIL(read_string(archive, archive.entries()[1])), "more zip64 data more zip64 data"sv);
}

TEST_CASE(empty_archive)
{
    auto zip = build_zip({});
    auto archive = TRY_OR_FAIL(ZipArchive::parse(zip));
    EXPECT_EQ(archive.entries().size(), 0u);
}

TEST_CASE(reject_garbage)
{
    EXPECT(ZipArchive::parse({}).is_error());
    EXPECT(ZipArchive::parse("PK\x05\x06"sv.bytes()).is_error());
    EXPECT(ZipArchive::parse("this is definitely not a zip file, just some text"sv.bytes()).is_error());
}

TEST_CASE(reject_bad_crc)
{
    auto zip = build_zip({ { .name = "a.txt", .data = bytes_of("abc"sv), .crc_override = 1234u } });
    auto archive = TRY_OR_FAIL(ZipArchive::parse(zip));
    EXPECT(archive.read(archive.entries()[0]).is_error());
}

TEST_CASE(reject_wrong_uncompressed_size)
{
    auto data = bytes_of("abcdefghijabcdefghijabcdefghij"sv);
    auto smaller = build_zip({ { .name = "a.txt", .data = data, .deflate = true, .uncompressed_size_override = 10u } });
    auto archive = TRY_OR_FAIL(ZipArchive::parse(smaller));
    EXPECT(archive.read(archive.entries()[0]).is_error());

    auto larger = build_zip({ { .name = "a.txt", .data = data, .deflate = true, .uncompressed_size_override = 100u } });
    auto archive2 = TRY_OR_FAIL(ZipArchive::parse(larger));
    EXPECT(archive2.read(archive2.entries()[0]).is_error());

    // Stored entries must have matching sizes.
    auto stored = build_zip({ { .name = "a.txt", .data = data, .uncompressed_size_override = 5u } });
    auto archive3 = TRY_OR_FAIL(ZipArchive::parse(stored));
    EXPECT(archive3.read(archive3.entries()[0]).is_error());
}

TEST_CASE(reject_unsupported_method_and_encryption)
{
    auto bzip2 = build_zip({ { .name = "a.txt", .data = bytes_of("abc"sv), .method_override = 12 } });
    auto archive = TRY_OR_FAIL(ZipArchive::parse(bzip2));
    EXPECT(archive.read(archive.entries()[0]).is_error());

    auto encrypted = build_zip({ { .name = "a.txt", .data = bytes_of("abc"sv), .flags = 1 } });
    auto archive2 = TRY_OR_FAIL(ZipArchive::parse(encrypted));
    EXPECT(archive2.read(archive2.entries()[0]).is_error());
}

TEST_CASE(reject_duplicate_names)
{
    auto zip = build_zip({
        { .name = "a.txt", .data = bytes_of("1"sv) },
        { .name = "a.txt", .data = bytes_of("2"sv) },
    });
    EXPECT(ZipArchive::parse(zip).is_error());
}

TEST_CASE(reject_too_many_entries)
{
    Vector<TestEntry> entries;
    for (size_t i = 0; i <= ZipArchive::max_entry_count; ++i)
        entries.append({ .name = ByteString::formatted("{}", i) });
    auto zip = build_zip(entries);
    EXPECT(ZipArchive::parse(zip).is_error());
}

TEST_CASE(reject_huge_declared_size)
{
    auto zip = build_zip({ { .name = "a.txt", .data = bytes_of("a"sv), .deflate = true, .uncompressed_size_override = ZipArchive::max_total_uncompressed_size + 1 } });
    EXPECT(ZipArchive::parse(zip).is_error());

    // Several entries that are fine on their own but too big together.
    auto half = ZipArchive::max_total_uncompressed_size / 2 + 1;
    auto zip2 = build_zip({
        { .name = "a.txt", .data = bytes_of("a"sv), .deflate = true, .uncompressed_size_override = half },
        { .name = "b.txt", .data = bytes_of("b"sv), .deflate = true, .uncompressed_size_override = half },
    });
    EXPECT(ZipArchive::parse(zip2).is_error());
}

TEST_CASE(reject_high_compression_ratio)
{
    auto zeroes = MUST(ByteBuffer::create_zeroed(8 * MiB));
    auto zip = build_zip({ { .name = "bomb.bin", .data = zeroes, .deflate = true } });
    auto archive = TRY_OR_FAIL(ZipArchive::parse(zip));
    EXPECT(archive.read(archive.entries()[0]).is_error());
}

TEST_CASE(truncation_and_corruption_do_not_crash)
{
    auto zip = build_zip({
        { .name = "a.txt", .data = bytes_of("hello hello hello hello"sv), .deflate = true },
        { .name = "dir/b.txt", .data = bytes_of("world"sv), .data_descriptor = true },
    });
    for (size_t length = 0; length < zip.size(); ++length)
        EXPECT(ZipArchive::parse(zip.bytes().trim(length)).is_error());

    for (size_t i = 0; i < zip.size(); ++i) {
        for (u8 value : { 0x00, 0xff, 0x7f }) {
            auto copy = MUST(ByteBuffer::copy(zip));
            copy[i] = value;
            auto archive = ZipArchive::parse(copy);
            if (archive.is_error())
                continue;
            for (auto const& entry : archive.value().entries())
                (void)archive.value().read(entry);
        }
    }
}

TEST_CASE(validate_entry_names)
{
    for (auto name : { "manifest.json"sv, "a/b/c.js"sv, "dir/"sv, "_locales/en/messages.json"sv, "a..b"sv, ".hidden"sv })
        EXPECT(!ZipArchive::validate_entry_name(name).is_error());

    for (auto name : { ""sv, "/"sv, "/etc/passwd"sv, "../x"sv, "a/../b"sv, ".."sv, "a/.."sv, "a\\b"sv, "..\\x"sv,
             "a/./b"sv, "./a"sv, "a//b"sv, "C:/x"sv, "C:x"sv, "a\0b"sv, "dir//"sv })
        EXPECT(ZipArchive::validate_entry_name(name).is_error());
}

TEST_CASE(extract_to_directory)
{
    auto root = make_temp_directory();
    ScopeGuard cleanup = [&] { (void)FileSystem::remove(root, FileSystem::RecursionMode::Allowed); };

    auto zip = build_zip({
        { .name = "manifest.json", .data = bytes_of("{}"sv) },
        { .name = "images/", .data = {} },
        { .name = "js/lib/util.js", .data = bytes_of("export {};"sv), .deflate = true },
        { .name = "empty.txt", .data = {} },
    });
    auto destination = ByteString::formatted("{}/ext", root);
    TRY_OR_FAIL(ZipArchive::extract_to_directory(zip, destination));

    EXPECT_EQ(read_file(ByteString::formatted("{}/manifest.json", destination)), "{}"sv);
    EXPECT_EQ(read_file(ByteString::formatted("{}/js/lib/util.js", destination)), "export {};"sv);
    EXPECT_EQ(read_file(ByteString::formatted("{}/empty.txt", destination)), ""sv);
    EXPECT(FileSystem::is_directory(ByteString::formatted("{}/images", destination)));
    EXPECT_EQ(count_directory_entries(root), 1u);

    // The destination must not already exist.
    EXPECT(ZipArchive::extract_to_directory(zip, destination).is_error());
    EXPECT_EQ(count_directory_entries(root), 1u);
}

TEST_CASE(extract_rejects_unsafe_entries)
{
    auto root = make_temp_directory();
    ScopeGuard cleanup = [&] { (void)FileSystem::remove(root, FileSystem::RecursionMode::Allowed); };
    auto destination = ByteString::formatted("{}/ext", root);

    auto check_rejected = [&](Vector<TestEntry> const& entries) {
        auto zip = build_zip(entries);
        EXPECT(ZipArchive::extract_to_directory(zip, destination).is_error());
        // Nothing may be left behind, not even a partial temporary directory.
        EXPECT_EQ(count_directory_entries(root), 0u);
    };

    check_rejected({ { .name = "ok.txt", .data = bytes_of("ok"sv) }, { .name = "../evil.txt", .data = bytes_of("x"sv) } });
    check_rejected({ { .name = "ok.txt", .data = bytes_of("ok"sv) }, { .name = "/tmp/evil.txt", .data = bytes_of("x"sv) } });
    check_rejected({ { .name = "ok.txt", .data = bytes_of("ok"sv) }, { .name = "..\\evil.txt", .data = bytes_of("x"sv) } });
    check_rejected({ { .name = "ok.txt", .data = bytes_of("ok"sv) }, { .name = ByteString("a\0b"sv), .data = bytes_of("x"sv) } });
    // Unix symlink (S_IFLNK) made on a Unix host.
    check_rejected({ { .name = "ok.txt", .data = bytes_of("ok"sv) }, { .name = "link", .data = bytes_of("/etc/passwd"sv), .version_made_by = (3 << 8) | 20, .external_attributes = 0120777u << 16 } });
    // A file and a directory with the same path.
    check_rejected({ { .name = "a", .data = bytes_of("ok"sv) }, { .name = "a/b.txt", .data = bytes_of("x"sv) } });
    // A corrupt entry fails the whole extraction.
    check_rejected({ { .name = "ok.txt", .data = bytes_of("ok"sv) }, { .name = "bad.txt", .data = bytes_of("abc"sv), .crc_override = 7u } });
}

TEST_CASE(extract_allows_unix_regular_files)
{
    auto root = make_temp_directory();
    ScopeGuard cleanup = [&] { (void)FileSystem::remove(root, FileSystem::RecursionMode::Allowed); };
    auto destination = ByteString::formatted("{}/ext", root);
    auto zip = build_zip({
        { .name = "dir/", .version_made_by = (3 << 8) | 20, .external_attributes = 040755u << 16 },
        { .name = "dir/a.txt", .data = bytes_of("a"sv), .version_made_by = (3 << 8) | 20, .external_attributes = 0100644u << 16 },
    });
    TRY_OR_FAIL(ZipArchive::extract_to_directory(zip, destination));
    EXPECT_EQ(read_file(ByteString::formatted("{}/dir/a.txt", destination)), "a"sv);
}
