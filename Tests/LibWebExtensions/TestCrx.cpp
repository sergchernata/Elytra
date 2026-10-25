/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebExtensions/Crx.h>
#include <LibWebExtensions/Zip.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <stdlib.h>

using WebExtensions::CrxFile;

namespace {

class Key {
public:
    static Key rsa(size_t bits = 2048) { return Key(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", bits)); }
    static Key ec() { return Key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256")); }

    Key(Key&& other)
        : m_key(exchange(other.m_key, nullptr))
        , m_spki(move(other.m_spki))
    {
    }
    ~Key() { EVP_PKEY_free(m_key); }

    ByteBuffer const& spki() const { return m_spki; }

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
        auto id = crx_id();
        StringBuilder builder;
        for (auto byte : id.bytes()) {
            builder.append(static_cast<char>('a' + (byte >> 4)));
            builder.append(static_cast<char>('a' + (byte & 0xf)));
        }
        return MUST(builder.to_string());
    }

private:
    explicit Key(EVP_PKEY* key)
        : m_key(key)
    {
        VERIFY(m_key);
        unsigned char* der = nullptr;
        auto length = i2d_PUBKEY(m_key, &der);
        VERIFY(length > 0);
        m_spki = MUST(ByteBuffer::copy(ReadonlyBytes { der, static_cast<size_t>(length) }));
        OPENSSL_free(der);
    }

    EVP_PKEY* m_key { nullptr };
    ByteBuffer m_spki;
};

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

ByteBuffer signed_message(ReadonlyBytes signed_header_data, ReadonlyBytes archive)
{
    ByteBuffer message;
    message.append("CRX3 SignedData\0"sv.bytes());
    append_u32le(message, signed_header_data.size());
    message.append(signed_header_data);
    message.append(archive);
    return message;
}

ByteBuffer signed_data_for(ReadonlyBytes crx_id)
{
    ByteBuffer signed_data;
    append_bytes_field(signed_data, 1, crx_id);
    return signed_data;
}

ByteBuffer signed_data_for(ByteBuffer const& crx_id)
{
    return signed_data_for(crx_id.bytes());
}

enum class ProofType : u32 {
    Rsa = 2,
    Ecdsa = 3,
};

struct Proof {
    Key const* key;
    ProofType type;
    bool corrupt_signature { false };
};

struct CrxOptions {
    Optional<ByteBuffer> signed_header_data {};
    ByteBuffer extra_header_fields {};
    u32 version { 3 };
};

ByteBuffer archive_bytes()
{
    return MUST(ByteBuffer::copy("PK\x05\x06\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"sv.bytes()));
}

ByteBuffer build_header(Vector<Proof> const& proofs, ByteBuffer const& signed_header_data, ByteBuffer const& archive, ByteBuffer const& extra_header_fields)
{
    auto message = signed_message(signed_header_data, archive);
    ByteBuffer header;
    header.append(extra_header_fields);
    for (auto const& proof : proofs) {
        auto signature = proof.key->sign(message);
        if (proof.corrupt_signature)
            signature[signature.size() / 2] ^= 1;
        ByteBuffer proof_bytes;
        append_bytes_field(proof_bytes, 1, proof.key->spki());
        append_bytes_field(proof_bytes, 2, signature);
        append_bytes_field(header, to_underlying(proof.type), proof_bytes);
    }
    append_bytes_field(header, 10000, signed_header_data);
    return header;
}

ByteBuffer build_crx(Vector<Proof> const& proofs, ByteBuffer const& archive, CrxOptions options = {})
{
    auto signed_header_data = options.signed_header_data.has_value() ? options.signed_header_data.release_value() : signed_data_for(proofs[0].key->crx_id());
    auto header = build_header(proofs, signed_header_data, archive, options.extra_header_fields);

    ByteBuffer crx;
    crx.append("Cr24"sv.bytes());
    append_u32le(crx, options.version);
    append_u32le(crx, header.size());
    crx.append(header);
    crx.append(archive);
    return crx;
}

// Keys are slow to generate, so share them between test cases.
Key const& rsa_key()
{
    static auto* key = new Key(Key::rsa());
    return *key;
}

Key const& other_rsa_key()
{
    static auto* key = new Key(Key::rsa());
    return *key;
}

Key const& ec_key()
{
    static auto* key = new Key(Key::ec());
    return *key;
}

ErrorOr<CrxFile> verify(ByteBuffer const& crx)
{
    return CrxFile::parse_and_verify(crx.bytes());
}

ErrorOr<CrxFile> verify(ReadonlyBytes crx)
{
    return CrxFile::parse_and_verify(crx);
}

}

TEST_CASE(rsa_signed_crx_verifies)
{
    auto archive = archive_bytes();
    auto crx = build_crx({ { &rsa_key(), ProofType::Rsa } }, archive);
    auto file = TRY_OR_FAIL(verify(crx));
    EXPECT_EQ(file.extension_id, rsa_key().extension_id());
    EXPECT_EQ(file.extension_id.bytes().size(), 32u);
    EXPECT_EQ(file.public_key, rsa_key().spki());
    EXPECT_EQ(file.zip_archive, archive);
}

TEST_CASE(ecdsa_signed_crx_verifies)
{
    auto archive = archive_bytes();
    auto crx = build_crx({ { &ec_key(), ProofType::Ecdsa } }, archive);
    auto file = TRY_OR_FAIL(verify(crx));
    EXPECT_EQ(file.extension_id, ec_key().extension_id());
    EXPECT_EQ(file.public_key, ec_key().spki());
}

TEST_CASE(multiple_proofs_verify)
{
    // Web Store packages carry the developer proof plus a store proof; the ID comes from crx_id, not the order.
    auto archive = archive_bytes();
    auto crx = build_crx({ { &other_rsa_key(), ProofType::Rsa }, { &rsa_key(), ProofType::Rsa }, { &ec_key(), ProofType::Ecdsa } }, archive,
        { .signed_header_data = signed_data_for(rsa_key().crx_id()) });
    auto file = TRY_OR_FAIL(verify(crx));
    EXPECT_EQ(file.extension_id, rsa_key().extension_id());
    EXPECT_EQ(file.public_key, rsa_key().spki());
}

TEST_CASE(unknown_fields_are_skipped)
{
    ByteBuffer extra;
    append_varint(extra, (5 << 3) | 0);
    append_varint(extra, 0xffffffffffffffffull);
    append_varint(extra, (6 << 3) | 1);
    extra.append("12345678"sv.bytes());
    append_varint(extra, (7 << 3) | 5);
    extra.append("1234"sv.bytes());
    append_bytes_field(extra, 8, "whatever"sv.bytes());
    append_varint(extra, (9 << 3) | 3);
    append_varint(extra, (1 << 3) | 0);
    append_varint(extra, 42);
    append_varint(extra, (9 << 3) | 4);

    auto signed_data = signed_data_for(rsa_key().crx_id());
    append_bytes_field(signed_data, 7, "future field"sv.bytes());

    auto crx = build_crx({ { &rsa_key(), ProofType::Rsa } }, archive_bytes(), { .signed_header_data = move(signed_data), .extra_header_fields = move(extra) });
    auto file = TRY_OR_FAIL(verify(crx));
    EXPECT_EQ(file.extension_id, rsa_key().extension_id());
}

TEST_CASE(tampered_archive_fails)
{
    auto archive = archive_bytes();
    auto crx = build_crx({ { &rsa_key(), ProofType::Rsa } }, archive);
    crx[crx.size() - 1] ^= 1;
    EXPECT(verify(crx).is_error());

    auto appended = build_crx({ { &rsa_key(), ProofType::Rsa } }, archive);
    appended.append('x');
    EXPECT(verify(appended).is_error());
}

TEST_CASE(tampered_header_fails)
{
    auto archive = archive_bytes();
    auto good = build_crx({ { &rsa_key(), ProofType::Rsa } }, archive);
    // Flip each byte of the header; none of these may verify (or crash).
    for (size_t i = 0; i < good.size(); ++i) {
        auto copy = MUST(ByteBuffer::copy(good));
        copy[i] ^= 0x01;
        EXPECT(verify(copy).is_error());
    }
}

TEST_CASE(wrong_crx_id_fails)
{
    auto archive = archive_bytes();
    // Properly signed by rsa_key, but claims to be other_rsa_key's extension.
    auto crx = build_crx({ { &rsa_key(), ProofType::Rsa } }, archive, { .signed_header_data = signed_data_for(other_rsa_key().crx_id()) });
    EXPECT(verify(crx).is_error());

    auto short_id = build_crx({ { &rsa_key(), ProofType::Rsa } }, archive, { .signed_header_data = signed_data_for(MUST(rsa_key().crx_id().slice(0, 8))) });
    EXPECT(verify(short_id).is_error());

    auto no_signed_data = build_crx({ { &rsa_key(), ProofType::Rsa } }, archive, { .signed_header_data = ByteBuffer {} });
    EXPECT(verify(no_signed_data).is_error());
}

TEST_CASE(every_proof_must_verify)
{
    auto archive = archive_bytes();
    auto crx = build_crx({ { &rsa_key(), ProofType::Rsa }, { &other_rsa_key(), ProofType::Rsa, true } }, archive);
    EXPECT(verify(crx).is_error());

    auto bad_matching = build_crx({ { &rsa_key(), ProofType::Rsa, true }, { &other_rsa_key(), ProofType::Rsa } }, archive);
    EXPECT(verify(bad_matching).is_error());
}

TEST_CASE(key_type_must_match_proof_type)
{
    auto archive = archive_bytes();
    EXPECT(verify(build_crx({ { &rsa_key(), ProofType::Ecdsa } }, archive)).is_error());
    EXPECT(verify(build_crx({ { &ec_key(), ProofType::Rsa } }, archive)).is_error());
}

TEST_CASE(no_proofs_fails)
{
    auto archive = archive_bytes();
    auto signed_data = signed_data_for(rsa_key().crx_id());
    ByteBuffer header;
    append_bytes_field(header, 10000, signed_data);
    ByteBuffer crx;
    crx.append("Cr24"sv.bytes());
    append_u32le(crx, 3);
    append_u32le(crx, header.size());
    crx.append(header);
    crx.append(archive);
    EXPECT(verify(crx).is_error());
}

TEST_CASE(truncated_fails)
{
    auto crx = build_crx({ { &rsa_key(), ProofType::Rsa } }, archive_bytes());
    for (size_t length = 0; length < crx.size(); ++length)
        EXPECT(verify(crx.bytes().trim(length)).is_error());
}

TEST_CASE(bad_container_fields_fail)
{
    auto archive = archive_bytes();
    auto crx = build_crx({ { &rsa_key(), ProofType::Rsa } }, archive);

    auto huge_header = MUST(ByteBuffer::copy(crx));
    for (size_t i = 8; i < 12; ++i)
        huge_header[i] = 0xff;
    EXPECT(verify(huge_header).is_error());

    auto bad_magic = MUST(ByteBuffer::copy(crx));
    bad_magic[0] = 'X';
    EXPECT(verify(bad_magic).is_error());

    auto crx2 = build_crx({ { &rsa_key(), ProofType::Rsa } }, archive, { .version = 2 });
    auto result = verify(crx2);
    EXPECT(result.is_error());
    EXPECT(StringView { result.error().string_literal() }.contains("CRX2"sv));

    EXPECT(verify(build_crx({ { &rsa_key(), ProofType::Rsa } }, archive, { .version = 4 })).is_error());
}

TEST_CASE(malformed_protobuf_fails)
{
    auto make_crx = [&](ReadonlyBytes header) {
        ByteBuffer crx;
        crx.append("Cr24"sv.bytes());
        append_u32le(crx, 3);
        append_u32le(crx, header.size());
        crx.append(header);
        auto archive = archive_bytes();
        crx.append(archive);
        return crx;
    };
    auto good_header = build_header({ { &rsa_key(), ProofType::Rsa } }, signed_data_for(rsa_key().crx_id()), archive_bytes(), {});
    EXPECT(!verify(make_crx(good_header)).is_error());

    auto expect_rejected = [&](ByteBuffer const& prefix) {
        auto header = MUST(ByteBuffer::copy(prefix));
        header.append(good_header);
        EXPECT(verify(make_crx(header)).is_error());
    };

    // Varint that never ends.
    expect_rejected(MUST(ByteBuffer::copy("\x28\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"sv.bytes())));
    // Length-delimited field longer than the header.
    ByteBuffer too_long;
    append_varint(too_long, (8 << 3) | 2);
    append_varint(too_long, 0xffffffffull);
    expect_rejected(too_long);
    // Invalid wire types.
    expect_rejected(MUST(ByteBuffer::copy("\x46"sv.bytes())));
    expect_rejected(MUST(ByteBuffer::copy("\x47"sv.bytes())));
    // Unmatched end group.
    expect_rejected(MUST(ByteBuffer::copy("\x4c"sv.bytes())));
    // Unterminated group.
    ByteBuffer group;
    append_varint(group, (9 << 3) | 3);
    auto header = MUST(ByteBuffer::copy(good_header));
    header.append(group);
    EXPECT(verify(make_crx(header)).is_error());
    // Known field with the wrong wire type.
    ByteBuffer wrong_type;
    append_varint(wrong_type, (2 << 3) | 0);
    append_varint(wrong_type, 1);
    expect_rejected(wrong_type);
    // Field number zero.
    expect_rejected(MUST(ByteBuffer::copy("\x00\x00"sv.bytes())));
}

TEST_CASE(crx_round_trip_with_zip)
{
    auto const* tmp = getenv("TMPDIR");
    auto pattern = ByteString::formatted("{}/ladybird-test-crx-XXXXXX", tmp ? tmp : "/tmp");
    auto buffer = MUST(ByteBuffer::copy(pattern.bytes()));
    buffer.append('\0');
    VERIFY(mkdtemp(reinterpret_cast<char*>(buffer.data())));
    ByteString root { reinterpret_cast<char const*>(buffer.data()) };
    ScopeGuard cleanup = [&] { (void)FileSystem::remove(root, FileSystem::RecursionMode::Allowed); };

    // A minimal stored zip holding manifest.json.
    auto name = "manifest.json"sv;
    auto contents = "{}"sv;
    auto crc = WebExtensions::crc32(contents.bytes());
    ByteBuffer zip;
    auto u16le = [&](u16 value) { zip.append(static_cast<u8>(value)); zip.append(static_cast<u8>(value >> 8)); };
    append_u32le(zip, 0x04034b50);
    u16le(20);
    u16le(0);
    u16le(0);
    u16le(0);
    u16le(0);
    append_u32le(zip, crc);
    append_u32le(zip, contents.length());
    append_u32le(zip, contents.length());
    u16le(name.length());
    u16le(0);
    zip.append(name.bytes());
    zip.append(contents.bytes());
    auto central_directory_offset = zip.size();
    append_u32le(zip, 0x02014b50);
    u16le(20);
    u16le(20);
    u16le(0);
    u16le(0);
    u16le(0);
    u16le(0);
    append_u32le(zip, crc);
    append_u32le(zip, contents.length());
    append_u32le(zip, contents.length());
    u16le(name.length());
    u16le(0);
    u16le(0);
    u16le(0);
    u16le(0);
    append_u32le(zip, 0);
    append_u32le(zip, 0);
    zip.append(name.bytes());
    auto central_directory_size = zip.size() - central_directory_offset;
    append_u32le(zip, 0x06054b50);
    u16le(0);
    u16le(0);
    u16le(1);
    u16le(1);
    append_u32le(zip, central_directory_size);
    append_u32le(zip, central_directory_offset);
    u16le(0);

    auto crx = build_crx({ { &ec_key(), ProofType::Ecdsa } }, zip);
    auto file = TRY_OR_FAIL(verify(crx));
    auto destination = ByteString::formatted("{}/{}", root, file.extension_id);
    TRY_OR_FAIL(WebExtensions::ZipArchive::extract_to_directory(file.zip_archive, destination));
    EXPECT(FileSystem::is_regular_file(ByteString::formatted("{}/manifest.json", destination)));
}

// Manual check against a real Web Store package: LADYBIRD_TEST_REAL_CRX=/path/to/file.crx
// Optionally LADYBIRD_TEST_REAL_CRX_ID=<expected extension id>.
TEST_CASE(real_crx_from_environment)
{
    auto const* path = getenv("LADYBIRD_TEST_REAL_CRX");
    if (!path)
        return;
    auto file = TRY_OR_FAIL(Core::File::open(StringView { path, strlen(path) }, Core::File::OpenMode::Read));
    auto bytes = TRY_OR_FAIL(file->read_until_eof());
    auto crx = TRY_OR_FAIL(verify(bytes));
    outln("Extension ID: {}", crx.extension_id);
    if (auto const* expected_id = getenv("LADYBIRD_TEST_REAL_CRX_ID"))
        EXPECT_EQ(crx.extension_id.bytes_as_string_view(), StringView(expected_id, strlen(expected_id)));

    auto archive = TRY_OR_FAIL(WebExtensions::ZipArchive::parse(crx.zip_archive));
    outln("Entries: {}, uncompressed size: {}", archive.entries().size(), archive.total_uncompressed_size());
    EXPECT(archive.find("manifest.json"sv).has_value());

    auto const* tmp = getenv("TMPDIR");
    auto pattern = ByteString::formatted("{}/ladybird-test-real-crx-XXXXXX", tmp ? tmp : "/tmp");
    auto buffer = MUST(ByteBuffer::copy(pattern.bytes()));
    buffer.append('\0');
    VERIFY(mkdtemp(reinterpret_cast<char*>(buffer.data())));
    ByteString root { reinterpret_cast<char const*>(buffer.data()) };
    ScopeGuard cleanup = [&] { (void)FileSystem::remove(root, FileSystem::RecursionMode::Allowed); };
    auto destination = ByteString::formatted("{}/{}", root, crx.extension_id);
    TRY_OR_FAIL(WebExtensions::ZipArchive::extract_to_directory(crx.zip_archive, destination));
    EXPECT(FileSystem::is_regular_file(ByteString::formatted("{}/manifest.json", destination)));
}
