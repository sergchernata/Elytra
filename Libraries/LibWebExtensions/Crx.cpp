/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibWebExtensions/Crx.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

namespace WebExtensions {

static constexpr auto crx_magic = "Cr24"sv;
static constexpr auto signature_context = "CRX3 SignedData\0"sv;
static constexpr size_t crx_id_size = 16;
static constexpr size_t max_proof_count = 64;
static constexpr size_t max_group_depth = 16;

namespace {

enum class WireType : u8 {
    Varint = 0,
    Fixed64 = 1,
    LengthDelimited = 2,
    StartGroup = 3,
    EndGroup = 4,
    Fixed32 = 5,
};

struct Field {
    u32 number { 0 };
    WireType wire_type { WireType::Varint };
    ReadonlyBytes bytes; // Payload of length-delimited fields.
};

// Minimal protobuf wire-format reader; unknown fields of every wire type are skipped.
class ProtobufReader {
public:
    explicit ProtobufReader(ReadonlyBytes data)
        : m_data(data)
    {
    }

    bool at_end() const { return m_position == m_data.size(); }

    ErrorOr<Field> read_field()
    {
        auto field = TRY(read_key());
        if (field.wire_type == WireType::EndGroup)
            return Error::from_string_literal("CRX: unexpected protobuf end group");
        if (field.wire_type == WireType::LengthDelimited)
            field.bytes = TRY(read_length_delimited());
        else
            TRY(skip_value(field, 0));
        return field;
    }

private:
    ErrorOr<u64> read_varint()
    {
        u64 value = 0;
        for (size_t i = 0; i < 10; ++i) {
            if (m_position >= m_data.size())
                return Error::from_string_literal("CRX: truncated protobuf varint");
            u8 byte = m_data[m_position++];
            if (i == 9 && byte > 1)
                return Error::from_string_literal("CRX: protobuf varint overflow");
            value |= static_cast<u64>(byte & 0x7f) << (7 * i);
            if (!(byte & 0x80))
                return value;
        }
        return Error::from_string_literal("CRX: protobuf varint too long");
    }

    ErrorOr<Field> read_key()
    {
        auto key = TRY(read_varint());
        auto number = key >> 3;
        auto wire_type = key & 7;
        if (number == 0 || number > 0x1fffffff)
            return Error::from_string_literal("CRX: invalid protobuf field number");
        if (wire_type > to_underlying(WireType::Fixed32))
            return Error::from_string_literal("CRX: invalid protobuf wire type");
        return Field { static_cast<u32>(number), static_cast<WireType>(wire_type), {} };
    }

    ErrorOr<void> skip_bytes(u64 count)
    {
        if (count > m_data.size() - m_position)
            return Error::from_string_literal("CRX: truncated protobuf field");
        m_position += count;
        return {};
    }

    ErrorOr<ReadonlyBytes> read_length_delimited()
    {
        auto length = TRY(read_varint());
        auto start = m_position;
        TRY(skip_bytes(length));
        return m_data.slice(start, length);
    }

    ErrorOr<void> skip_value(Field const& field, size_t depth)
    {
        switch (field.wire_type) {
        case WireType::Varint:
            TRY(read_varint());
            return {};
        case WireType::Fixed64:
            return skip_bytes(8);
        case WireType::Fixed32:
            return skip_bytes(4);
        case WireType::LengthDelimited:
            TRY(read_length_delimited());
            return {};
        case WireType::StartGroup: {
            if (depth >= max_group_depth)
                return Error::from_string_literal("CRX: protobuf groups nested too deeply");
            while (true) {
                auto inner = TRY(read_key());
                if (inner.wire_type == WireType::EndGroup) {
                    if (inner.number != field.number)
                        return Error::from_string_literal("CRX: mismatched protobuf end group");
                    return {};
                }
                TRY(skip_value(inner, depth + 1));
            }
        }
        case WireType::EndGroup:
            break;
        }
        return Error::from_string_literal("CRX: unexpected protobuf end group");
    }

