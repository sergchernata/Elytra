/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebExtensions/NativeMessaging.h>

static constexpr auto extension_id = "aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv;
static constexpr auto other_extension_id = "dppgmdbiimibapkepcbdbmkaabgiofem"sv;

static ByteString test_root()
{
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-native-messaging-{}", Core::System::getpid())).string();
}

static void remove_test_root()
{
    if (FileSystem::exists(test_root()))
        MUST(FileSystem::remove(test_root(), FileSystem::RecursionMode::Allowed));
}

static ByteString make_directory(StringView name)
{
    auto path = LexicalPath::join(test_root(), name).string();
    if (!FileSystem::exists(test_root()))
        MUST(Core::System::mkdir(test_root(), 0700));
    MUST(Core::System::mkdir(path, 0700));
    return path;
}

static void write_file(ByteString const& path, StringView contents, mode_t mode = 0600)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate, mode));
    MUST(file->write_until_depleted(contents.bytes()));
    MUST(Core::System::chmod(path, mode));
}

static ByteString manifest_json(StringView name, StringView path, StringView origins = "[\"chrome-extension://aeblfdkhhhdcdjpifhhbdiojplfjncoa/\"]"sv, StringView type = "stdio"sv)
{
    return ByteString::formatted(R"({{"name": "{}", "description": "Test host", "path": "{}", "type": "{}", "allowed_origins": {}}})", name, path, type, origins);
}

TEST_CASE(host_names)
{
    using WebExtensions::NativeHostManifest;
    EXPECT(NativeHostManifest::is_valid_name("com.1password.1password"sv));
    EXPECT(NativeHostManifest::is_valid_name("com.my_company.my_application"sv));
    EXPECT(NativeHostManifest::is_valid_name("a"sv));
    EXPECT(NativeHostManifest::is_valid_name("a_b.c9"sv));
    EXPECT(!NativeHostManifest::is_valid_name(""sv));
    EXPECT(!NativeHostManifest::is_valid_name("Com.example"sv));
    EXPECT(!NativeHostManifest::is_valid_name(".com.example"sv));
    EXPECT(!NativeHostManifest::is_valid_name("com.example."sv));
    EXPECT(!NativeHostManifest::is_valid_name("com..example"sv));
    EXPECT(!NativeHostManifest::is_valid_name("com-example"sv));
    EXPECT(!NativeHostManifest::is_valid_name("../evil"sv));
    EXPECT(!NativeHostManifest::is_valid_name("com/example"sv));
    EXPECT(!NativeHostManifest::is_valid_name("com example"sv));
}

TEST_CASE(parse_valid_manifest)
{
    auto json = R"({
        "name": "com.1password.1password",
        "description": "1Password BrowserSupport",
        "path": "/Applications/1Password.app/Contents/Library/LoginItems/1Password Browser Helper.app/Contents/MacOS/1Password-BrowserSupport",
        "type": "stdio",
        "allowed_origins": [
            "chrome-extension://aeblfdkhhhdcdjpifhhbdiojplfjncoa/",
            "chrome-extension://dppgmdbiimibapkepcbdbmkaabgiofem/"
        ]
    })"sv;
    auto manifest = TRY_OR_FAIL(WebExtensions::NativeHostManifest::parse(json, "/Library/Google/Chrome/NativeMessagingHosts"sv));
    EXPECT_EQ(manifest.name, "com.1password.1password"sv);
    EXPECT_EQ(manifest.description, "1Password BrowserSupport"sv);
    EXPECT_EQ(manifest.path, "/Applications/1Password.app/Contents/Library/LoginItems/1Password Browser Helper.app/Contents/MacOS/1Password-BrowserSupport"sv);
    EXPECT_EQ(manifest.allowed_origins.size(), 2u);
    EXPECT(manifest.allows_extension(extension_id));
    EXPECT(manifest.allows_extension(other_extension_id));
    EXPECT(!manifest.allows_extension("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"sv));
    EXPECT(!manifest.allows_extension(""sv));
    EXPECT(!manifest.allows_extension("chrome-extension://aeblfdkhhhdcdjpifhhbdiojplfjncoa/"sv));
}

TEST_CASE(parse_empty_allowed_origins)
{
    auto manifest = TRY_OR_FAIL(WebExtensions::NativeHostManifest::parse(manifest_json("com.example.host"sv, "/bin/cat"sv, "[]"sv), "/tmp"sv));
    EXPECT(!manifest.allows_extension(extension_id));
}

