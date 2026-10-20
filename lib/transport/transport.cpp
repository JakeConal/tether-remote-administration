// lib/transport/transport.cpp - console transport wiring (task 4.4's
// "transports are independently enableable by configuration"): the factory
// reads the console config's optional gmail section and builds only what is
// enabled. TCP is always present; Gmail is constructed (and its client with
// it - credentials load included) ONLY when gmail.enabled. A TCP-only config
// therefore never constructs or polls the Gmail adapter at all.
#include "transport.hpp"

#include "config.hpp"
#include "gmail.hpp"
#include "tcp.hpp"

#include <chrono>
#include <memory>
#include <utility>

namespace tether::transport {

TransportMillis monotonic_now_ms() {
    return std::int64_t{std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count()};
}

namespace {

// (Adapter knob derivation happens inline in build_console_transports.)

} // namespace

ConsoleTransportSet build_console_transports(
    const common::ConsoleConfig& config, const protocol::SessionConfig& session_config,
    const protocol::TokenResolver& tokenize,
    const std::function<std::unique_ptr<GmailClient>()>& make_gmail_client) {
    ConsoleTransportOptions options;
    options.listen_address = config.listen_address;
    options.port = static_cast<std::uint16_t>(config.port);
    options.session = session_config;
    options.resolve_token = tokenize;

    auto tcp = std::make_unique<TcpConsoleTransport>(std::move(options));

    ConsoleTransportSet set;
    ITransport* tcp_raw = tcp.get();
    set.tcp = std::move(tcp);

    const bool gmail_enabled = config.gmail.has_value() && config.gmail->enabled;
    if (gmail_enabled) {
        const common::GmailSectionConfig& gmail = *config.gmail;
        // Credentials load from the configured path OUTSIDE the repo (D8).
        const GmailCredentials credentials = load_gmail_credentials(gmail.credentials_path);

        GmailTransportOptions gmail_options;
        gmail_options.poll_interval_ms = std::int64_t{gmail.poll_interval_seconds} * 1000;
        // Adapter knobs derive from the poll interval (no extra config surface;
        // flagged in the phase report): a failed API cycle waits another full
        // interval before retrying, doubling up to ten intervals - API stress
        // decays instead of spinning, and success restores normal cadence.
        gmail_options.backoff_initial_ms = gmail_options.poll_interval_ms;
        gmail_options.backoff_max_ms = gmail_options.poll_interval_ms * 10;
        gmail_options.admin_list = gmail.admin_list;
        gmail_options.adapter_account = credentials.account;
        gmail_options.default_agent = gmail.default_agent;

        // Mail-issued commands route into the very same transport-virtual seam
        // as console-issued ones (the TCP transport).
        CommandDispatcher dispatcher =
            [tcp_raw](const std::string& agent, const protocol::Cmd& command,
                      const protocol::ResultHook& hook, TransportMillis now) {
                return tcp_raw->issue(agent, command, hook, now);
            };
        set.gmail = std::make_unique<GmailTransport>(make_gmail_client(),
                                                     std::move(gmail_options),
                                                     std::move(dispatcher));
        set.gmail_enabled = true;
    }
    return set;
}

} // namespace tether::transport
