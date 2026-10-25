/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebExtensions/StorageArea.h>

using WebExtensions::StorageArea;
using WebExtensions::StorageQuota;

static JsonValue json(StringView text)
{
    return MUST(JsonValue::from_string(text));
}

static JsonObject object(StringView text)
{
    return json(text).as_object();
}

static bool equals(JsonValue const& a, StringView b)
{
    return WebExtensions::json_values_equal(a, json(b));
}

static ByteString test_root()
{
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-storage-area-{}", Core::System::getpid())).string();
}

static void remove_test_root()
{
    if (FileSystem::exists(test_root()))
        MUST(FileSystem::remove(test_root(), FileSystem::RecursionMode::Allowed));
}

static ByteString make_test_root()
{
    remove_test_root();
    MUST(Core::System::mkdir(test_root(), 0700));
    return test_root();
}

TEST_CASE(json_equality)
{
    EXPECT(equals(json(R"({"a":1,"b":[1,2,{"c":null}]})"sv), R"({"b":[1,2,{"c":null}],"a":1})"sv));
    EXPECT(equals(json("1"sv), "1.0"sv));
    EXPECT(equals(json("\"x\""sv), "\"x\""sv));
    EXPECT(!equals(json("1"sv), "\"1\""sv));
    EXPECT(!equals(json("[1,2]"sv), "[2,1]"sv));
    EXPECT(!equals(json(R"({"a":1})"sv), R"({"a":1,"b":2})"sv));
    EXPECT(!equals(json(R"({"a":1,"b":2})"sv), R"({"a":1,"c":2})"sv));
    EXPECT(!equals(json("null"sv), "false"sv));
    EXPECT(!equals(json("true"sv), "false"sv));
    EXPECT(!equals(json("0"sv), "false"sv));
}

TEST_CASE(quotas)
{
    auto local = StorageQuota::local();
    EXPECT_EQ(local.quota_bytes, 10485760u);
    EXPECT(!local.quota_bytes_per_item.has_value());
    EXPECT(!local.max_items.has_value());
    EXPECT(!StorageQuota::local(true).quota_bytes.has_value());
    EXPECT_EQ(StorageQuota::session().quota_bytes, 10485760u);
    auto sync = StorageQuota::sync();
    EXPECT_EQ(sync.quota_bytes, 102400u);
    EXPECT_EQ(sync.quota_bytes_per_item, 8192u);
    EXPECT_EQ(sync.max_items, 512u);
}

TEST_CASE(set_and_get)
{
    StorageArea area;
    auto changes = TRY_OR_FAIL(area.set(object(R"({"a":1,"b":{"c":[true,null]},"s":"text"})"sv)));
    EXPECT(equals(changes, R"({"a":{"newValue":1},"b":{"newValue":{"c":[true,null]}},"s":{"newValue":"text"}})"sv));

    EXPECT(equals(TRY_OR_FAIL(area.get(JsonValue {})), R"({"a":1,"b":{"c":[true,null]},"s":"text"})"sv));
    EXPECT(equals(TRY_OR_FAIL(area.get("a"sv)), R"({"a":1})"sv));
    EXPECT(equals(TRY_OR_FAIL(area.get("missing"sv)), "{}"sv));
    EXPECT(equals(TRY_OR_FAIL(area.get(json(R"(["a","s","missing"])"sv))), R"({"a":1,"s":"text"})"sv));
    EXPECT(equals(TRY_OR_FAIL(area.get(json("[]"sv))), "{}"sv));
    EXPECT(equals(TRY_OR_FAIL(area.get(json(R"({"a":5,"missing":"default","n":null})"sv))), R"({"a":1,"missing":"default","n":null})"sv));
    EXPECT(equals(TRY_OR_FAIL(area.get(json("{}"sv))), "{}"sv));
    EXPECT(equals(TRY_OR_FAIL(area.get(""sv)), "{}"sv));

    auto keys = area.get_keys();
    EXPECT_EQ(keys.size(), 3u);
    EXPECT(keys.contains_slow("a"_string));
    EXPECT(keys.contains_slow("b"_string));
    EXPECT(keys.contains_slow("s"_string));
    EXPECT_EQ(area.item_count(), 3u);
}

