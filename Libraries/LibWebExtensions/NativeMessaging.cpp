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

#include <AK/HashTable.h>
#include <AK/Time.h>
#include <LibCore/Notifier.h>
#include <LibCore/Timer.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(AK_OS_MACOS)
#    include <crt_externs.h>
#    include <dlfcn.h>
#    include <spawn.h>
#elif defined(AK_OS_LINUX)
#    include <sys/syscall.h>
#endif

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

#if !defined(AK_OS_WINDOWS)
// Starts the host as Chrome does: in its own directory, with only the given pipes as its stdin and stdout (and our
// stderr), default signal handling, and on macOS responsible for itself rather than for us.
static ErrorOr<pid_t> spawn_host(NativeHostManifest const& manifest, StringView extension_id, int stdin_fd, int stdout_fd)
{
    // Spawning a path that isn't there may only fail in the child; check first, so the caller can tell.
    if (::access(manifest.path.characters(), X_OK) < 0)
        return Error::from_syscall("access"sv, errno);

    auto arguments = native_host_arguments(manifest, extension_id);
    Vector<char*> argv;
    for (auto& argument : arguments)
        argv.append(const_cast<char*>(argument.characters()));
    argv.append(nullptr);
    auto directory = ByteString { LexicalPath(manifest.path).dirname() };

    sigset_t no_signals;
    sigemptyset(&no_signals);

#    if defined(AK_OS_MACOS)
#        define CHECK(invocation)                  \
            if (int returned_errno = (invocation)) \
                return Error::from_errno(returned_errno);

    posix_spawn_file_actions_t actions;
    CHECK(posix_spawn_file_actions_init(&actions));
    ScopeGuard destroy_actions = [&] { posix_spawn_file_actions_destroy(&actions); };
    posix_spawnattr_t attributes;
    CHECK(posix_spawnattr_init(&attributes));
    ScopeGuard destroy_attributes = [&] { posix_spawnattr_destroy(&attributes); };

    // Nothing but what's named here reaches the host, and it starts with default signal handling.
    CHECK(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
    CHECK(posix_spawnattr_setsigmask(&attributes, &no_signals));
    sigset_t default_signals;
    sigfillset(&default_signals);
    CHECK(posix_spawnattr_setsigdefault(&attributes, &default_signals));

    // As in Chrome, the host is responsible for itself: macOS asks the user about its privacy permissions, not ours.
    using DisclaimFunction = int (*)(posix_spawnattr_t*, int);
    static auto disclaim = reinterpret_cast<DisclaimFunction>(dlsym(RTLD_DEFAULT, "responsibility_spawnattrs_setdisclaim"));
    if (disclaim)
        CHECK(disclaim(&attributes, 1));

    CHECK(posix_spawn_file_actions_adddup2(&actions, stdin_fd, STDIN_FILENO));
    CHECK(posix_spawn_file_actions_adddup2(&actions, stdout_fd, STDOUT_FILENO));
    if (::fcntl(STDERR_FILENO, F_GETFD) != -1)
        CHECK(posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO));
    CHECK(posix_spawn_file_actions_addchdir_np(&actions, directory.characters()));
#        undef CHECK

    pid_t pid = -1;
    if (int error = ::posix_spawn(&pid, manifest.path.characters(), &actions, &attributes, argv.data(), *_NSGetEnviron()))
        return Error::from_errno(error);
    return pid;
#    else
    auto pid = TRY(Core::System::fork());
    if (pid == 0) {
        // Only async-signal-safe calls from here on.
        for (int signal_number = 1; signal_number < NSIG; ++signal_number)
            ::signal(signal_number, SIG_DFL);
        ::sigprocmask(SIG_SETMASK, &no_signals, nullptr);
        if (::dup2(stdin_fd, STDIN_FILENO) < 0 || ::dup2(stdout_fd, STDOUT_FILENO) < 0 || ::chdir(directory.characters()) < 0)
            ::_exit(127);
#        if defined(SYS_close_range)
        ::syscall(SYS_close_range, STDERR_FILENO + 1, ~0U, 0);
#        endif
        ::execv(manifest.path.characters(), argv.data());
        ::_exit(127);
    }
    return pid;
#    endif
}
#endif

struct HostPipes {
    int stdin_write { -1 };
    int stdout_read { -1 };
    pid_t pid { -1 };
};

static ErrorOr<HostPipes> launch_host(NativeHostManifest const& manifest, StringView extension_id)
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

#if defined(AK_OS_WINDOWS)
    auto arguments = native_host_arguments(manifest, extension_id);
    Core::ProcessSpawnOptions options {
        .executable = manifest.path,
        .arguments = arguments.span().slice(1),
        .file_actions = {
            Core::FileAction::DupFd { stdin_fds[0], STDIN_FILENO },
            Core::FileAction::DupFd { stdout_fds[1], STDOUT_FILENO },
        },
    };
    auto pid = TRY(Core::Process::spawn(options)).pid();
