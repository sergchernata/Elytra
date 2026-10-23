/*
 * Release signing.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Curves/EdwardsCurve.h>
#include <UI/Qt/Arc/ReleaseSignature.h>

namespace Ladybird::Arc {

QByteArray release_statement(QString const& tag, QString const& file_name, QString const& sha256)
{
    // Keep in step with the "Sign the release" step of .github/workflows/arc-release-macos.yml.
    return QString("ladybird-arc-release\ntag: %1\nfile: %2\nsha256: %3\n").arg(tag, file_name, sha256.toLower()).toUtf8();
}

bool release_signature_is_valid(QByteArray const& statement, QByteArray const& signature_file, QByteArray const& public_key_base64)
{
    auto decode = [](QByteArray const& base64) {
        auto result = QByteArray::fromBase64Encoding(base64.trimmed(), QByteArray::AbortOnBase64DecodingErrors);
        return result ? *result : QByteArray();
    };
    auto signature = decode(signature_file);
    auto public_key = decode(public_key_base64);
    if (signature.size() != 64 || public_key.size() != 32)
        return false;

    auto bytes = [](QByteArray const& data) { return ReadonlyBytes { reinterpret_cast<u8 const*>(data.constData()), static_cast<size_t>(data.size()) }; };
    auto valid = Crypto::Curves::Ed25519 {}.verify(bytes(public_key), bytes(signature), bytes(statement));
    return !valid.is_error() && valid.value();
}

}
