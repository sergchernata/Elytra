/*
 * Release signing: each release's download comes with an Ed25519 signature (made by the release workflow with a key
 * kept in a GitHub secret), and the app installs only downloads signed with the key below.
 *
 * The signature covers a short statement naming the release's tag, the file and its SHA-256, so a signed file can't be
 * passed off as a different (newer) release, and a signature can't be moved to another file.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QByteArray>
#include <QString>

namespace Ladybird::Arc {

// The public half of the release signing key (raw Ed25519, base64).
inline constexpr char RELEASE_PUBLIC_KEY_BASE64[] = "cEUMWgldZGOZ0eIqwFL0w1xyRMG4CC3t+hG+BV+/qAA=";

// What the release workflow signs for `file_name` (with hex SHA-256 `sha256`) in release `tag`.
QByteArray release_statement(QString const& tag, QString const& file_name, QString const& sha256);

// Whether `signature_file` (a base64 Ed25519 signature, as the workflow publishes it) signs `statement` with the key
// `public_key_base64`.
bool release_signature_is_valid(QByteArray const& statement, QByteArray const& signature_file, QByteArray const& public_key_base64 = RELEASE_PUBLIC_KEY_BASE64);

}