TEST_CASE(parse_rejects_invalid_manifests)
{
    auto rejects = [](StringView json) {
        return WebExtensions::NativeHostManifest::parse(json, "/tmp"sv).is_error();
    };

    EXPECT(rejects(""sv));
    EXPECT(rejects("not json"sv));
    EXPECT(rejects("[]"sv));
    EXPECT(rejects("null"sv));
    EXPECT(rejects("\"string\""sv));
    EXPECT(rejects("{}"sv));
    EXPECT(rejects(R"({"name": "com.example.host", "path": "/bin/cat", "type": "stdio", "allowed_origins": []})"sv));
    EXPECT(rejects(R"({"name": "com.example.host", "description": 5, "path": "/bin/cat", "type": "stdio", "allowed_origins": []})"sv));
    EXPECT(rejects(R"({"description": "x", "path": "/bin/cat", "type": "stdio", "allowed_origins": []})"sv));
    EXPECT(rejects(R"({"name": "com.example.host", "description": "x", "type": "stdio", "allowed_origins": []})"sv));
    EXPECT(rejects(R"({"name": "com.example.host", "description": "x", "path": "/bin/cat", "allowed_origins": []})"sv));
    EXPECT(rejects(R"({"name": "com.example.host", "description": "x", "path": "/bin/cat", "type": "stdio"})"sv));
    EXPECT(rejects(R"({"name": "com.example.host", "description": "x", "path": "/bin/cat", "type": "stdio", "allowed_origins": "chrome-extension://aeblfdkhhhdcdjpifhhbdiojplfjncoa/"})"sv));
    EXPECT(rejects(R"({"name": "com.example.host", "description": "x", "path": "/bin/cat", "type": "stdio", "allowed_origins": [5]})"sv));
    EXPECT(rejects(R"({"name": 5, "description": "x", "path": "/bin/cat", "type": "stdio", "allowed_origins": []})"sv));
    EXPECT(rejects(R"({"name": "com.example.host", "description": "x", "path": ["/bin/cat"], "type": "stdio", "allowed_origins": []})"sv));

    EXPECT(rejects(manifest_json("Com.Example"sv, "/bin/cat"sv)));
    EXPECT(rejects(manifest_json("com..example"sv, "/bin/cat"sv)));
    EXPECT(rejects(manifest_json("com.example.host"sv, "/bin/cat"sv, "[]"sv, "pipe"sv)));
    EXPECT(rejects(manifest_json("com.example.host"sv, "/bin/cat"sv, "[]"sv, "STDIO"sv)));
    EXPECT(rejects(manifest_json("com.example.host"sv, ""sv)));
    EXPECT(rejects(manifest_json("com.example.host"sv, "/bin/cat\\u0000x"sv)));
#if !defined(AK_OS_WINDOWS)
    EXPECT(rejects(manifest_json("com.example.host"sv, "cat"sv)));
    EXPECT(rejects(manifest_json("com.example.host"sv, "./bin/cat"sv)));
#endif
}

TEST_CASE(parse_rejects_invalid_origins)
{
    auto rejects_origin = [](StringView origin) {
        auto origins = ByteString::formatted("[\"{}\"]", origin);
        return WebExtensions::NativeHostManifest::parse(manifest_json("com.example.host"sv, "/bin/cat"sv, origins), "/tmp"sv).is_error();
    };

    EXPECT(!rejects_origin("chrome-extension://aeblfdkhhhdcdjpifhhbdiojplfjncoa/"sv));
    EXPECT(rejects_origin("chrome-extension://*/"sv));
    EXPECT(rejects_origin("chrome-extension://*/*"sv));
    EXPECT(rejects_origin("<all_urls>"sv));
    EXPECT(rejects_origin("chrome-extension://aeblfdkhhhdcdjpifhhbdiojplfjncoa"sv));
    EXPECT(rejects_origin("chrome-extension://aeblfdkhhhdcdjpifhhbdiojplfjncoa/*"sv));
    EXPECT(rejects_origin("chrome-extension://aeblfdkhhhdcdjpifhhbdiojplfjnco/"sv));
    EXPECT(rejects_origin("chrome-extension://aeblfdkhhhdcdjpifhhbdiojplfjncoaa/"sv));
    EXPECT(rejects_origin("chrome-extension://AEBLFDKHHHDCDJPIFHHBDIOJPLFJNCOA/"sv));
    EXPECT(rejects_origin("chrome-extension://qeblfdkhhhdcdjpifhhbdiojplfjncoa/"sv));
    EXPECT(rejects_origin("moz-extension://aeblfdkhhhdcdjpifhhbdiojplfjncoa/"sv));
    EXPECT(rejects_origin("https://aeblfdkhhhdcdjpifhhbdiojplfjncoa/"sv));
    EXPECT(rejects_origin(""sv));
    EXPECT(rejects_origin("chrome-extension://"sv));
    EXPECT(rejects_origin("chrome-extension:///"sv));
}

TEST_CASE(find_host_in_search_order)
{
    remove_test_root();
    auto first = make_directory("first"sv);
    auto second = make_directory("second"sv);
    auto missing = LexicalPath::join(test_root(), "missing"sv).string();

    write_file(LexicalPath::join(second, "com.example.host.json"sv).string(), manifest_json("com.example.host"sv, "/second/host"sv));
    write_file(LexicalPath::join(second, "com.example.other.json"sv).string(), manifest_json("com.example.other"sv, "/second/other"sv));
    write_file(LexicalPath::join(first, "com.example.other.json"sv).string(), manifest_json("com.example.other"sv, "/first/other"sv));

    auto host = TRY_OR_FAIL(WebExtensions::find_native_host("com.example.host"sv, { missing, first, second }));
    EXPECT(host.has_value());
    EXPECT_EQ(host->path, "/second/host"sv);

    auto other = TRY_OR_FAIL(WebExtensions::find_native_host("com.example.other"sv, { missing, first, second }));
    EXPECT(other.has_value());
    EXPECT_EQ(other->path, "/first/other"sv);

    auto none = TRY_OR_FAIL(WebExtensions::find_native_host("com.example.none"sv, { missing, first, second }));
    EXPECT(!none.has_value());

    auto empty = TRY_OR_FAIL(WebExtensions::find_native_host("com.example.host"sv, {}));
    EXPECT(!empty.has_value());

    remove_test_root();
}

TEST_CASE(find_host_rejects_bad_names_and_manifests)
{
    remove_test_root();
    auto directory = make_directory("hosts"sv);
    auto fallback = make_directory("fallback"sv);

    // Names that could escape the directory are refused before touching the file system.
    write_file(LexicalPath::join(test_root(), "escape.json"sv).string(), manifest_json("escape"sv, "/bin/cat"sv));
    EXPECT(WebExtensions::find_native_host("../escape"sv, { directory }).is_error());
    EXPECT(WebExtensions::find_native_host(""sv, { directory }).is_error());
    EXPECT(WebExtensions::find_native_host("Com.Example"sv, { directory }).is_error());

    // As in Chrome, the first manifest that exists is used, even when it is invalid.
    write_file(LexicalPath::join(directory, "com.example.broken.json"sv).string(), "{ not json"sv);
    write_file(LexicalPath::join(fallback, "com.example.broken.json"sv).string(), manifest_json("com.example.broken"sv, "/bin/cat"sv));
    EXPECT(WebExtensions::find_native_host("com.example.broken"sv, { directory, fallback }).is_error());

    // The name inside the manifest must match the name asked for.
    write_file(LexicalPath::join(directory, "com.example.mismatch.json"sv).string(), manifest_json("com.example.other"sv, "/bin/cat"sv));
    EXPECT(WebExtensions::find_native_host("com.example.mismatch"sv, { directory }).is_error());

    // A directory where the manifest should be is an error, not a crash.
    MUST(Core::System::mkdir(LexicalPath::join(directory, "com.example.dir.json"sv).string(), 0700));
    EXPECT(WebExtensions::find_native_host("com.example.dir"sv, { directory }).is_error());

    remove_test_root();
}

TEST_CASE(default_directories)
{
    auto directories = WebExtensions::default_native_host_directories("/browser/NativeMessagingHosts"sv);
    EXPECT(directories.size() >= 3u);
    EXPECT_EQ(directories.first(), "/browser/NativeMessagingHosts"sv);

    auto home = Core::StandardPaths::home_directory();
#if defined(AK_OS_MACOS)
    EXPECT_EQ(directories[1], ByteString::formatted("{}/Library/Application Support/Google/Chrome/NativeMessagingHosts", home));
    EXPECT_EQ(directories[2], "/Library/Google/Chrome/NativeMessagingHosts"sv);
#elif defined(AK_OS_LINUX)
    EXPECT_EQ(directories[1], ByteString::formatted("{}/.config/google-chrome/NativeMessagingHosts", home));
    EXPECT_EQ(directories[2], "/etc/opt/chrome/native-messaging-hosts"sv);
#endif

    auto without_browser = WebExtensions::default_native_host_directories();
    EXPECT_EQ(without_browser.size(), directories.size() - 1);
    EXPECT_EQ(without_browser.first(), directories[1]);
}

TEST_CASE(launch_arguments)
{
    auto manifest = TRY_OR_FAIL(WebExtensions::NativeHostManifest::parse(manifest_json("com.example.host"sv, "/opt/host/bin"sv), "/tmp"sv));
    auto arguments = WebExtensions::native_host_arguments(manifest, extension_id);
#if defined(AK_OS_WINDOWS)
    EXPECT_EQ(arguments.size(), 3u);
    EXPECT_EQ(arguments[2], "--parent-window=0"sv);
#else
    EXPECT_EQ(arguments.size(), 2u);
#endif
    EXPECT_EQ(arguments[0], "/opt/host/bin"sv);
    EXPECT_EQ(arguments[1], "chrome-extension://aeblfdkhhhdcdjpifhhbdiojplfjncoa/"sv);
}

static ByteBuffer frame(StringView body)
{
    u32 length = body.length();
    auto buffer = MUST(ByteBuffer::create_uninitialized(sizeof(length) + body.length()));
    memcpy(buffer.data(), &length, sizeof(length));
    memcpy(buffer.data() + sizeof(length), body.characters_without_null_termination(), body.length());
    return buffer;
}

static void append_frame(ByteBuffer& stream, StringView body)
{
    auto encoded = frame(body);
    stream.append(encoded.bytes());
}

static ErrorOr<Vector<JsonValue>> feed(WebExtensions::NativeMessageDecoder& decoder, ByteBuffer const& bytes)
{
    return decoder.feed(bytes.bytes());
}

TEST_CASE(encode_message)
{
    JsonObject object;
    object.set("text"sv, "héllo"sv);
    auto encoded = TRY_OR_FAIL(WebExtensions::encode_native_message(object));
    auto body = JsonValue(object).serialized();
    EXPECT_EQ(encoded.size(), 4 + body.bytes().size());
    u32 length = 0;
    memcpy(&length, encoded.data(), 4);
    EXPECT_EQ(length, body.bytes().size());
    EXPECT_EQ(StringView(encoded.bytes().slice(4)), body.bytes_as_string_view());
    EXPECT_EQ(encoded, frame(body));
}

TEST_CASE(encode_rejects_oversized_message)
{
    auto big = MUST(String::repeated('a', WebExtensions::max_message_size_to_native_host));
    EXPECT(WebExtensions::encode_native_message(JsonValue(big)).is_error());

    // Two quotes plus the string fit exactly.
    auto fits = MUST(String::repeated('a', WebExtensions::max_message_size_to_native_host - 2));
    auto encoded = TRY_OR_FAIL(WebExtensions::encode_native_message(JsonValue(fits)));
    EXPECT_EQ(encoded.size(), WebExtensions::max_message_size_to_native_host + 4);
}

TEST_CASE(decode_messages)
{
    WebExtensions::NativeMessageDecoder decoder;
    auto messages = TRY_OR_FAIL(feed(decoder, frame(R"({"a":1})"sv)));
    EXPECT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].as_object().get_i64("a"sv), 1);
    EXPECT(!decoder.has_partial_message());

    auto empty = TRY_OR_FAIL(decoder.feed({}));
    EXPECT(empty.is_empty());
}