TEST_CASE(get_rejects_bad_keys)
{
    StorageArea area;
    EXPECT(area.get(JsonValue(5)).is_error());
    EXPECT(area.get(JsonValue(true)).is_error());
    EXPECT(area.get(json(R"(["a",1])"sv)).is_error());
    EXPECT(area.get(json(R"([["a"]])"sv)).is_error());
}

TEST_CASE(set_reports_only_real_changes)
{
    StorageArea area;
    TRY_OR_FAIL(area.set(object(R"({"a":1,"b":{"x":1,"y":2}})"sv)));

    // Identical values, including objects with members in another order, are not changes.
    auto unchanged = TRY_OR_FAIL(area.set(object(R"({"a":1,"b":{"y":2,"x":1}})"sv)));
    EXPECT(unchanged.is_empty());

    auto changes = TRY_OR_FAIL(area.set(object(R"({"a":2,"b":{"y":2,"x":1},"c":null})"sv)));
    EXPECT(equals(changes, R"({"a":{"oldValue":1,"newValue":2},"c":{"newValue":null}})"sv));
    EXPECT(equals(TRY_OR_FAIL(area.get("c"sv)), R"({"c":null})"sv));

    EXPECT(TRY_OR_FAIL(area.set({})).is_empty());
}

TEST_CASE(remove_and_clear)
{
    StorageArea area;
    TRY_OR_FAIL(area.set(object(R"({"a":1,"b":2,"c":3,"d":4})"sv)));

    EXPECT(equals(TRY_OR_FAIL(area.remove("a"sv)), R"({"a":{"oldValue":1}})"sv));
    EXPECT(TRY_OR_FAIL(area.remove("a"sv)).is_empty());
    EXPECT(equals(TRY_OR_FAIL(area.remove(json(R"(["b","missing","c"])"sv))), R"({"b":{"oldValue":2},"c":{"oldValue":3}})"sv));
    EXPECT(TRY_OR_FAIL(area.remove(json("[]"sv))).is_empty());
    EXPECT(area.remove(JsonValue {}).is_error());
    EXPECT(area.remove(JsonValue(1)).is_error());
    EXPECT(area.remove(json(R"(["d",1])"sv)).is_error());
    EXPECT(area.remove(json(R"({"d":1})"sv)).is_error());
    EXPECT_EQ(area.item_count(), 1u);

    TRY_OR_FAIL(area.set(object(R"({"e":"five"})"sv)));
    EXPECT(equals(area.clear(), R"({"d":{"oldValue":4},"e":{"oldValue":"five"}})"sv));
    EXPECT_EQ(area.item_count(), 0u);
    EXPECT_EQ(area.total_bytes_in_use(), 0u);
    EXPECT(area.clear().is_empty());
}

TEST_CASE(bytes_in_use)
{
    EXPECT_EQ(StorageArea::item_size("key"sv, JsonValue("value"sv)), 3u + 7u);
    EXPECT_EQ(StorageArea::item_size("n"sv, JsonValue(12345)), 1u + 5u);
    EXPECT_EQ(StorageArea::item_size("o"sv, json(R"({"a":[1,2]})"sv)), 1u + 11u);
    // Keys count in UTF-8 bytes.
    EXPECT_EQ(StorageArea::item_size("é"sv, JsonValue(true)), 2u + 4u);

    StorageArea area;
    TRY_OR_FAIL(area.set(object(R"({"key":"value","n":12345})"sv)));
    EXPECT_EQ(TRY_OR_FAIL(area.get_bytes_in_use(JsonValue {})), 16u);
    EXPECT_EQ(area.total_bytes_in_use(), 16u);
    EXPECT_EQ(TRY_OR_FAIL(area.get_bytes_in_use("key"sv)), 10u);
    EXPECT_EQ(TRY_OR_FAIL(area.get_bytes_in_use("missing"sv)), 0u);
    EXPECT_EQ(TRY_OR_FAIL(area.get_bytes_in_use(json(R"(["key","n","missing"])"sv))), 16u);
    EXPECT_EQ(TRY_OR_FAIL(area.get_bytes_in_use(json("[]"sv))), 0u);
    EXPECT(area.get_bytes_in_use(JsonValue(1)).is_error());
    EXPECT(area.get_bytes_in_use(json(R"({"key":1})"sv)).is_error());

    TRY_OR_FAIL(area.set(object(R"({"key":"v"})"sv)));
    EXPECT_EQ(area.total_bytes_in_use(), 12u);
    TRY_OR_FAIL(area.remove("n"sv));
    EXPECT_EQ(area.total_bytes_in_use(), 6u);
}

