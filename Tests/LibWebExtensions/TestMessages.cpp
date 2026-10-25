/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebExtensions/Manifest.h>
#include <LibWebExtensions/Messages.h>

using namespace WebExtensions;

static MessageCatalog catalog_from(StringView json)
{
    MessageCatalog catalog;
    MUST(catalog.add_catalog("en"sv, json));
    return catalog;
}

static bool catalog_fails(StringView json)
{
    MessageCatalog catalog;
    return catalog.add_catalog("en"sv, json).is_error();
}

TEST_CASE(simple_message)
{
    auto catalog = catalog_from(R"~({"hello": {"message": "Hello world", "description": "greeting"}})~"sv);
    EXPECT_EQ(catalog.get_message("hello"sv), "Hello world"sv);
    EXPECT(!catalog.get_message("missing"sv).has_value());
    EXPECT(!catalog.get_message(""sv).has_value());
}

TEST_CASE(names_are_case_insensitive)
{
    auto catalog = catalog_from(R"~({"extName": {"message": "1Password"}})~"sv);
    EXPECT_EQ(catalog.get_message("extName"sv), "1Password"sv);
    EXPECT_EQ(catalog.get_message("extname"sv), "1Password"sv);
    EXPECT_EQ(catalog.get_message("EXTNAME"sv), "1Password"sv);
}

TEST_CASE(positional_substitutions)
{
    auto catalog = catalog_from(R"~({"m": {"message": "$1 has $2 items, not $3"}})~"sv);
    EXPECT_EQ(catalog.get_message("m"sv, { "Alice"_string, "3"_string }), "Alice has 3 items, not "sv);
    EXPECT_EQ(catalog.get_message("m"sv), " has  items, not "sv);

    catalog = catalog_from(R"~({"m": {"message": "$9-$1"}})~"sv);
    Vector<String> nine;
    for (int i = 1; i <= 9; ++i)
        nine.append(String::number(i));
    EXPECT_EQ(catalog.get_message("m"sv, nine), "9-1"sv);

    // Chrome rejects more than nine substitutions.
    nine.append("10"_string);
    EXPECT(!catalog.get_message("m"sv, nine).has_value());
}

TEST_CASE(dollar_escapes_follow_chrome)
{
    auto catalog = catalog_from(R"~({
        "price": {"message": "Costs $$5"},
        "triple": {"message": "a$$$1"},
        "invalid": {"message": "x$0y"},
        "letter": {"message": "q$z!"},
        "trailing": {"message": "end$"}
    })~"sv);
    EXPECT_EQ(catalog.get_message("price"sv), "Costs $5"sv);
    // base::ReplaceStringPlaceholders: each '$' after the first in a run is kept.
    EXPECT_EQ(catalog.get_message("triple"sv, { "S"_string }), "a$$1"sv);
    EXPECT_EQ(catalog.get_message("invalid"sv), "xy"sv);
    EXPECT_EQ(catalog.get_message("letter"sv), "q!"sv);
    EXPECT_EQ(catalog.get_message("trailing"sv), "end"sv);
}

TEST_CASE(named_placeholders)
{
    auto catalog = catalog_from(R"~({
        "error": {
            "message": "Error: $Details$ (from $user$)",
            "placeholders": {
                "details": {"content": "$1", "example": "Failed"},
                "USER": {"content": "$2"}
            }
        },
        "fixed": {
            "message": "Visit $site$!",
            "placeholders": {"site": {"content": "example.com"}}
        }
    })~"sv);
    EXPECT_EQ(catalog.get_message("error"sv, { "boom"_string, "bob"_string }), "Error: boom (from bob)"sv);
    EXPECT_EQ(catalog.get_message("fixed"sv), "Visit example.com!"sv);
}