#else
    auto pid = TRY(spawn_host(manifest, extension_id, stdin_fds[0], stdout_fds[1]));
#endif

    close_stdin.disarm();
    close_stdout.disarm();
    (void)Core::System::close(stdin_fds[0]);
    (void)Core::System::close(stdout_fds[1]);
    return HostPipes { .stdin_write = stdin_fds[1], .stdout_read = stdout_fds[0], .pid = pid };
}

// Kills the host if it's still our child and not yet reaped (so its pid can't be anyone else's yet).
static bool kill_if_still_running(pid_t pid)
{
    auto result = Core::System::waitpid(pid, WNOHANG);
    if (result.is_error() || result.value().pid == pid)
        return false;
    (void)Core::Process::terminate_process(pid, Core::Process::TerminationMode::Forceful);
    return true;
}

ErrorOr<NonnullOwnPtr<NativeHostConnection>> NativeHostConnection::launch(NativeHostManifest const& manifest, StringView extension_id)
{
    auto host = TRY(launch_host(manifest, extension_id));
    return adopt_nonnull_own_or_enomem(new (nothrow) NativeHostConnection(host.pid, host.stdin_write, host.stdout_read));
}

NativeHostConnection::NativeHostConnection(pid_t pid, int stdin_fd, int stdout_fd)
    : m_pid(pid)
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

    if (m_reaped || m_pid <= 0)
        return;

    // Closing stdin tells the host to quit. Give it a moment, then make sure it is gone.
    for (u32 waited_ms = 0; waited_ms < host_exit_grace_period_ms; waited_ms += 10) {
        auto result = Core::System::waitpid(m_pid, WNOHANG);
        if (result.is_error() || result.value().pid == m_pid) {
            m_reaped = true;
            return;
        }
        (void)Core::System::sleep_ms(10);
    }

    if (kill_if_still_running(m_pid))
        (void)Core::System::waitpid(m_pid);
    m_reaped = true;
}

// Hosts that were closed and haven't exited yet: polled until they have, killed once their grace period is over.
namespace {

struct EndingHost {
    pid_t pid { -1 };
    MonotonicTime deadline;
    bool killed { false };
};

struct HostReaper {
    Vector<EndingHost> hosts;
    RefPtr<Core::Timer> timer;
    HashTable<NativeHostProcess*> running;

    // Returns whether the host is gone.
    static bool poll(EndingHost& host)
    {
        auto result = Core::System::waitpid(host.pid, WNOHANG);
        // An error means someone else (such as the browser's SIGCHLD handler) has reaped it.
        if (result.is_error() || result.value().pid == host.pid)
            return true;
        if (!host.killed && MonotonicTime::now() >= host.deadline)
            host.killed = kill_if_still_running(host.pid);
        return false;
    }

    void poll_all()
    {
        hosts.remove_all_matching([](auto& host) { return poll(host); });
        if (hosts.is_empty() && timer)
            timer->stop();
    }

    void add(pid_t pid)
    {
        EndingHost host { .pid = pid, .deadline = MonotonicTime::now() + AK::Duration::from_milliseconds(host_exit_grace_period_ms), .killed = false };
        if (poll(host))
            return;
        hosts.append(host);
        if (!timer)
            timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(50), [this] { poll_all(); });
        if (!timer->is_active())
            timer->start();
    }
};

// Never destroyed: hosts may still be ending as the application exits.
HostReaper& reaper()
{
    static auto* reaper = new HostReaper;
    return *reaper;
}

}

// Chrome doesn't limit this; it only keeps a host that reads nothing from making us hold on to everything.
static constexpr size_t max_queued_bytes_to_native_host = 4 * max_message_size_to_native_host;

// How much is read from a host at a time, so a chatty host can't hold up the event loop.
static constexpr size_t max_read_per_activation = 1 * MiB;

ErrorOr<NonnullRefPtr<NativeHostProcess>> NativeHostProcess::launch(NativeHostManifest const& manifest, StringView extension_id)
{
    auto host = TRY(launch_host(manifest, extension_id));
    // Only our ends are non-blocking; the host's are as it expects.
    auto make_non_blocking = [](int fd) -> ErrorOr<void> {
        auto flags = TRY(Core::System::fcntl(fd, F_GETFL));
        TRY(Core::System::fcntl(fd, F_SETFL, flags | O_NONBLOCK));
        return {};
    };
    auto result = make_non_blocking(host.stdin_write);
    if (!result.is_error())
        result = make_non_blocking(host.stdout_read);
    auto process = adopt_ref_if_nonnull(new (nothrow) NativeHostProcess(host.pid, host.stdin_write, host.stdout_read));
    if (!process) {
        (void)Core::System::close(host.stdin_write);
        (void)Core::System::close(host.stdout_read);
        reaper().add(host.pid);
        return Error::from_errno(ENOMEM);
    }
    if (result.is_error()) {
        process->close();
        return result.release_error();
    }
    return process.release_nonnull();
}