    ReadonlyBytes m_data;
    size_t m_position { 0 };
};

enum class ProofType {
    Rsa,
    Ecdsa,
};

struct Proof {
    ProofType type;
    ReadonlyBytes public_key;
    ReadonlyBytes signature;
};

struct Header {
    Vector<Proof> proofs;
    ReadonlyBytes signed_header_data;
};

}

static ErrorOr<Proof> parse_proof(ProofType type, ReadonlyBytes bytes)
{
    Proof proof { type, {}, {} };
    ProtobufReader reader { bytes };
    while (!reader.at_end()) {
        auto field = TRY(reader.read_field());
        if (field.number != 1 && field.number != 2)
            continue;
        if (field.wire_type != WireType::LengthDelimited)
            return Error::from_string_literal("CRX: proof field has the wrong type");
        if (field.number == 1)
            proof.public_key = field.bytes;
        else
            proof.signature = field.bytes;
    }
    if (proof.public_key.is_empty() || proof.signature.is_empty())
        return Error::from_string_literal("CRX: proof is missing its key or signature");
    return proof;
}

static ErrorOr<Header> parse_header(ReadonlyBytes bytes)
{
    Header header;
    ProtobufReader reader { bytes };
    while (!reader.at_end()) {
        auto field = TRY(reader.read_field());
        if (field.number != 2 && field.number != 3 && field.number != 10000)
            continue;
        if (field.wire_type != WireType::LengthDelimited)
            return Error::from_string_literal("CRX: header field has the wrong type");
        if (field.number == 10000) {
            header.signed_header_data = field.bytes;
            continue;
        }
        if (header.proofs.size() >= max_proof_count)
            return Error::from_string_literal("CRX: too many proofs");
        TRY(header.proofs.try_append(TRY(parse_proof(field.number == 2 ? ProofType::Rsa : ProofType::Ecdsa, field.bytes))));
    }
    return header;
}

static ErrorOr<ReadonlyBytes> parse_crx_id(ReadonlyBytes signed_header_data)
{
    Optional<ReadonlyBytes> crx_id;
    ProtobufReader reader { signed_header_data };
    while (!reader.at_end()) {
        auto field = TRY(reader.read_field());
        if (field.number != 1)
            continue;
        if (field.wire_type != WireType::LengthDelimited)
            return Error::from_string_literal("CRX: crx_id has the wrong type");
        crx_id = field.bytes;
    }
    if (!crx_id.has_value() || crx_id->size() != crx_id_size)
        return Error::from_string_literal("CRX: missing or invalid crx_id");
    return *crx_id;
}

static ErrorOr<void> sha256(ReadonlyBytes data, Bytes digest)
{
    VERIFY(digest.size() == 32);
    unsigned int length = 0;
    if (EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 || length != 32)
        return Error::from_string_literal("CRX: SHA-256 failed");
    return {};
}

static bool verify_proof(Proof const& proof, ReadonlyBytes signed_header_data, ReadonlyBytes archive)
{
    ScopeGuard clear_errors = [] { ERR_clear_error(); };

    auto const* key_data = proof.public_key.data();
    auto* key = d2i_PUBKEY(nullptr, &key_data, static_cast<long>(proof.public_key.size()));
    if (!key)
        return false;
    ScopeGuard free_key = [&] { EVP_PKEY_free(key); };
    if (key_data != proof.public_key.data() + proof.public_key.size())
        return false;

    if (proof.type == ProofType::Rsa) {
        if (EVP_PKEY_get_base_id(key) != EVP_PKEY_RSA || EVP_PKEY_get_bits(key) < 1024)
            return false;
    } else {
        if (EVP_PKEY_get_base_id(key) != EVP_PKEY_EC)
            return false;
        char group[64] {};
        size_t group_length = 0;
        if (EVP_PKEY_get_group_name(key, group, sizeof(group), &group_length) != 1)
            return false;
        if (StringView { group, group_length } != "prime256v1"sv)
            return false;
    }

    auto* context = EVP_MD_CTX_new();
    if (!context)
        return false;
    ScopeGuard free_context = [&] { EVP_MD_CTX_free(context); };

    u8 size_bytes[4];
    auto size = static_cast<u32>(signed_header_data.size());
    for (size_t i = 0; i < 4; ++i)
        size_bytes[i] = static_cast<u8>(size >> (8 * i));

    if (EVP_DigestVerifyInit(context, nullptr, EVP_sha256(), nullptr, key) != 1)
        return false;
    if (EVP_DigestVerifyUpdate(context, signature_context.characters_without_null_termination(), signature_context.length()) != 1
        || EVP_DigestVerifyUpdate(context, size_bytes, sizeof(size_bytes)) != 1
        || EVP_DigestVerifyUpdate(context, signed_header_data.data(), signed_header_data.size()) != 1
        || EVP_DigestVerifyUpdate(context, archive.data(), archive.size()) != 1)
        return false;
    return EVP_DigestVerifyFinal(context, proof.signature.data(), proof.signature.size()) == 1;
}

// Chrome maps each nibble of the 16-byte crx_id to 'a'..'p'.
static ErrorOr<String> extension_id_from_crx_id(ReadonlyBytes crx_id)
{
    StringBuilder builder;
    for (auto byte : crx_id) {
        builder.append(static_cast<char>('a' + (byte >> 4)));
        builder.append(static_cast<char>('a' + (byte & 0xf)));
    }
    return builder.to_string();
}

static u32 read_u32le(ReadonlyBytes bytes, size_t offset)
{
    return static_cast<u32>(bytes[offset]) | (static_cast<u32>(bytes[offset + 1]) << 8)
        | (static_cast<u32>(bytes[offset + 2]) << 16) | (static_cast<u32>(bytes[offset + 3]) << 24);
}

ErrorOr<CrxFile> CrxFile::parse_and_verify(ReadonlyBytes data)
{
    if (data.size() < 12)
        return Error::from_string_literal("CRX: file too small");
    if (data.slice(0, 4) != crx_magic.bytes())
        return Error::from_string_literal("CRX: not a CRX file");

    auto version = read_u32le(data, 4);
    if (version == 2)
        return Error::from_string_literal("CRX: CRX2 packages are no longer supported, only CRX3");
    if (version != 3)
        return Error::from_string_literal("CRX: unsupported CRX version");

    auto header_size = read_u32le(data, 8);
    if (header_size > max_header_size || header_size > data.size() - 12)
        return Error::from_string_literal("CRX: invalid header size");

    auto header_bytes = data.slice(12, header_size);
    auto archive = data.slice(12 + header_size);
    auto header = TRY(parse_header(header_bytes));

    if (header.proofs.is_empty())
        return Error::from_string_literal("CRX: no signatures");
    auto crx_id = TRY(parse_crx_id(header.signed_header_data));

    Optional<Proof const&> declared_proof;
    for (auto const& proof : header.proofs) {
        u8 digest[32];
        TRY(sha256(proof.public_key, { digest, sizeof(digest) }));
        bool matches_id = ReadonlyBytes { digest, crx_id_size } == crx_id;

        if (!verify_proof(proof, header.signed_header_data, archive))
            return Error::from_string_literal("CRX: signature verification failed");
        if (matches_id && !declared_proof.has_value())
            declared_proof = proof;
    }
    if (!declared_proof.has_value())
        return Error::from_string_literal("CRX: no signature from the key matching crx_id");

    return CrxFile {
        .extension_id = TRY(extension_id_from_crx_id(crx_id)),
        .public_key = TRY(ByteBuffer::copy(declared_proof->public_key)),
        .zip_archive = TRY(ByteBuffer::copy(archive)),
    };
}

}