TEST_CASE(placeholder_errors)
{
    // Placeholder used but not defined.
    EXPECT(catalog_fails(R"~({"m": {"message": "Hi $name$"}})~"sv));
    // Placeholder without content.
    EXPECT(catalog_fails(R"~({"m": {"message": "Hi $name$", "placeholders": {"name": {}}}})~"sv));
    EXPECT(catalog_fails(R"~({"m": {"message": "Hi $name$", "placeholders": {"name": {"content": 1}}}})~"sv));
    EXPECT(catalog_fails(R"~({"m": {"message": "Hi", "placeholders": []}})~"sv));
    EXPECT(catalog_fails(R"~({"m": {"message": "Hi", "placeholders": {"bad name": {"content": "x"}}}})~"sv));
    // A placeholder that expands into itself must not hang.
    EXPECT(catalog_fails(R"~({"m": {"message": "$a$", "placeholders": {"a": {"content": "x$a$"}}}})~"sv));
    // "$0y$" looks like a placeholder reference, so Chrome rejects it.
    EXPECT(catalog_fails(R"~({"m": {"message": "x$0y$z"}})~"sv));
    // "$1 and $2" contains no valid $name$ token, so it loads.
    EXPECT(!catalog_fails(R"~({"m": {"message": "$1 and $2"}})~"sv));
}

TEST_CASE(malformed_catalogs)
{
    EXPECT(catalog_fails(""sv));
    EXPECT(catalog_fails("[]"sv));
    EXPECT(catalog_fails("{"sv));
    EXPECT(catalog_fails(R"~({"m": "just a string"})~"sv));
    EXPECT(catalog_fails(R"~({"m": {}})~"sv));
    EXPECT(catalog_fails(R"~({"m": {"message": 5}})~"sv));
    EXPECT(catalog_fails(R"~({"bad-name": {"message": "x"}})~"sv));
    EXPECT(catalog_fails(R"~({"": {"message": "x"}})~"sv));
    EXPECT(catalog_fails(R"~({"@@extension_id": {"message": "x"}})~"sv));
    EXPECT(catalog_fails("\xff{}"sv));
    EXPECT(!catalog_fails("{}"sv));
    // Comments and a BOM are allowed, as in Chrome.
    EXPECT(!catalog_fails("\xEF\xBB\xBF{ // c\n\"a\": {\"message\": \"b\"}}"sv));
}