TEST_CASE(decode_split_and_concatenated_frames)
{
    ByteBuffer stream;
    append_frame(stream, R"({"n":1})"sv);
    append_frame(stream, R"("two")"sv);
    append_frame(stream, "[3]"sv);
    append_frame(stream, "null"sv);

    // All at once.
    {
        WebExtensions::NativeMessageDecoder decoder;
        auto messages = TRY_OR_FAIL(decoder.feed(stream));
        EXPECT_EQ(messages.size(), 4u);
        EXPECT_EQ(messages[0].as_object().get_i64("n"sv), 1);
        EXPECT_EQ(messages[1].as_string(), "two"sv);
        EXPECT_EQ(messages[2].as_array().size(), 1u);
        EXPECT(messages[3].is_null());
    }

    // One byte at a time.
    {
        WebExtensions::NativeMessageDecoder decoder;
        Vector<JsonValue> messages;
        for (size_t i = 0; i < stream.size(); ++i) {
            auto some = TRY_OR_FAIL(decoder.feed(stream.bytes().slice(i, 1)));
            messages.extend(move(some));
            if (i + 1 < stream.size() && messages.size() < 4)
                EXPECT(decoder.has_partial_message() || messages.size() > 0);
        }
        EXPECT_EQ(messages.size(), 4u);
        EXPECT_EQ(messages[1].as_string(), "two"sv);
        EXPECT(!decoder.has_partial_message());
    }

    // Split inside the length header and inside the body.
    {
        WebExtensions::NativeMessageDecoder decoder;
        EXPECT(TRY_OR_FAIL(decoder.feed(stream.bytes().slice(0, 2))).is_empty());
        EXPECT(decoder.has_partial_message());
        EXPECT(TRY_OR_FAIL(decoder.feed(stream.bytes().slice(2, 6))).is_empty());
        auto rest = TRY_OR_FAIL(decoder.feed(stream.bytes().slice(8)));
        EXPECT_EQ(rest.size(), 4u);
    }
}

