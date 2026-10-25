/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibWebExtensions/StorageArea.h>

#include <unistd.h>

namespace WebExtensions {

// The limits from chrome.storage.local, chrome.storage.session and chrome.storage.sync.
static constexpr size_t local_quota_bytes = 10485760;
static constexpr size_t session_quota_bytes = 10485760;
static constexpr size_t sync_quota_bytes = 102400;
static constexpr size_t sync_quota_bytes_per_item = 8192;
static constexpr size_t sync_max_items = 512;

StorageQuota StorageQuota::local(bool unlimited_storage)
{
    if (unlimited_storage)
        return unlimited();
    return { .quota_bytes = local_quota_bytes, .quota_bytes_per_item = {}, .max_items = {} };
}

StorageQuota StorageQuota::session()
{
    return { .quota_bytes = session_quota_bytes, .quota_bytes_per_item = {}, .max_items = {} };
}

StorageQuota StorageQuota::sync()
{
    return { .quota_bytes = sync_quota_bytes, .quota_bytes_per_item = sync_quota_bytes_per_item, .max_items = sync_max_items };
}

bool json_values_equal(JsonValue const& a, JsonValue const& b)
{
    return a.equals(b);
}

size_t StorageArea::item_size(StringView key, JsonValue const& value)
{
    StringBuilder builder;
    value.serialize(builder);
    return key.length() + builder.length();
}

// Reads a key list: a single string or an array of strings.
static ErrorOr<Vector<String>> key_list(JsonValue const& keys)
{
    Vector<String> result;
    if (keys.is_string()) {
        result.append(keys.as_string());
        return result;
    }
    if (!keys.is_array())
        return Error::from_string_literal("Keys must be a string or an array of strings");
    auto const& array = keys.as_array();
    TRY(result.try_ensure_capacity(array.size()));
    for (auto const& key : array.values()) {
        if (!key.is_string())
            return Error::from_string_literal("Keys must be a string or an array of strings");
        result.unchecked_append(key.as_string());
    }
    return result;
}

ErrorOr<JsonObject> StorageArea::get(JsonValue const& keys) const
{
    JsonObject result;
    if (keys.is_null()) {
        for (auto const& [key, item] : m_items)
            result.set(key, item.value);
        return result;
    }

    if (keys.is_object()) {
        TRY(keys.as_object().try_for_each_member([&](String const& key, JsonValue const& default_value) -> ErrorOr<void> {
            auto item = m_items.get(key);
            result.set(key, item.has_value() ? item->value : default_value);
            return {};
        }));
        return result;
    }

    for (auto const& key : TRY(key_list(keys))) {
        if (auto item = m_items.get(key); item.has_value())
            result.set(key, item->value);
    }
    return result;
}

ErrorOr<JsonObject> StorageArea::set(JsonObject const& items)
{
    // Work out the result first, so that a failed quota check changes nothing.
    size_t new_total = m_total_bytes;
    size_t new_count = m_items.size();
    Vector<String> changed_keys;
    Vector<size_t> changed_sizes;

    TRY(items.try_for_each_member([&](String const& key, JsonValue const& value) -> ErrorOr<void> {
        auto size = item_size(key, value);
        if (m_quota.quota_bytes_per_item.has_value() && size > *m_quota.quota_bytes_per_item)
            return Error::from_string_literal("QUOTA_BYTES_PER_ITEM quota exceeded");

        auto existing = m_items.get(key);
        if (existing.has_value()) {
            if (json_values_equal(existing->value, value))
                return {};
            new_total = new_total - existing->size + size;
        } else {
            new_total += size;
            ++new_count;
        }
        changed_keys.append(key);
        changed_sizes.append(size);
        return {};
    }));

    if (m_quota.quota_bytes.has_value() && new_total > *m_quota.quota_bytes)
        return Error::from_string_literal("QUOTA_BYTES quota exceeded");
    if (m_quota.max_items.has_value() && new_count > *m_quota.max_items)
        return Error::from_string_literal("MAX_ITEMS quota exceeded");

    JsonObject changes;
    for (size_t i = 0; i < changed_keys.size(); ++i) {
        auto const& key = changed_keys[i];
        auto const& value = *items.get(key);

        JsonObject change;
        if (auto existing = m_items.get(key); existing.has_value())
            change.set("oldValue"sv, existing->value);
        change.set("newValue"sv, value);
        changes.set(key, move(change));

        m_items.set(key, { value, changed_sizes[i] });
    }
    m_total_bytes = new_total;
    return changes;
}

ErrorOr<JsonObject> StorageArea::remove(JsonValue const& keys)
{
    auto list = TRY(key_list(keys));
    JsonObject changes;
    for (auto const& key : list) {
        auto item = m_items.take(key);
        if (!item.has_value())
            continue;
        m_total_bytes -= item->size;
        JsonObject change;
        change.set("oldValue"sv, move(item->value));
        changes.set(key, move(change));
    }
    return changes;
}

JsonObject StorageArea::clear()
{
    JsonObject changes;
    for (auto& [key, item] : m_items) {
        JsonObject change;
        change.set("oldValue"sv, move(item.value));
        changes.set(key, move(change));
    }
    m_items.clear();
    m_total_bytes = 0;
    return changes;
}

ErrorOr<size_t> StorageArea::get_bytes_in_use(JsonValue const& keys) const
{
    if (keys.is_null())
        return m_total_bytes;

    size_t total = 0;
    auto list = TRY(key_list(keys));
    HashTable<String> counted;
    for (auto const& key : list) {
        if (counted.set(key) != HashSetResult::InsertedNewEntry)
            continue;
        if (auto item = m_items.get(key); item.has_value())
            total += item->size;
    }
    return total;
}

Vector<String> StorageArea::get_keys() const
{
    Vector<String> keys;
    keys.ensure_capacity(m_items.size());
    for (auto const& [key, item] : m_items)
        keys.unchecked_append(key);
    return keys;
}

ErrorOr<void> StorageArea::save_to_file(ByteString const& path) const
{
    StringBuilder builder;
    TRY(get(JsonValue {})).serialize(builder);
    auto contents = TRY(builder.to_byte_buffer());

    auto temporary_path = ByteString::formatted("{}.tmp-{}", path, Core::System::getpid());
    (void)Core::System::unlink(temporary_path);

    ArmedScopeGuard remove_temporary = [&] { (void)Core::System::unlink(temporary_path); };
    {
        auto file = TRY(Core::File::open(temporary_path, Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew | Core::File::OpenMode::NoFollow, 0600));
        TRY(Core::System::fchmod(file->fd(), 0600));
        TRY(file->write_until_depleted(contents));
        if (::fsync(file->fd()) < 0)
            return Error::from_syscall("fsync"sv, errno);
    }
    TRY(Core::System::rename(temporary_path, path));
    remove_temporary.disarm();
    return {};
}

ErrorOr<StorageArea> StorageArea::load_from_file(ByteString const& path, StorageQuota quota)
{
    StorageArea area(quota);

    auto file_or_error = Core::File::open(path, Core::File::OpenMode::Read);
    if (file_or_error.is_error()) {
        if (file_or_error.error().is_errno() && file_or_error.error().code() == ENOENT)
            return area;
        return file_or_error.release_error();
    }
    auto contents = TRY(file_or_error.value()->read_until_eof());

    auto json = TRY(JsonParser::parse(contents));
    if (!json.is_object())
        return Error::from_string_literal("Storage file does not hold a JSON object");

    json.as_object().for_each_member([&](String const& key, JsonValue const& value) {
        auto size = item_size(key, value);
        area.m_items.set(key, { value, size });
        area.m_total_bytes += size;
    });
    return area;
}

}