TEST_CASE(predefined_messages)
{
    MessageCatalog catalog;
    catalog.set_extension_id("aeblfdkhhhdcdjpifhhbdiojplfjncoa"_string);
    catalog.set_ui_locale("en-US"sv);
    EXPECT_EQ(catalog.get_message("@@extension_id"sv), "aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv);
    EXPECT_EQ(catalog.get_message("@@EXTENSION_ID"sv), "aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv);
    EXPECT_EQ(catalog.get_message("@@ui_locale"sv), "en_US"sv);
    EXPECT_EQ(catalog.get_message("@@bidi_dir"sv), "ltr"sv);
    EXPECT_EQ(catalog.get_message("@@bidi_reversed_dir"sv), "rtl"sv);
    EXPECT_EQ(catalog.get_message("@@bidi_start_edge"sv), "left"sv);
    EXPECT_EQ(catalog.get_message("@@bidi_end_edge"sv), "right"sv);
    EXPECT(!catalog.get_message("@@unknown"sv).has_value());

    catalog.set_ui_locale("he"sv);
    EXPECT_EQ(catalog.get_message("@@bidi_dir"sv), "rtl"sv);
    EXPECT_EQ(catalog.get_message("@@bidi_reversed_dir"sv), "ltr"sv);
    EXPECT_EQ(catalog.get_message("@@bidi_start_edge"sv), "right"sv);
    EXPECT_EQ(catalog.get_message("@@bidi_end_edge"sv), "left"sv);
    catalog.set_ui_locale("ar_EG"sv);
    EXPECT_EQ(catalog.get_message("@@bidi_dir"sv), "rtl"sv);
    catalog.set_ui_locale("fa"sv);
    EXPECT_EQ(catalog.get_message("@@bidi_dir"sv), "rtl"sv);

    // Without an id, @@extension_id is unavailable.
    MessageCatalog anonymous;
    EXPECT(!anonymous.get_message("@@extension_id"sv).has_value());
}

TEST_CASE(catalog_priority)
{
    MessageCatalog catalog;
    MUST(catalog.add_catalog("en_US"sv, R"~({"color": {"message": "color"}})~"sv));
    MUST(catalog.add_catalog("en"sv, R"~({"color": {"message": "colour"}, "only_en": {"message": "en"}})~"sv));
    EXPECT_EQ(catalog.get_message("color"sv), "color"sv);
    EXPECT_EQ(catalog.get_message("only_en"sv), "en"sv);
}

TEST_CASE(substitute_manifest_messages)
{
    auto catalog = catalog_from(R"~({
        "extName": {"message": "1Password – Password Manager"},
        "a_b": {"message": "AB"},
        "dollar": {"message": "$$x"}
    })~"sv);
    EXPECT_EQ(catalog.substitute_manifest_messages("__MSG_extName__"sv), "1Password – Password Manager"sv);
    EXPECT_EQ(catalog.substitute_manifest_messages("__MSG_EXTNAME__"sv), "1Password – Password Manager"sv);
    EXPECT_EQ(catalog.substitute_manifest_messages("Pre __MSG_a_b__ and __MSG_extName__."sv), "Pre AB and 1Password – Password Manager."sv);
    EXPECT_EQ(catalog.substitute_manifest_messages("no tokens"sv), "no tokens"sv);
    EXPECT_EQ(catalog.substitute_manifest_messages("__MSG_unknown__"sv), "__MSG_unknown__"sv);
    EXPECT_EQ(catalog.substitute_manifest_messages("__MSG_unknown__ __MSG_a_b__"sv), "__MSG_unknown__ AB"sv);
    EXPECT_EQ(catalog.substitute_manifest_messages("__MSG_extName"sv), "__MSG_extName"sv);
    EXPECT_EQ(catalog.substitute_manifest_messages("__MSG___"sv), "__MSG___"sv);
    EXPECT_EQ(catalog.substitute_manifest_messages("__MSG_bad name__"sv), "__MSG_bad name__"sv);
    EXPECT_EQ(catalog.substitute_manifest_messages(""sv), ""sv);
    // Manifest substitution doesn't apply $ escapes.
    EXPECT_EQ(catalog.substitute_manifest_messages("__MSG_dollar__"sv), "$$x"sv);
}

TEST_CASE(normalize_locale_name)
{
    EXPECT_EQ(WebExtensions::normalize_locale_name("en-US"sv), "en_US"sv);
    EXPECT_EQ(WebExtensions::normalize_locale_name("zh_CN"sv), "zh_CN"sv);
    EXPECT_EQ(WebExtensions::normalize_locale_name("en"sv), "en"sv);
    EXPECT(!WebExtensions::normalize_locale_name(""sv).has_value());
    EXPECT(!WebExtensions::normalize_locale_name("../../etc"sv).has_value());
    EXPECT(!WebExtensions::normalize_locale_name("en/US"sv).has_value());
    EXPECT(!WebExtensions::normalize_locale_name("."sv).has_value());
    EXPECT(!WebExtensions::normalize_locale_name("en US"sv).has_value());
}

// On-disk loading.

static ByteString test_root()
{
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-webext-messages-{}", Core::System::getpid())).string();
}

static void remove_test_root()
{
    if (FileSystem::exists(test_root()))
        MUST(FileSystem::remove(test_root(), FileSystem::RecursionMode::Allowed));
}

static void write_catalog(StringView locale, StringView json)
{
    auto directory = LexicalPath::join(test_root(), "_locales"sv, locale).string();
    MUST(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    auto file = MUST(Core::File::open(LexicalPath::join(directory, "messages.json"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(file->write_until_depleted(json.bytes()));
}

TEST_CASE(load_fallback_chain)
{
    remove_test_root();
    write_catalog("en"sv, R"~({"a": {"message": "en a"}, "b": {"message": "en b"}, "c": {"message": "en c"}})~"sv);
    write_catalog("fr"sv, R"~({"a": {"message": "fr a"}, "b": {"message": "fr b"}})~"sv);
    write_catalog("fr_CA"sv, R"~({"a": {"message": "fr_CA a"}})~"sv);
    write_catalog("de"sv, "this is not json"sv);

    auto catalog = TRY_OR_FAIL(MessageCatalog::load(test_root(), "en"sv, { "fr-CA"_string }, "aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv));
    EXPECT_EQ(catalog.get_message("a"sv), "fr_CA a"sv);
    EXPECT_EQ(catalog.get_message("b"sv), "fr b"sv);
    EXPECT_EQ(catalog.get_message("c"sv), "en c"sv);
    EXPECT_EQ(catalog.get_message("@@ui_locale"sv), "fr_CA"sv);
    EXPECT_EQ(catalog.get_message("@@extension_id"sv), "aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv);
    EXPECT_EQ(catalog.loaded_locales(), (Vector<String> { "fr_CA"_string, "fr"_string, "en"_string }));

    // A locale the extension doesn't ship falls back to the default.
    catalog = TRY_OR_FAIL(MessageCatalog::load(test_root(), "en"sv, { "ja"_string }));
    EXPECT_EQ(catalog.get_message("a"sv), "en a"sv);
    EXPECT_EQ(catalog.loaded_locales(), (Vector<String> { "en"_string }));

    // Several preferred locales are tried in order.
    catalog = TRY_OR_FAIL(MessageCatalog::load(test_root(), "en"sv, { "ja"_string, "fr"_string }));
    EXPECT_EQ(catalog.get_message("a"sv), "fr a"sv);

    // A malformed catalog that gets selected is an error.
    EXPECT(MessageCatalog::load(test_root(), "en"sv, { "de"_string }).is_error());
    // Path traversal in locale names is never followed.
    EXPECT(MessageCatalog::load(test_root(), "../en"sv, {}).is_error());
    catalog = TRY_OR_FAIL(MessageCatalog::load(test_root(), "en"sv, { "../../x"_string }));
    EXPECT_EQ(catalog.loaded_locales(), (Vector<String> { "en"_string }));
    remove_test_root();
}

TEST_CASE(load_default_locale_rules)
{
    remove_test_root();
    MUST(Core::Directory::create(test_root(), Core::Directory::CreateDirectories::Yes));

    // No _locales and no default_locale: an empty catalog.
    auto catalog = TRY_OR_FAIL(MessageCatalog::load(test_root(), ""sv, { "en"_string }));
    EXPECT(catalog.is_empty());
    EXPECT(!catalog.get_message("x"sv).has_value());

    // default_locale without _locales.
    EXPECT(MessageCatalog::load(test_root(), "en"sv, {}).is_error());

    // _locales without default_locale.
    write_catalog("en"sv, R"~({"a": {"message": "en a"}})~"sv);
    EXPECT(MessageCatalog::load(test_root(), ""sv, {}).is_error());

    // default_locale whose catalog is missing.
    EXPECT(MessageCatalog::load(test_root(), "fr"sv, {}).is_error());

    // A missing extension directory is an error only if a locale is required.
    EXPECT(MessageCatalog::load("/nonexistent/extension/root"sv, "en"sv, {}).is_error());
    remove_test_root();
}

TEST_CASE(localize_manifest)
{
    auto manifest = MUST(Manifest::parse(R"~({"manifest_version": 3, "name": "__MSG_extName__", "version": "1",
        "short_name": "__MSG_short__", "description": "__MSG_extDescription__",
        "action": {"default_title": "__MSG_title__"},
        "commands": {"lock": {"description": "__MSG_lock__"}}})~"sv));
    auto catalog = catalog_from(R"~({
        "extName": {"message": "  1Password – Password Manager "},
        "extDescription": {"message": "The best way"},
        "short": {"message": "1P"},
        "title": {"message": "Open 1Password"},
        "lock": {"message": "Lock"}
    })~"sv);
    localize(manifest, catalog);
    EXPECT_EQ(manifest.name, "1Password – Password Manager"sv);
    EXPECT_EQ(manifest.short_name, "1P"sv);
    EXPECT_EQ(manifest.description, "The best way"sv);
    EXPECT_EQ(manifest.action->default_title, "Open 1Password"sv);
    EXPECT_EQ(manifest.command("lock"sv)->description, "Lock"sv);
}
