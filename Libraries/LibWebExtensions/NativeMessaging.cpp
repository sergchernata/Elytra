/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibWebExtensions/NativeMessaging.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

namespace WebExtensions {

static constexpr auto extension_origin_prefix = "chrome-extension://"sv;

// No real manifest comes close; this only bounds what a hostile file can make us read.
static constexpr size_t max_manifest_size = 1 * MiB;

// How long a host gets to exit on its own once its pipes are closed, as in Chrome.
static constexpr u32 host_exit_grace_period_ms = 2000;

static bool is_valid_extension_id(StringView id)
{
    if (id.length() != 32)
        return false;
    for (auto c : id) {
        if (c < 'a' || c > 'p')
            return false;
    }
    return true;
}

static ByteString origin_for_extension(StringView extension_id)
{
    return ByteString::formatted("{}{}/", extension_origin_prefix, extension_id);
}

// Only exact extension origins are accepted; Chrome rejects wildcards.
static bool is_valid_allowed_origin(StringView origin)
{
    if (origin.length() != extension_origin_prefix.length() + 32 + 1)
        return false;
    if (!origin.starts_with(extension_origin_prefix) || !origin.ends_with('/'))
        return false;
    auto id = origin.substring_view(extension_origin_prefix.length(), 32);
    return is_valid_extension_id(id);
}

bool NativeHostManifest::is_valid_name(StringView name)
{
    if (name.is_empty())
        return false;
    for (size_t i = 0; i < name.length(); ++i) {
        auto c = name[i];
        if (!is_ascii_lower_alpha(c) && !is_ascii_digit(c) && c != '.' && c != '_')
            return false;
        if (c == '.' && (i == 0 || name[i - 1] == '.' || i == name.length() - 1))
            return false;
    }
    return true;
}

ErrorOr<NativeHostManifest> NativeHostManifest::parse(StringView json, StringView manifest_directory)
{
    auto value = TRY(JsonParser::parse(json));
    if (!value.is_object())
        return Error::from_string_literal("Native messaging host manifest is not a JSON object");
    auto const& object = value.as_object();

    NativeHostManifest manifest;

    auto name = object.get_string("name"sv);
    if (!name.has_value() || !is_valid_name(*name))
        return Error::from_string_literal("Invalid value for name");
    manifest.name = name->to_byte_string();

    auto description = object.get_string("description"sv);
    if (!description.has_value())
        return Error::from_string_literal("Invalid value for description");
    manifest.description = *description;

    auto type = object.get_string("type"sv);
    if (!type.has_value() || *type != "stdio"sv)
        return Error::from_string_literal("Invalid value for type");

    auto path = object.get_string("path"sv);
    if (!path.has_value() || path->is_empty() || path->contains('\0'))
        return Error::from_string_literal("Invalid value for path");
    manifest.path = path->to_byte_string();
#if defined(AK_OS_WINDOWS)
    // Only Windows allows a path relative to the manifest's directory.
    if (!LexicalPath(manifest.path).is_absolute())
        manifest.path = LexicalPath::join(manifest_directory, manifest.path).string();
#else
    (void)manifest_directory;
    if (!manifest.path.starts_with('/'))
        return Error::from_string_literal("Native messaging host path must be absolute");
#endif

    auto origins = object.get_array("allowed_origins"sv);
    if (!origins.has_value())
        return Error::from_string_literal("Invalid value for allowed_origins");
    for (auto const& origin : origins->values()) {
        if (!origin.is_string() || !is_valid_allowed_origin(origin.as_string()))
            return Error::from_string_literal("Invalid value for allowed_origins");
        manifest.allowed_origins.append(origin.as_string());
    }

    return manifest;
}

bool NativeHostManifest::allows_extension(StringView extension_id) const
{
    if (!is_valid_extension_id(extension_id))
        return false;
    auto origin = origin_for_extension(extension_id);
    return any_of(allowed_origins, [&](auto const& allowed) { return allowed == origin.view(); });
}

Vector<ByteString> default_native_host_directories(Optional<ByteString> browser_host_directory)
{
    Vector<ByteString> directories;
    if (browser_host_directory.has_value())
        directories.append(browser_host_directory.release_value());

    [[maybe_unused]] auto home = Core::StandardPaths::home_directory();
#if defined(AK_OS_MACOS)
    directories.append(ByteString::formatted("{}/Library/Application Support/Google/Chrome/NativeMessagingHosts", home));
    directories.append("/Library/Google/Chrome/NativeMessagingHosts"sv);
    directories.append(ByteString::formatted("{}/Library/Application Support/Chromium/NativeMessagingHosts", home));
    directories.append("/Library/Application Support/Chromium/NativeMessagingHosts"sv);
#elif !defined(AK_OS_WINDOWS)
    directories.append(ByteString::formatted("{}/.config/google-chrome/NativeMessagingHosts", home));
    directories.append("/etc/opt/chrome/native-messaging-hosts"sv);
    directories.append(ByteString::formatted("{}/.config/chromium/NativeMessagingHosts", home));
    directories.append("/etc/chromium/native-messaging-hosts"sv);
#endif
    return directories;
}

ErrorOr<Optional<NativeHostManifest>> find_native_host(StringView host_name, Vector<ByteString> const& search_directories)
{
    // The name check also keeps the lookup inside the search directories.
    if (!NativeHostManifest::is_valid_name(host_name))
        return Error::from_string_literal("Invalid native messaging host name");

    for (auto const& directory : search_directories) {
        auto path = LexicalPath::join(directory, ByteString::formatted("{}.json", host_name)).string();

        auto file_or_error = Core::File::open(path, Core::File::OpenMode::Read);
        if (file_or_error.is_error()) {
            auto const& error = file_or_error.error();
            if (error.is_errno() && (error.code() == ENOENT || error.code() == ENOTDIR))
                continue;
            return file_or_error.release_error();
        }

        // As in Chrome, the first manifest that exists is the one used, valid or not.
        auto file = file_or_error.release_value();
        auto size = TRY(file->size());
        if (size > max_manifest_size)
            return Error::from_string_literal("Native messaging host manifest is too large");
        auto contents = TRY(file->read_until_eof());
        if (contents.size() > max_manifest_size)
            return Error::from_string_literal("Native messaging host manifest is too large");

        auto manifest = TRY(NativeHostManifest::parse(contents, directory));
        if (manifest.name != host_name)
            return Error::from_string_literal("Native messaging host manifest name does not match");
        return manifest;
    }
    return OptionalNone {};
}

Vector<ByteString> native_host_arguments(NativeHostManifest const& manifest, StringView extension_id)
{
    Vector<ByteString> arguments;
    arguments.append(manifest.path);
    arguments.append(origin_for_extension(extension_id));
#if defined(AK_OS_WINDOWS)
    // We have no window handle to give; Chrome passes 0 for service workers too.
    arguments.append("--parent-window=0"sv);
#endif
    return arguments;
}

ErrorOr<ByteBuffer> encode_native_message(JsonValue const& message)
{
    StringBuilder builder;
    message.serialize(builder);
    auto length = builder.length();
    if (length > max_message_size_to_native_host)
        return Error::from_string_literal("Native message is too large");

    auto header = static_cast<u32>(length);
    auto buffer = TRY(ByteBuffer::create_uninitialized(sizeof(header) + length));
    memcpy(buffer.data(), &header, sizeof(header));
    builder.string_view().bytes().copy_to(buffer.bytes().slice(sizeof(header)));
    return buffer;
}

ErrorOr<Vector<JsonValue>> NativeMessageDecoder::feed(ReadonlyBytes bytes)
{
    if (m_failed)
        return Error::from_string_literal("Native message stream already failed");

    ArmedScopeGuard mark_failed = [&] {
        m_failed = true;
        m_buffer.clear();
    };

    TRY(m_buffer.try_append(bytes));

    Vector<JsonValue> messages;
    size_t offset = 0;
    while (m_buffer.size() - offset >= sizeof(u32)) {
        u32 length = 0;
        memcpy(&length, m_buffer.data() + offset, sizeof(length));
        if (length > m_max_message_size)
            return Error::from_string_literal("Native message is too large");
        if (m_buffer.size() - offset - sizeof(length) < length)
            break;

        auto body = StringView { m_buffer.bytes().slice(offset + sizeof(length), length) };
        TRY(messages.try_append(TRY(JsonParser::parse(body))));
        offset += sizeof(length) + length;
    }

    if (offset == m_buffer.size())
        m_buffer.clear();
    else if (offset > 0)
        m_buffer = TRY(m_buffer.slice(offset, m_buffer.size() - offset));

    mark_failed.disarm();
    return messages;
}

ErrorOr<NonnullOwnPtr<NativeHostConnection>> NativeHostConnection::launch(NativeHostManifest const& manifest, StringView extension_id)
{
    if (!manifest.allows_extension(extension_id))
        return Error::from_string_literal("Access to the native messaging host is forbidden");

    auto stdin_fds = TRY(Core::System::pipe2(O_CLOEXEC, Core::System::PipeReadEnd::ReadExternally));
    ArmedScopeGuard close_stdin = [&] {
        (void)Core::System::close(stdin_fds[0]);
        (void)Core::System::close(stdin_fds[1]);
    };
    auto stdout_fds = TRY(Core::System::pipe2(O_CLOEXEC));
    ArmedScopeGuard close_stdout = [&] {
        (void)Core::System::close(stdout_fds[0]);
        (void)Core::System::close(stdout_fds[1]);
    };

#if defined(F_SETNOSIGPIPE)
    // A host that has gone away must make writes fail rather than kill us with SIGPIPE.
    if (::fcntl(stdin_fds[1], F_SETNOSIGPIPE, 1) < 0)
        return Error::from_syscall("fcntl(F_SETNOSIGPIPE)"sv, errno);
#endif

    auto arguments = native_host_arguments(manifest, extension_id);
    Core::ProcessSpawnOptions options {
        .executable = manifest.path,
        .arguments = arguments.span().slice(1),
        .file_actions = {
            Core::FileAction::DupFd { stdin_fds[0], STDIN_FILENO },
            Core::FileAction::DupFd { stdout_fds[1], STDOUT_FILENO },
        },
    };
    auto process = TRY(Core::Process::spawn(options));

    close_stdin.disarm();
    close_stdout.disarm();
    (void)Core::System::close(stdin_fds[0]);
    (void)Core::System::close(stdout_fds[1]);

    return adopt_nonnull_own_or_enomem(new (nothrow) NativeHostConnection(move(process), stdin_fds[1], stdout_fds[0]));
}

NativeHostConnection::NativeHostConnection(Core::Process process, int stdin_fd, int stdout_fd)
    : m_process(move(process))
    , m_stdin_fd(stdin_fd)
    , m_stdout_fd(stdout_fd)
{
}

NativeHostConnection::~NativeHostConnection()
{
    close();
}

ErrorOr<void> NativeHostConnection::send(JsonValue const& message)
{
    if (m_closed)
        return Error::from_string_literal("Native messaging host connection is closed");

    auto frame = TRY(encode_native_message(message));
    auto remaining = frame.bytes();
    while (!remaining.is_empty()) {
        auto written_or_error = Core::System::write(m_stdin_fd, remaining);
        if (written_or_error.is_error()) {
            if (written_or_error.error().is_errno() && written_or_error.error().code() == EINTR)
                continue;
            close();
            return written_or_error.release_error();
        }
        remaining = remaining.slice(written_or_error.value());
    }
    return {};
}

ErrorOr<Vector<JsonValue>> NativeHostConnection::receive()
{
    if (m_closed)
        return Error::from_string_literal("Native messaging host connection is closed");

    u8 buffer[64 * KiB];
    while (true) {
        auto read_or_error = Core::System::read(m_stdout_fd, { buffer, sizeof(buffer) });
        if (read_or_error.is_error()) {
            if (read_or_error.error().is_errno() && read_or_error.error().code() == EINTR)
                continue;
            close();
            return read_or_error.release_error();
        }

        auto count = read_or_error.value();
        if (count == 0) {
            // The host closed its end. A partial frame left over is simply dropped.
            close();
            return Vector<JsonValue> {};
        }

        auto messages_or_error = m_decoder.feed({ buffer, count });
        if (messages_or_error.is_error()) {
            close();
            return messages_or_error.release_error();
        }
        return messages_or_error.release_value();
    }
}

void NativeHostConnection::close()
{
    if (!m_closed) {
        m_closed = true;
        (void)Core::System::close(m_stdin_fd);
        (void)Core::System::close(m_stdout_fd);
        m_stdin_fd = -1;
        m_stdout_fd = -1;
    }

    if (m_reaped || m_process.pid() <= 0)
        return;

    // Closing stdin tells the host to quit. Give it a moment, then make sure it is gone.
    for (u32 waited_ms = 0; waited_ms < host_exit_grace_period_ms; waited_ms += 10) {
        auto result = Core::System::waitpid(m_process.pid(), WNOHANG);
        if (result.is_error() || result.value().pid == m_process.pid()) {
            m_reaped = true;
            return;
        }
        (void)Core::System::sleep_ms(10);
    }

    (void)Core::Process::terminate_process(m_process.pid(), Core::Process::TerminationMode::Forceful);
    (void)Core::System::waitpid(m_process.pid());
    m_reaped = true;
}

}
