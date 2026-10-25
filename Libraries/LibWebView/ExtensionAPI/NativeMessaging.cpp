/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <signal.h>
#include <LibWebExtensions/Manifest.h>
#include <LibWebExtensions/NativeMessaging.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionAPI/Management.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>

namespace WebView {

// Chrome's runtime.lastError messages.
static constexpr auto invalid_name_error = "Invalid native messaging host name specified."sv;
static constexpr auto not_found_error = "Specified native messaging host not found."sv;
static constexpr auto forbidden_error = "Access to the specified native messaging host is forbidden."sv;
static constexpr auto failed_to_start_error = "Failed to start native messaging host."sv;
static constexpr auto exited_error = "Native host has exited."sv;
static constexpr auto io_error = "Error when communicating with the native messaging host."sv;

// How long hosts get to exit as the browser shuts down, before they're killed.
static constexpr auto shutdown_grace_period = AK::Duration::from_milliseconds(500);

// runtime.connectNative and runtime.sendNativeMessage: a native messaging host process per port or message, talking
// to the extension over its stdin and stdout (LibWebExtensions/NativeMessaging.h has the details).
class NativeMessagingAPI final
    : public ExtensionAPI
    , public ExtensionRuntime::BrowserPortEnd {
public:
    explicit NativeMessagingAPI(ExtensionRuntime& runtime)
        : ExtensionAPI(runtime)
    {
        // runtime.connectNative(application): chrome-api.js has made the port; its end here is the host.
        runtime.register_function("runtime.connectNative"sv, [this](auto const& caller, JsonArray const& arguments, ExtensionRuntime::Reply reply) {
            reply(JsonValue {});
            if (arguments.size() < 2 || !arguments.at(1).is_string())
                return;
            connect(caller, arguments.at(0), arguments.at(1).as_string());
        });

        // runtime.sendNativeMessage(application, message): a host for one message, which ends with its first answer.
        runtime.register_function("runtime.sendNativeMessage"sv, [this](auto const& caller, JsonArray const& arguments, ExtensionRuntime::Reply reply) {
            send_one_message(caller, arguments.size() > 0 ? arguments.at(0) : JsonValue {}, arguments.size() > 1 ? arguments.at(1) : JsonValue {}, move(reply));
        });
    }

    virtual void browser_port_message(u64 port, JsonValue const& message) override
    {
        auto host_id = m_host_of_port.get(port);
        if (!host_id.has_value())
            return;
        auto host = m_hosts.find(*host_id);
        if (host == m_hosts.end())
            return;
        if (host->value.process->send(message).is_error())
            end_host(*host_id, io_error);
    }

    virtual void browser_port_disconnected(u64 port) override
    {
        auto host_id = m_host_of_port.take(port);
        if (!host_id.has_value())
            return;
        if (auto host = m_hosts.take(*host_id); host.has_value())
            close_host(*host);
    }

    virtual void context_destroyed(u64 context_id) override
    {
        // Its ports are closed by now; what's left are its one-message hosts.
        Vector<u64> ended;
        for (auto const& [id, host] : m_hosts) {
            if (host.context_id == context_id)
                ended.append(id);
        }
        for (auto id : ended) {
            auto host = m_hosts.take(id);
            if (host->port.has_value())
                m_host_of_port.remove(*host->port);
            close_host(*host);
        }
    }

    virtual void shut_down() override
    {
        for (auto& [id, host] : m_hosts)
            host.process->close();
        m_hosts.clear();
        m_host_of_port.clear();
        WebExtensions::NativeHostProcess::end_all(shutdown_grace_period);
    }

private:
    static constexpr size_t MAXIMUM_HOSTS_PER_EXTENSION = 16;
    static constexpr size_t MAXIMUM_HOSTS = 64;
    static constexpr auto too_many_error = "Too many native messaging hosts are running."sv;

    // Hosts told to quit live on for up to a couple of seconds (NativeHostProcess::close()): they count until they're
    // gone, or an extension could start hosts without end by closing them at once. And starting is limited in rate.
    static constexpr auto ending_host_lifetime = AK::Duration::from_seconds(3);
    static constexpr double MAXIMUM_LAUNCH_BURST = 64;
    static constexpr double LAUNCHES_PER_SECOND = 4;
    static constexpr auto launched_too_often_error = "Native messaging hosts are being started too often."sv;

    struct EndingHost {
        String extension_id;
        pid_t pid { -1 };
        MonotonicTime closed_at;
    };
    struct LaunchBudget {
        double launches { MAXIMUM_LAUNCH_BURST };
        MonotonicTime refilled;
    };

    struct Host {
        NonnullRefPtr<WebExtensions::NativeHostProcess> process;
        String extension_id;
        u64 context_id { 0 };
        // A port's, or else sendNativeMessage's reply.
        Optional<u64> port;
        ExtensionRuntime::Reply reply;
    };

    // Finds and starts the host for the context's extension, or says why it can't, in Chrome's words.
    ErrorOr<NonnullRefPtr<WebExtensions::NativeHostProcess>, StringView> launch(ExtensionRuntime::Context const& caller, JsonValue const& application)
    {
        // Only the extension's own pages and background, and only with the permission.
        if (caller.type == "content_script"sv)
            return forbidden_error;
        auto const* extension = runtime().registry().find(caller.extension_id);
        if (!extension || !runtime().management().has_permission(extension->id(), "nativeMessaging"sv))
            return forbidden_error;

        if (!application.is_string() || !WebExtensions::NativeHostManifest::is_valid_name(application.as_string()))
            return invalid_name_error;

        auto manifest = WebExtensions::find_native_host(application.as_string(), runtime().native_host_directories());
        if (manifest.is_error()) {
            dbgln("Extensions: Can't read the manifest of native messaging host {}: {}", application.as_string(), manifest.error());
            return not_found_error;
        }
        if (!manifest.value().has_value())
            return not_found_error;
        if (!manifest.value()->allows_extension(caller.extension_id))
            return forbidden_error;

        // Each host is a process: an extension starting them in a loop would use up the user's.
        size_t running = 0;
        for (auto const& [id, host] : m_hosts) {
            if (host.extension_id == caller.extension_id)
                ++running;
        }
        forget_ended_hosts();
        for (auto const& host : m_ending) {
            if (host.extension_id == caller.extension_id)
                ++running;
        }
        if (running >= MAXIMUM_HOSTS_PER_EXTENSION || m_hosts.size() + m_ending.size() >= MAXIMUM_HOSTS)
            return too_many_error;

        auto now = MonotonicTime::now();
        auto& budget = m_launch_budgets.ensure(caller.extension_id, [&] { return LaunchBudget { MAXIMUM_LAUNCH_BURST, now }; });
        budget.launches = min(MAXIMUM_LAUNCH_BURST, budget.launches + static_cast<double>((now - budget.refilled).to_milliseconds()) * LAUNCHES_PER_SECOND / 1000.0);
        budget.refilled = now;
        if (budget.launches < 1)
            return launched_too_often_error;
        budget.launches -= 1;

        auto process = WebExtensions::NativeHostProcess::launch(*manifest.value(), caller.extension_id);
        if (process.is_error()) {
            dbgln("Extensions: Can't start native messaging host {}: {}", application.as_string(), process.error());
            return failed_to_start_error;
        }
        return process.release_value();
    }

    void connect(ExtensionRuntime::Context const& caller, JsonValue const& application, String const& port_id)
    {
        auto port = runtime().open_browser_port(caller, port_id, *this);
        if (!port.has_value())
            return;

        auto process = launch(caller, application);
        if (process.is_error()) {
            runtime().close_browser_port(*port, MUST(String::from_utf8(process.error())));
            return;
        }

        auto id = m_next_host_id++;
        auto& host_process = *process.value();
        host_process.on_message = [this, id](JsonValue message) {
            auto host = m_hosts.find(id);
            if (host != m_hosts.end() && host->value.port.has_value())
                runtime().post_from_browser_port(*host->value.port, message);
        };
        host_process.on_end = [this, id](WebExtensions::NativeHostProcess::Ending ending) {
            end_host(id, ending == WebExtensions::NativeHostProcess::Ending::Exited ? exited_error : io_error);
        };
        m_hosts.set(id, Host { .process = process.release_value(), .extension_id = caller.extension_id, .context_id = caller.id, .port = *port, .reply = {} });
        m_host_of_port.set(*port, id);
    }

    void send_one_message(ExtensionRuntime::Context const& caller, JsonValue const& application, JsonValue const& message, ExtensionRuntime::Reply reply)
    {
        auto process = launch(caller, application);
        if (process.is_error())
            return reply(MUST(String::from_utf8(process.error())));
        if (process.value()->send(message).is_error()) {
            m_ending.append({ caller.extension_id, process.value()->pid(), MonotonicTime::now() });
            process.value()->close();
            return reply(MUST(String::from_utf8(io_error)));
        }

        auto id = m_next_host_id++;
        auto& host_process = *process.value();
        host_process.on_message = [this, id](JsonValue message) {
            auto host = m_hosts.take(id);
            if (!host.has_value())
                return;
            close_host(*host);
            host->reply(move(message));
        };
        host_process.on_end = [this, id](WebExtensions::NativeHostProcess::Ending ending) {
            end_host(id, ending == WebExtensions::NativeHostProcess::Ending::Exited ? exited_error : io_error);
        };
        m_hosts.set(id, Host { .process = process.release_value(), .extension_id = caller.extension_id, .context_id = caller.id, .port = {}, .reply = move(reply) });
    }

    // The host ended or failed: its port closes, or its message is answered, with the error.
    void end_host(u64 id, StringView error)
    {
        auto host = m_hosts.take(id);
        if (!host.has_value())
            return;
        close_host(*host);
        auto message = MUST(String::from_utf8(error));
        if (host->port.has_value()) {
            m_host_of_port.remove(*host->port);
            runtime().close_browser_port(*host->port, move(message));
        } else if (host->reply) {
            host->reply(move(message));
        }
    }

    void close_host(Host& host)
    {
        if (!host.process->is_closed() && host.process->pid() > 0)
            m_ending.append({ host.extension_id, host.process->pid(), MonotonicTime::now() });
        host.process->close();
    }

    void forget_ended_hosts()
    {
        auto now = MonotonicTime::now();
        m_ending.remove_all_matching([&](EndingHost const& host) {
            return now - host.closed_at > ending_host_lifetime || ::kill(host.pid, 0) < 0;
        });
    }

    HashMap<u64, Host> m_hosts;
    Vector<EndingHost> m_ending;
    HashMap<String, LaunchBudget> m_launch_budgets;
    HashMap<u64, u64> m_host_of_port;
    u64 m_next_host_id { 1 };
};

NonnullOwnPtr<ExtensionAPI> create_native_messaging_api(ExtensionRuntime& runtime)
{
    return make<NativeMessagingAPI>(runtime);
}

}
