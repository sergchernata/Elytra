/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <LibTest/TestCase.h>
#include <LibWebExtensions/ExtensionId.h>

// The public "key" from 1Password's published manifest.json.
static constexpr auto onepassword_key = "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAnHpaUll4uWujpAdbIXOQY2WE6hk8PllsYsnoUaj5qHXwv4IB6A9pONqGaTL2KL20u6E6XVhncY6Ae6SQSBQqiIkgjPsiG0NDNsDlju/kzBnfimKFC/bpzOrqFqbhswQHifnet5uHlpG97whTzLO3ka0M5aqB9V9mD/0qVXvNgAVVnSTULH254YqpeCcAhmsKiFZSL6OrOZmCp8kZ/OeOUK9iYWYylL7VcOXVrZf10EPrlaCNXzVk7K35dPuQ7svhA0Pgju3kngB4RLa5Iojhw3IT+B5+m8pisjOSd1oKMrRmhGs7rDhF5IEtAiVxqVp7uOOMPQj3vrbMDAzf7vqLtQIDAQAB"sv;

TEST_CASE(id_from_base64_key_matches_chrome)
{
    auto id = TRY_OR_FAIL(WebExtensions::extension_id_from_base64_key(onepassword_key));
    EXPECT_EQ(id, "aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv);
}

TEST_CASE(id_from_public_key_bytes)
{
    auto der = TRY_OR_FAIL(decode_base64(onepassword_key));
    auto id = TRY_OR_FAIL(WebExtensions::extension_id_from_public_key(der.bytes()));
    EXPECT_EQ(id, "aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv);
}

TEST_CASE(id_from_public_key_rejects_empty)
{
    EXPECT(WebExtensions::extension_id_from_public_key({}).is_error());
}

TEST_CASE(id_from_base64_key_tolerates_whitespace)
{
    StringBuilder builder;
    builder.append("  \n"sv);
    auto key = onepassword_key;
    for (size_t i = 0; i < key.length(); ++i) {
        builder.append(key[i]);
        if (i % 64 == 63)
            builder.append("\r\n"sv);
        if (i == 10)
            builder.append(" \t"sv);
    }
    builder.append("\n "sv);
    auto id = TRY_OR_FAIL(WebExtensions::extension_id_from_base64_key(builder.string_view()));
    EXPECT_EQ(id, "aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv);
}

TEST_CASE(id_from_base64_key_tolerates_pem_armor)
{
    auto pem = ByteString::formatted("-----BEGIN PUBLIC KEY-----\n{}\n-----END PUBLIC KEY-----\n", onepassword_key);
    auto id = TRY_OR_FAIL(WebExtensions::extension_id_from_base64_key(pem));
    EXPECT_EQ(id, "aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv);
}

TEST_CASE(id_from_base64_key_rejects_garbage)
{
    EXPECT(WebExtensions::extension_id_from_base64_key(""sv).is_error());
    EXPECT(WebExtensions::extension_id_from_base64_key("   \n"sv).is_error());
    EXPECT(WebExtensions::extension_id_from_base64_key("not base64!!"sv).is_error());
    EXPECT(WebExtensions::extension_id_from_base64_key("-----BEGIN PUBLIC KEY-----"sv).is_error());
    EXPECT(WebExtensions::extension_id_from_base64_key("-----BEGIN PUBLIC KEY-----\n-----END PUBLIC KEY-----"sv).is_error());
}

TEST_CASE(id_from_path)
{
    // Chrome's own id_util test vector: GenerateId("test").
    EXPECT_EQ(WebExtensions::extension_id_from_path("test"sv), "jpignaibiiemhngfjkcpokkamffknabf"sv);
    EXPECT_EQ(WebExtensions::extension_id_from_path("/Users/test/my-extension"sv), "diakbeaocgjfnikcbajlpmifbnddiilp"sv);
    EXPECT_EQ(WebExtensions::extension_id_from_path("/tmp/ünï"sv), "lgidenflndoliibcgjengdhllljpgnbe"sv);
    EXPECT_EQ(WebExtensions::extension_id_from_path(""sv), "odlameecjipmbmbejkplpemijjgpljce"sv);
}

TEST_CASE(generated_ids_are_valid)
{
    EXPECT(WebExtensions::is_valid_extension_id(WebExtensions::extension_id_from_path("/a/b"sv)));
}

TEST_CASE(is_valid_extension_id)
{
    EXPECT(WebExtensions::is_valid_extension_id("aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv));
    EXPECT(WebExtensions::is_valid_extension_id("pppppppppppppppppppppppppppppppp"sv));
    EXPECT(!WebExtensions::is_valid_extension_id(""sv));
    EXPECT(!WebExtensions::is_valid_extension_id("aeblfdkhhhdcdjpifhhbdiojplfjnco"sv));
    EXPECT(!WebExtensions::is_valid_extension_id("aeblfdkhhhdcdjpifhhbdiojplfjncoaa"sv));
    EXPECT(!WebExtensions::is_valid_extension_id("qeblfdkhhhdcdjpifhhbdiojplfjncoa"sv));
    EXPECT(!WebExtensions::is_valid_extension_id("AEBLFDKHHHDCDJPIFHHBDIOJPLFJNCOA"sv));
    EXPECT(!WebExtensions::is_valid_extension_id("aeblfdkhhhdcdjpifhhbdiojplfjnc0a"sv));
    EXPECT(!WebExtensions::is_valid_extension_id("aeblfdkhhhdcdjpifhhbdiojplfjnc/a"sv));
}