TEST_CASE(decode_size_limit)
{
    // A header announcing exactly the limit is fine while the body is pending.
    {
        WebExtensions::NativeMessageDecoder decoder;
        u32 length = WebExtensions::max_message_size_from_native_host;
        EXPECT(TRY_OR_FAIL(decoder.feed({ &length, sizeof(length) })).is_empty());
    }
    {
        WebExtensions::NativeMessageDecoder decoder;
        u32 length = WebExtensions::max_message_size_from_native_host + 1;
        EXPECT(decoder.feed({ &length, sizeof(length) }).is_error());
        EXPECT(decoder.has_failed());
    }
    {
        WebExtensions::NativeMessageDecoder decoder;
        u32 length = 0xffffffff;
        EXPECT(decoder.feed({ &length, sizeof(length) }).is_error());
    }
    // A custom limit.
    {
        WebExtensions::NativeMessageDecoder decoder(4);
        EXPECT(feed(decoder, frame("12345"sv)).is_error());
    }
    // A full message at the limit decodes.
    {
        WebExtensions::NativeMessageDecoder decoder;
        auto body = ByteString::formatted("\"{}\"", ByteString::repeated('x', WebExtensions::max_message_size_from_native_host - 2));
        auto messages = TRY_OR_FAIL(feed(decoder, frame(body)));
        EXPECT_EQ(messages.size(), 1u);
        EXPECT_EQ(messages[0].as_string().bytes().size(), WebExtensions::max_message_size_from_native_host - 2);
    }
}