TEST_CASE(total_quota_is_atomic)
{
    StorageArea area(StorageQuota { .quota_bytes = 20, .quota_bytes_per_item = {}, .max_items = {} });
    TRY_OR_FAIL(area.set(object(R"({"a":"12345678"})"sv)));
    EXPECT_EQ(area.total_bytes_in_use(), 11u);

    // 11 + 11 > 20: nothing at all is written.
    EXPECT(area.set(object(R"({"z":1,"b":"12345678"})"sv)).is_error());
    EXPECT(equals(TRY_OR_FAIL(area.get(JsonValue {})), R"({"a":"12345678"})"sv));
    EXPECT_EQ(area.total_bytes_in_use(), 11u);

    // Replacing a value counts the difference.
    TRY_OR_FAIL(area.set(object(R"({"a":"12345678901234567"})"sv)));
    EXPECT_EQ(area.total_bytes_in_use(), 20u);
    EXPECT(area.set(object(R"({"a":"123456789012345678"})"sv)).is_error());

    // Shrinking and growing in one call is judged by the result.
    TRY_OR_FAIL(area.set(object(R"({"a":1,"b":"12345"})"sv)));
    EXPECT_EQ(area.total_bytes_in_use(), 2u + 8u);
}

TEST_CASE(sync_quotas)
{
    StorageArea area(StorageQuota::sync());

    auto item = [](StringView key, size_t value_length) {
        JsonObject items;
        items.set(key, JsonValue(MUST(String::repeated('x', value_length))));
        return items;
    };

    // QUOTA_BYTES_PER_ITEM counts the key and the quoted value.
    TRY_OR_FAIL(area.set(item("k"sv, 8192 - 1 - 2)));
    EXPECT(area.set(item("j"sv, 8192 - 1 - 2 + 1)).is_error());
    EXPECT_EQ(area.item_count(), 1u);
    area.clear();

    // MAX_ITEMS.
    JsonObject many;
    for (size_t i = 0; i < 512; ++i)
        many.set(MUST(String::formatted("k{}", i)), JsonValue(1));
    TRY_OR_FAIL(area.set(many));
    EXPECT_EQ(area.item_count(), 512u);
    EXPECT(area.set(object(R"({"one_more":1})"sv)).is_error());
    TRY_OR_FAIL(area.set(object(R"({"k0":2})"sv)));
    EXPECT_EQ(area.item_count(), 512u);
    area.clear();

    // QUOTA_BYTES.
    for (size_t i = 0; i < 12; ++i)
        TRY_OR_FAIL(area.set(item(MUST(String::formatted("item{}", i)), 8000)));
    EXPECT(area.set(item("item12"sv, 8000)).is_error());
    EXPECT_EQ(area.item_count(), 12u);
    EXPECT(area.total_bytes_in_use() <= 102400u);
}

TEST_CASE(unlimited_local)
{
    StorageArea area(StorageQuota::local(true));
    JsonObject items;
    items.set("big"sv, JsonValue(MUST(String::repeated('x', 11 * MiB))));
    TRY_OR_FAIL(area.set(items));

    StorageArea limited(StorageQuota::local());
    EXPECT(limited.set(items).is_error());
    EXPECT_EQ(limited.item_count(), 0u);
}

TEST_CASE(many_keys)
{
    StorageArea area(StorageQuota::local());
    for (size_t i = 0; i < 5000; ++i) {
        JsonObject items;
        items.set(MUST(String::formatted("key-{}", i)), JsonValue(i));
        TRY_OR_FAIL(area.set(items));
    }
    EXPECT_EQ(area.item_count(), 5000u);
    EXPECT(equals(TRY_OR_FAIL(area.get("key-4999"sv)), R"({"key-4999":4999})"sv));
    EXPECT_EQ(TRY_OR_FAIL(area.get(JsonValue {})).size(), 5000u);
}