NativeHostProcess::NativeHostProcess(pid_t pid, int stdin_fd, int stdout_fd)
    : m_pid(pid)
    , m_stdin_fd(stdin_fd)
    , m_stdout_fd(stdout_fd)
{
    reaper().running.set(this);
    m_stdout_notifier = Core::Notifier::construct(m_stdout_fd, Core::Notifier::Type::Read);
    m_stdout_notifier->on_activation = [this] { read_output(); };
    m_stdin_notifier = Core::Notifier::construct(m_stdin_fd, Core::Notifier::Type::Write);
    m_stdin_notifier->set_enabled(false);
    m_stdin_notifier->on_activation = [this] { write_queued(); };
}

NativeHostProcess::~NativeHostProcess()
{
    close();
    reaper().running.remove(this);
}

ErrorOr<void> NativeHostProcess::send(JsonValue const& message)
{
    if (m_closed)
        return Error::from_string_literal("Native messaging host connection is closed");
    auto frame = TRY(encode_native_message(message));
    if (queued_bytes() + frame.size() > max_queued_bytes_to_native_host)
        return Error::from_string_literal("Native messaging host isn't reading its messages");

    if (m_outgoing_offset == m_outgoing.size()) {
        m_outgoing.clear();
        m_outgoing_offset = 0;
    }
    TRY(m_outgoing.try_append(frame.bytes()));
    write_queued();
    return {};
}

void NativeHostProcess::write_queued()
{
    while (!m_closed && m_outgoing_offset < m_outgoing.size()) {
        auto written = ::write(m_stdin_fd, m_outgoing.data() + m_outgoing_offset, m_outgoing.size() - m_outgoing_offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                m_stdin_notifier->set_enabled(true);
                return;
            }
            // The host stopped reading (it's exiting, most likely). Stop writing; its output says the rest.
            m_outgoing.clear();
            m_outgoing_offset = 0;
            m_stdin_notifier->set_enabled(false);
            return;
        }
        m_outgoing_offset += written;
    }
    if (m_stdin_notifier)
        m_stdin_notifier->set_enabled(false);
    // Don't hold on to a large buffer once it has gone out.
    if (m_outgoing_offset == m_outgoing.size() && m_outgoing.capacity() > 64 * KiB) {
        m_outgoing = {};
        m_outgoing_offset = 0;
    }
}

void NativeHostProcess::read_output()
{
    NonnullRefPtr protect = *this;
    u8 buffer[64 * KiB];
    size_t total = 0;
    while (!m_closed && total < max_read_per_activation) {
        auto count = ::read(m_stdout_fd, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            end(Ending::Exited);
            return;
        }
        if (count == 0) {
            // The host closed its output. A partial frame left over is simply dropped.
            end(Ending::Exited);
            return;
        }
        total += count;

        auto messages = m_decoder.feed({ buffer, static_cast<size_t>(count) });
        if (messages.is_error()) {
            end(Ending::BadOutput);
            return;
        }
        for (auto& message : messages.value()) {
            if (m_closed)
                return;
            if (on_message)
                on_message(move(message));
        }
    }
}

void NativeHostProcess::end(Ending ending)
{
    if (m_closed)
        return;
    close();
    if (on_end)
        on_end(ending);
}

void NativeHostProcess::close()
{
    if (m_closed)
        return;
    m_closed = true;
    if (m_stdout_notifier) {
        m_stdout_notifier->close();
        m_stdout_notifier->on_activation = nullptr;
    }
    if (m_stdin_notifier) {
        m_stdin_notifier->close();
        m_stdin_notifier->on_activation = nullptr;
    }
    (void)Core::System::close(m_stdin_fd);
    (void)Core::System::close(m_stdout_fd);
    m_stdin_fd = -1;
    m_stdout_fd = -1;
    m_outgoing = {};
    m_outgoing_offset = 0;
    if (m_pid > 0)
        reaper().add(m_pid);
}

void NativeHostProcess::end_all(AK::Duration grace)
{
    auto& reaper = WebExtensions::reaper();
    // Closing one may run code that drops others, so take them as they come.
    while (!reaper.running.is_empty()) {
        auto* process = *reaper.running.begin();
        reaper.running.remove(process);
        process->close();
    }

    auto deadline = MonotonicTime::now() + grace;
    while (!reaper.hosts.is_empty() && MonotonicTime::now() < deadline) {
        reaper.hosts.remove_all_matching([](auto& host) {
            auto result = Core::System::waitpid(host.pid, WNOHANG);
            return result.is_error() || result.value().pid == host.pid;
        });
        if (!reaper.hosts.is_empty())
            (void)Core::System::sleep_ms(10);
    }
    for (auto const& host : reaper.hosts) {
        if (kill_if_still_running(host.pid))
            (void)Core::System::waitpid(host.pid);
    }
    reaper.hosts.clear();
    if (reaper.timer)
        reaper.timer->stop();
}

size_t NativeHostProcess::ending_count()
{
    return reaper().hosts.size();
}

}