TEST_CASE(decode_rejects_bad_payloads)
{
    auto fails = [](ByteBuffer const& bytes) {
        WebExtensions::NativeMessageDecoder decoder;
        return decoder.feed(bytes.bytes()).is_error();
    };

    EXPECT(fails(frame(""sv)));
    EXPECT(fails(frame("{"sv)));
    EXPECT(fails(frame("{}x"sv)));
    EXPECT(fails(frame("not json"sv)));
    EXPECT(fails(frame("\"\xff\xfe\""sv)));
    EXPECT(fails(frame("\"\xc3\""sv)));
    EXPECT(fails(frame(ByteString::formatted("{}{}", ByteString::repeated('[', 1000), ByteString::repeated(']', 1000)))));

    // Once failed, the decoder stays failed, and messages decoded in the same feed are not returned.
    WebExtensions::NativeMessageDecoder decoder;
    ByteBuffer stream;
    append_frame(stream, "1"sv);
    append_frame(stream, "{"sv);
    EXPECT(decoder.feed(stream).is_error());
    EXPECT(decoder.has_failed());
    EXPECT(feed(decoder, frame("2"sv)).is_error());
}

static ByteString write_host_script(ByteString const& directory, StringView name, StringView body)
{
    auto path = LexicalPath::join(directory, name).string();
    write_file(path, ByteString::formatted("#!/bin/sh\n{}\n", body), 0700);
    return path;
}

static WebExtensions::NativeHostManifest host_manifest(StringView path)
{
    return MUST(WebExtensions::NativeHostManifest::parse(manifest_json("com.example.host"sv, path), "/tmp"sv));
}

static Vector<JsonValue> receive_messages(WebExtensions::NativeHostConnection& connection, size_t count)
{
    Vector<JsonValue> messages;
    while (messages.size() < count && !connection.is_closed()) {
        auto some = MUST(connection.receive());
        messages.extend(move(some));
    }
    return messages;
}