TEST_CASE(save_and_load)
{
    auto root = make_test_root();
    auto path = LexicalPath::join(root, "local.json"sv).string();

    StorageArea area(StorageQuota::local());
    TRY_OR_FAIL(area.set(object(R"({"a":1,"b":{"c":["d",null,true,1.5]},"unicode":"héllo \u0000 \"q\""})"sv)));
    TRY_OR_FAIL(area.save_to_file(path));

    auto stat = TRY_OR_FAIL(Core::System::stat(path));
    EXPECT_EQ(static_cast<unsigned>(stat.st_mode & 0777), 0600u);

    auto loaded = TRY_OR_FAIL(StorageArea::load_from_file(path, StorageQuota::local()));
    EXPECT(WebExtensions::json_values_equal(TRY_OR_FAIL(loaded.get(JsonValue {})), TRY_OR_FAIL(area.get(JsonValue {}))));
    EXPECT_EQ(loaded.total_bytes_in_use(), area.total_bytes_in_use());
    EXPECT_EQ(loaded.quota().quota_bytes, 10485760u);

    // Saving again replaces the file and leaves no temporary files behind.
    TRY_OR_FAIL(area.remove("a"sv));
    TRY_OR_FAIL(area.save_to_file(path));
    auto reloaded = TRY_OR_FAIL(StorageArea::load_from_file(path, StorageQuota::local()));
    EXPECT_EQ(reloaded.item_count(), 2u);

    size_t entries = 0;
    TRY_OR_FAIL(Core::Directory::for_each_entry(root, Core::DirIterator::SkipParentAndBaseDir, [&](auto const&, auto const&) -> ErrorOr<IterationDecision> {
        ++entries;
        return IterationDecision::Continue;
    }));
    EXPECT_EQ(entries, 1u);

    remove_test_root();
}

TEST_CASE(load_missing_and_corrupt_files)
{
    auto root = make_test_root();

    auto missing = TRY_OR_FAIL(StorageArea::load_from_file(LexicalPath::join(root, "missing.json"sv).string(), StorageQuota::sync()));
    EXPECT_EQ(missing.item_count(), 0u);
    EXPECT_EQ(missing.quota().max_items, 512u);

    auto write = [&](StringView name, StringView contents) {
        auto path = LexicalPath::join(root, name).string();
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(contents.bytes()));
        return path;
    };

    EXPECT(StorageArea::load_from_file(write("garbage.json"sv, "{ nope"sv), StorageQuota::local()).is_error());
    EXPECT(StorageArea::load_from_file(write("array.json"sv, "[1,2]"sv), StorageQuota::local()).is_error());
    EXPECT(StorageArea::load_from_file(write("empty.json"sv, ""sv), StorageQuota::local()).is_error());
    EXPECT(StorageArea::load_from_file(write("binary.json"sv, "\xff\xfe\x00"sv), StorageQuota::local()).is_error());
    EXPECT(StorageArea::load_from_file(root, StorageQuota::local()).is_error());

    // Quotas are not enforced on load, but later writes are judged against them.
    auto over = TRY_OR_FAIL(StorageArea::load_from_file(write("over.json"sv, R"({"a":"0123456789"})"sv), StorageQuota { .quota_bytes = 5, .quota_bytes_per_item = {}, .max_items = {} }));
    EXPECT_EQ(over.item_count(), 1u);
    EXPECT(over.set(object(R"({"a":"012345"})"sv)).is_error());
    TRY_OR_FAIL(over.remove("a"sv));
    TRY_OR_FAIL(over.set(object(R"({"a":"0"})"sv)));

    remove_test_root();
}

TEST_CASE(save_to_unwritable_location)
{
    StorageArea area;
    TRY_OR_FAIL(area.set(object(R"({"a":1})"sv)));
    EXPECT(area.save_to_file("/nonexistent-directory/storage.json"sv).is_error());
}
