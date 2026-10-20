// lib/transport/transport.hpp - the D2 transport seam for tether v2 (Phase 4,
// tasks 4.1-4.4). ONE interface, two implementations: the direct TCP transport
// (tcp.hpp) and the Gmail polling adapter (gmail.hpp). Both speak protocol v2
// `Message`s only - the protocol layer (lib/protocol) never learns which wire
// carried a frame.
//
// Time model: transports are pump()-driven with injected milliseconds (the
// Phase 3 convention, `SessionMillis`'s transport twin). No transport blocks
// waiting for the network; blocking happens only inside `connect`/accept
// attempts, called from the code that owns the thread.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "session.hpp"

namespace tether::common {
struct ConsoleConfig;
}

namespace tether::transport {

// Milliseconds since an arbitrary (in practice monotonic) epoch; the caller
// supplies it so tests and production share the same deterministic contract.
using TransportMillis = std::int64_t;

// The shared monotonic tick for real-driven pumps (console main, agent main,
// integration tests): steady-clock milliseconds.
TransportMillis monotonic_now_ms();

// Every transport failure (unreachable agent, refused session, wire death,
// rejected mail) is explicit - never swallowed by a silent no-op.
class TransportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Design D2: "a transport interface with two implementations ... both present
// equivalent command/result semantics".
//
// issue(): route a command toward `agent`; results surface through `hook` by
// seq. TCP routes over the live session stack (registry lookup + CMD frame).
// Gmail does NOT issue programmatically - its commands arrive as admin mail
// through pump(); issue() throws TransportError there (flagged interpretation,
// recorded in the phase report: mail -> command translation is the Gmail
// transport's issuance mechanism, so the interface method is deliberately
// unsupported on that side).
//
// pump(now): advance the transport one non-blocking step. TCP: process accept
// completions, pump sessions, run keepalive timers. Gmail: at most one inbox
// poll cycle, honoring the configured cadence and error backoff.
class ITransport {
public:
    virtual ~ITransport() = default;

    virtual std::uint32_t issue(const std::string& agent, const protocol::Cmd& command,
                                const protocol::ResultHook& hook, TransportMillis now) = 0;
    virtual void pump(TransportMillis now) = 0;
};

// ---- console transport wiring (task 4.4 config-enablement) -------------------

// What the factory builds for a console config. TCP is always present;
// gmail is NULL unless the config enables it - a TCP-only configuration never
// constructs the Gmail adapter (and therefore never loads its credentials nor
// issues a single API call).
struct ConsoleTransportSet {
    std::unique_ptr<ITransport> tcp;
    std::unique_ptr<ITransport> gmail;
    bool gmail_enabled = false;
};

// `make_gmail_client` is invoked ONLY when gmail is enabled (tests spy on it
// to prove the TCP-only absence). On enablement the factory loads OAuth
// credentials from the configured path (outside the repo), derives the
// adapter options, and routes mail-issued commands into the TCP transport.
class GmailClient;
ConsoleTransportSet build_console_transports(
    const common::ConsoleConfig& config, const protocol::SessionConfig& session_config,
    const protocol::TokenResolver& tokenize,
    const std::function<std::unique_ptr<GmailClient>()>& make_gmail_client);

} // namespace tether::transport