TEST_CASE(connection_round_trip)
{
    remove_test_root();
    auto directory = make_directory("hosts"sv);
    auto origin_path = LexicalPath::join(directory, "origin"sv).string();
    auto script = write_host_script(directory, "echo-host"sv, ByteString::formatted("printf '%s' \"$#:$1\" > '{}'\nexec cat", origin_path));

    auto connection = TRY_OR_FAIL(WebExtensions::NativeHostConnection::launch(host_manifest(script), extension_id));
    EXPECT(connection->pid() > 0);
    EXPECT(connection->stdout_fd() >= 0);

    JsonObject first;
    first.set("action"sv, "hello"sv);
    TRY_OR_FAIL(connection->send(first));
    TRY_OR_FAIL(connection->send(JsonValue(42)));

    auto messages = receive_messages(*connection, 2);
    EXPECT_EQ(messages.size(), 2u);
    if (messages.size() == 2) {
        EXPECT_EQ(messages[0].as_object().get_string("action"sv), "hello"sv);
        EXPECT_EQ(messages[1].get_i64(), 42);
    }

    connection->close();
    EXPECT(connection->is_closed());
    EXPECT(connection->send(first).is_error());
    EXPECT(connection->receive().is_error());

    auto origin = MUST(MUST(Core::File::open(origin_path, Core::File::OpenMode::Read))->read_until_eof());
#if defined(AK_OS_WINDOWS)
    EXPECT_EQ(StringView(origin), "2:chrome-extension://aeblfdkhhhdcdjpifhhbdiojplfjncoa/"sv);
#else
    EXPECT_EQ(StringView(origin), "1:chrome-extension://aeblfdkhhhdcdjpifhhbdiojplfjncoa/"sv);
#endif

    remove_test_root();
}

TEST_CASE(connection_to_exited_host)
{
    remove_test_root();
    auto directory = make_directory("hosts"sv);
    auto script = write_host_script(directory, "exit-host"sv, "exit 0"sv);

    auto connection = TRY_OR_FAIL(WebExtensions::NativeHostConnection::launch(host_manifest(script), extension_id));
    auto messages = receive_messages(*connection, 1);
    EXPECT(messages.is_empty());
    EXPECT(connection->is_closed());

    // Writing to a host that is gone fails instead of raising SIGPIPE.
    EXPECT(connection->send(JsonValue("hi"sv)).is_error());
    connection->close();
    remove_test_root();
}

TEST_CASE(connection_survives_writing_to_dead_host)
{
    remove_test_root();
    auto directory = make_directory("hosts"sv);
    auto script = write_host_script(directory, "exit-host"sv, "exit 0"sv);

    auto connection = TRY_OR_FAIL(WebExtensions::NativeHostConnection::launch(host_manifest(script), extension_id));
    // Keep writing until the host is gone; none of this may kill the test process.
    bool failed = false;
    for (int i = 0; i < 10000 && !failed; ++i)
        failed = connection->send(JsonValue("payload"sv)).is_error();
    EXPECT(failed);
    connection->close();
    remove_test_root();
}

TEST_CASE(connection_rejects_garbage_output)
{
    remove_test_root();
    auto directory = make_directory("hosts"sv);
    auto script = write_host_script(directory, "garbage-host"sv, "printf 'xxxxxxxxxxxx'\nexec cat > /dev/null"sv);

    auto connection = TRY_OR_FAIL(WebExtensions::NativeHostConnection::launch(host_manifest(script), extension_id));
    bool failed = false;
    while (!failed && !connection->is_closed())
        failed = connection->receive().is_error();
    EXPECT(failed);
    EXPECT(connection->is_closed());
    connection->close();
    remove_test_root();
}

TEST_CASE(connection_to_missing_executable)
{
    auto manifest = host_manifest("/nonexistent/native/host"sv);
    auto connection = WebExtensions::NativeHostConnection::launch(manifest, extension_id);
    // Spawning may fail outright, or the child may exit at once; either way nothing crashes.
    if (!connection.is_error()) {
        auto messages = receive_messages(*connection.value(), 1);
        EXPECT(messages.is_empty());
        connection.value()->close();
    }
}

TEST_CASE(connection_rejects_invalid_extension_id)
{
    auto manifest = host_manifest("/bin/cat"sv);
    EXPECT(WebExtensions::NativeHostConnection::launch(manifest, "not-an-id"sv).is_error());
    EXPECT(WebExtensions::NativeHostConnection::launch(manifest, other_extension_id).is_error());
}
