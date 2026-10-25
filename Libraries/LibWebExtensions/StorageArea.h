/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>

namespace WebExtensions {

// The limits of one chrome.storage area. An empty limit is unlimited.
struct StorageQuota {
    Optional<size_t> quota_bytes;
    Optional<size_t> quota_bytes_per_item;
    Optional<size_t> max_items;

    static StorageQuota local(bool unlimited_storage = false);
    static StorageQuota session();
    static StorageQuota sync();
    static StorageQuota unlimited() { return {}; }
};

// One chrome.storage area (local, session or sync). Change records are objects of the form
// {key: {oldValue?, newValue?}}, as passed to onChanged; they only list values that actually changed.
class StorageArea {
public:
    explicit StorageArea(StorageQuota quota = StorageQuota::unlimited())
        : m_quota(quota)
    {
    }

    // keys: null for everything, a string, an array of strings, or an object of defaults.
    ErrorOr<JsonObject> get(JsonValue const& keys) const;

    // Fails, changing nothing, if any quota would be exceeded.
    ErrorOr<JsonObject> set(JsonObject const& items);

    // keys: a string or an array of strings.
    ErrorOr<JsonObject> remove(JsonValue const& keys);

    JsonObject clear();

    // keys: null for everything, a string, or an array of strings. Each item counts its key length plus the length of
    // its JSON serialization, as Chrome counts.
    ErrorOr<size_t> get_bytes_in_use(JsonValue const& keys) const;

    Vector<String> get_keys() const;

    size_t item_count() const { return m_items.size(); }
    size_t total_bytes_in_use() const { return m_total_bytes; }
    StorageQuota const& quota() const { return m_quota; }

    // Writes the area as one JSON object to a temporary file readable only by the user, then renames it into place.
    ErrorOr<void> save_to_file(ByteString const& path) const;

    // A missing file loads as an empty area; an unreadable or corrupt one is an error. Quotas are not enforced on load.
    static ErrorOr<StorageArea> load_from_file(ByteString const& path, StorageQuota);

    static size_t item_size(StringView key, JsonValue const& value);

private:
    struct Item {
        JsonValue value;
        size_t size { 0 };
    };

    OrderedHashMap<String, Item> m_items;
    size_t m_total_bytes { 0 };
    StorageQuota m_quota;
};

// Deep JSON equality, with object members compared regardless of order and numbers compared by value.
bool json_values_equal(JsonValue const&, JsonValue const&);

}
