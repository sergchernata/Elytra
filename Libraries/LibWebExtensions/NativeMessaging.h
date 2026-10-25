/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibCore/Process.h>

namespace WebExtensions {

// Chrome's limits: messages from the host are capped at 1 MiB, messages to the host at 64 MiB.
static constexpr size_t max_message_size_from_native_host = 1 * MiB;
static constexpr size_t max_message_size_to_native_host = 64 * MiB;

// A native messaging host manifest, as Chrome reads it from a NativeMessagingHosts directory.
struct NativeHostManifest {
    ByteString name;
    String description;
    ByteString path;
    Vector<String> allowed_origins;

    // Validates the manifest the way Chrome does. On Windows only, a relative path resolves against manifest_directory.
    static ErrorOr<NativeHostManifest> parse(StringView json, StringView manifest_directory);

    static bool is_valid_name(StringView);

    bool allows_extension(StringView extension_id) const;
};

// Directories searched for <host_name>.json, in order. The browser's own per-user directory comes first, then
// Google Chrome's per-user and system-wide directories, where hosts such as 1Password install their manifests.
Vector<ByteString> default_native_host_directories(Optional<ByteString> browser_host_directory = {});

// Returns the first valid manifest found. A missing file moves on to the next directory; an invalid one is an error.
ErrorOr<Optional<NativeHostManifest>> find_native_host(StringView host_name, Vector<ByteString> const& search_directories);

// The argv Chrome launches a host with, starting with the executable path.
Vector<ByteString> native_host_arguments(NativeHostManifest const&, StringView extension_id);

// One frame: a u32 length in native byte order, then the UTF-8 JSON.
ErrorOr<ByteBuffer> encode_native_message(JsonValue const&);

// Splits a host's output stream into messages. After an error the stream is unusable and the connection must close.
class NativeMessageDecoder {
public:
    explicit NativeMessageDecoder(size_t max_message_size = max_message_size_from_native_host)
        : m_max_message_size(max_message_size)
    {
    }

    ErrorOr<Vector<JsonValue>> feed(ReadonlyBytes);

    bool has_partial_message() const { return !m_buffer.is_empty(); }
    bool has_failed() const { return m_failed; }

private:
    ByteBuffer m_buffer;
    size_t m_max_message_size { 0 };
    bool m_failed { false };
};

// A host process driven by the event loop, as the browser needs: its output is read as it comes, writes never block
// (what the host isn't reading yet waits in memory), and an ended host is reaped without waiting for it. Like the
// blocking connection below, it starts in the host's directory with Chrome's arguments, and its stderr is inherited.
class NativeHostProcess final : public RefCounted<NativeHostProcess> {
public:
    enum class Ending : u8 {
        Exited,    // The host closed its output, or went away.
        BadOutput, // A frame was too large or not JSON.
    };

    static ErrorOr<NonnullRefPtr<NativeHostProcess>> launch(NativeHostManifest const&, StringView extension_id);
    ~NativeHostProcess();

    // What the host sends, in order. It may close() the host, or drop it.
    Function<void(JsonValue)> on_message;
    // The host ended on its own (never after close()). It may drop the host.
    Function<void(Ending)> on_end;

    // Queues a message. Fails if it's too large, if the host is closed, or if too much is waiting for the host.
    ErrorOr<void> send(JsonValue const&);

    // Closes the pipes, which tells the host to quit, and reaps it in the background, killing it if it hasn't exited
    // after a grace period.
    void close();

    bool is_closed() const { return m_closed; }
    pid_t pid() const { return m_pid; }
    size_t queued_bytes() const { return m_outgoing.size() - m_outgoing_offset; }

    // At shutdown: closes every host and waits at most `grace` for them to exit, then kills the rest.
    static void end_all(AK::Duration grace);
    // Hosts closed but not yet reaped.
    static size_t ending_count();

private:
    NativeHostProcess(pid_t, int stdin_fd, int stdout_fd);

    void read_output();
    void write_queued();
    void end(Ending);

    pid_t m_pid { -1 };
    int m_stdin_fd { -1 };
    int m_stdout_fd { -1 };
    RefPtr<Core::Notifier> m_stdout_notifier;
    RefPtr<Core::Notifier> m_stdin_notifier;
    NativeMessageDecoder m_decoder;
    ByteBuffer m_outgoing;
    size_t m_outgoing_offset { 0 };
    bool m_closed { false };
};

// A running host process, talking over its stdin and stdout. Its stderr is inherited, as in Chrome.
class NativeHostConnection {
    AK_MAKE_NONCOPYABLE(NativeHostConnection);
    AK_MAKE_NONMOVABLE(NativeHostConnection);
    AK_ALLOC_WITH_KMALLOC;

public:
    static ErrorOr<NonnullOwnPtr<NativeHostConnection>> launch(NativeHostManifest const&, StringView extension_id);
    ~NativeHostConnection();

    ErrorOr<void> send(JsonValue const&);

    // Blocks until the host writes something. Returns no messages once the host has closed its stdout.
    ErrorOr<Vector<JsonValue>> receive();

    // For the caller's own event loop notifier.
    int stdout_fd() const { return m_stdout_fd; }
    pid_t pid() const { return m_pid; }
    bool is_closed() const { return m_closed; }

    // Closes the pipes and ends the process, forcefully if it has not exited after a short grace period.
    void close();

private:
    NativeHostConnection(pid_t, int stdin_fd, int stdout_fd);

    pid_t m_pid { -1 };
    int m_stdin_fd { -1 };
    int m_stdout_fd { -1 };
    NativeMessageDecoder m_decoder;
    bool m_closed { false };
    bool m_reaped { false };
};

}
