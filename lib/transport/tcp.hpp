// lib/transport/tcp.hpp - the primary TCP transport (design D2, task 4.1).
//
// Console side: an asio-backed listener that accepts multiple agents
// concurrently and registers each authenticated session in the P3
// AgentRegistry (identity supersession comes free from that layer).
// Agent side: a dialer that connects outbound (design D1) with bounded
// exponential-ish retry/backoff at startup.
//
// Both use the same socket adapter, which is the Phase 3 byte seam made real:
// TcpByteStream implements protocol::IByteSource/IByteSink over a live asio
// socket, ConsoleSession/AgentSession keep driving handshake, keepalive
// (PING/PONG), seq correlation and dead-peer state from there. Stream end
// (EOF) and hard socket errors surface as end_of_stream, which the session
// state machine already maps to Down - so an abruptly killed agent is
// detected on the console's next pump, and a silently vanished one within the
// configured keepalive window.
#pragma once

#include "transport.hpp"

#include "session.hpp"

#include <asio.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tether::protocol {
class ConsoleSession;
} // namespace tether::protocol

namespace tether::transport {

// ---- shared socket byte seam -----------------------------------------------

// Non-blocking reads (would_block -> 0 bytes; EOF/reset -> end_of_stream) and
// short-spin full writes. One instance serves both wire directions of one
// session. The instance OWNS a reference to the io_context its socket is
// registered with (agent-side links get a context that would otherwise die
// with their connect() frame; console-side streams alias the transport's io_).
class TcpByteStream final : public protocol::IByteSource, public protocol::IByteSink {
public:
    TcpByteStream(std::shared_ptr<asio::io_context> io, asio::ip::tcp::socket socket,
                  std::string log_identity);

    protocol::ReadResult read(unsigned char* dst, std::size_t max) override;
    void write(const unsigned char* bytes, std::size_t count) override;

    // Abrupt local shutdown: kills the wire now (the peer's next read reports
    // EOF/connection reset). Used by tests and teardown.
    void shutdown();
    bool closed() const noexcept { return closed_; }
    const std::string& log_identity() const noexcept { return log_identity_; }

    asio::ip::tcp::socket& socket() noexcept { return socket_; }

private:
    std::shared_ptr<asio::io_context> io_;
    asio::ip::tcp::socket socket_;
    std::string log_identity_;
    bool closed_ = false;
};

// ---- console side ------------------------------------------------------------

struct ConsoleTransportOptions {
    std::string listen_address;          // e.g. "127.0.0.1"
    std::uint16_t port = 0;              // 0 -> ephemeral (tests report actual)
    protocol::SessionConfig session;     // handshake/keepalive/timeout knobs
    protocol::TokenResolver resolve_token; // HELLO identity -> provisioned token
};

// Accepts multiple agents concurrently; each HELLO-superseding session
// registers in the AgentRegistry it owns. Sessions go Down through the same
// state machine as the wire: EOF after an abrupt close, protocol violations,
// and the keepalive window all trace back there.
class TcpConsoleTransport final : public ITransport {
public:
    explicit TcpConsoleTransport(ConsoleTransportOptions options);
    ~TcpConsoleTransport() override;

    // Bind + listen + begin accepting. Throws TransportError on a bind/listen
    // failure ("address in use", no route, ...).
    void start();
    std::uint16_t listening_port() const noexcept { return listening_port_; }
    bool started() const noexcept { return started_; }

    // ITransport.
    void pump(TransportMillis now) override;
    std::uint32_t issue(const std::string& agent, const protocol::Cmd& command,
                        const protocol::ResultHook& hook, TransportMillis now) override;

    // Observable session state for tests/registry wiring (P3 AgentRegistry).
    bool has_agent(const std::string& identity) const;
    std::shared_ptr<protocol::ConsoleSession> session_for(const std::string& identity) const;
    std::vector<std::string> registered_identities() const;

private:
    struct Connection {
        std::unique_ptr<TcpByteStream> stream;
        std::shared_ptr<protocol::ConsoleSession> session;
        std::string identity;
        bool registered = false;
        bool dead = false;
        // The connection's SessionMillis origin: P3 sessions live on a caller
        // -owned clock that starts with the session, so the transport maps
        // every wall `now` into the session's domain (accept -> 0).
        std::optional<protocol::SessionMillis> epoch;
    };

    void begin_accept();

    ConsoleTransportOptions options_;
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket pending_socket_;
    std::vector<Connection> connections_;
    protocol::AgentRegistry registry_;
    std::uint16_t listening_port_ = 0;
    bool started_ = false;
};

// ---- agent side: dial out with bounded retry/backoff -------------------------

struct DialPolicy {
    std::int64_t initial_backoff_ms = 500;  // first retry budget
    std::int64_t max_backoff_ms = 10'000;   // the cap (bounded interval)
};

// Backoff sequence: initial, then doubled each failure, capped at max. Pure;
// unit-tested separately from any socket behavior.
std::int64_t next_backoff_ms(std::int64_t current, const DialPolicy& policy);

struct AgentEndpoint {
    std::string host;
    std::uint16_t port;
};

// One live agent-side link: connected socket + P3 AgentSession on top.
class TcpAgentLink {
public:
    // Synchronous connect, then HELLO is sent by dial-out loop / callers via
    // the session's begin(). Throws TransportError when connect fails.
    // Static so the policy loop stays a plain function, unit-testable.
    static std::unique_ptr<TcpAgentLink> connect(const AgentEndpoint& endpoint,
                                                 const protocol::AgentIdentity& identity,
                                                 const std::string& token,
                                                 const protocol::SessionConfig& session);

    ~TcpAgentLink();
    TcpAgentLink(const TcpAgentLink&) = delete;
    TcpAgentLink& operator=(const TcpAgentLink&) = delete;

    void begin_handshake(TransportMillis now) { session_->begin(now); }
    void set_command_handler(protocol::CommandHandler handler) { session_->set_command_handler(std::move(handler)); }
    void pump(TransportMillis now) { session_->pump(now); }
    // Agent-side result flow, forwarded to the session ( pieces may be
    // partial until a terminal piece closes the seq - see AgentSession).
    void send_result(std::uint32_t seq, const protocol::Message& piece) {
        session_->send_result(seq, piece);
    }
    protocol::SessionState state() const noexcept { return session_->state(); }
    bool established() const noexcept { return session_->state() == protocol::SessionState::Established; }
    bool down() const noexcept { return session_->state() == protocol::SessionState::Down; }
    void shutdown_stream() { stream_->shutdown(); }
    void close_session() { session_.reset(); }

private:
    TcpAgentLink(std::unique_ptr<TcpByteStream> stream, protocol::AgentIdentity identity,
                 std::string token, protocol::SessionConfig session);

    std::unique_ptr<TcpByteStream> stream_;
    std::unique_ptr<protocol::AgentSession> session_;
};

// Retry loop: attempt connect; on failure wait `next_backoff_ms` and retry.
// `max_attempts == 0` means retry forever (the spec: the agent stays up and
// connects once the console becomes reachable). A nonzero budget exists for
// deterministic tests only. Real sleeps are bounded by the policy cap.
std::unique_ptr<TcpAgentLink> establish_with_retries(const AgentEndpoint& endpoint,
                                                     const protocol::AgentIdentity& identity,
                                                     const std::string& token,
                                                     const protocol::SessionConfig& session,
                                                     DialPolicy policy,
                                                     std::size_t max_attempts = 0);

} // namespace tether::transport
