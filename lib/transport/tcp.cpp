// lib/transport/tcp.cpp - asio-backed TCP transport (task 4.1). See tcp.hpp.
//
// Concurrency model: everything is poll()-driven on the caller's thread. The
// only async operation is accept (paired with io_context::poll(), which runs
// only already-ready handlers, so pump() never blocks). Reads ride the
// non-blocking socket inside TcpByteStream; writes are short full-write loops.
// That keeps the P3 session state machine (injected clock, no callbacks in its
// internals) exactly in charge of handshake/keepalive/seq/dead-peer decisions.
#include "tcp.hpp"

#include "logging.hpp"
#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace tether::transport {
namespace {

using Logger = common::Logger;

std::string socket_error_text(const asio::error_code& ec) { return ec.message(); }

void configure_client_socket(asio::ip::tcp::socket& socket, asio::error_code& ec) {
    // Non-blocking reads are what makes IByteSource::read never block; keepalive
    // riding the socket is the P3 state machine's job (PING/PONG frames), so no
    // OS-level keepalive is enabled here.
    socket.non_blocking(true, ec);
    if (ec) {
        return;
    }
    asio::ip::tcp::no_delay option(true);
    socket.set_option(option, ec);
}
} // namespace

// ---- TcpByteStream ----------------------------------------------------------

TcpByteStream::TcpByteStream(std::shared_ptr<asio::io_context> io, asio::ip::tcp::socket socket,
                             std::string log_identity)
    : io_(std::move(io)), socket_(std::move(socket)), log_identity_(std::move(log_identity)) {
    asio::error_code ec;
    socket_.non_blocking(true, ec);
    if (ec) {
        Logger::instance().warn("tcp stream: could not set non_blocking",
                                {{"peer", log_identity_}, {"error", ec.message()}});
    }
    asio::ip::tcp::no_delay option(true);
    socket_.set_option(option, ec);
    if (ec) {
        Logger::instance().warn("tcp stream: could not set TCP_NODELAY",
                                {{"peer", log_identity_}, {"error", ec.message()}});
    }
}

protocol::ReadResult TcpByteStream::read(unsigned char* dst, std::size_t max) {
    if (closed_) {
        return {0, true};
    }
    asio::error_code ec;
    const std::size_t n = socket_.read_some(asio::buffer(dst, max), ec);
    if (ec == asio::error::would_block || ec == asio::error::try_again) {
        return {0, false}; // wire idle: the session may run timers instead
    }
    if (ec == asio::error::eof) {
        closed_ = true;
        return {n, true};
    }
    if (ec) {
        // connection reset/aborted/broken pipe: stream ended as far as the
        // session state machine is concerned either way (Down).
        Logger::instance().info(
            "tcp stream ended on socket error", 
            {{"peer", log_identity_}, {"error", socket_error_text(ec)}});
        closed_ = true;
        return {n, true};
    }
    return {n, false};
}

void TcpByteStream::write(const unsigned char* bytes, std::size_t count) {
    if (closed_) {
        throw TransportError("tcp write after stream close (peer=" + log_identity_ + ")");
    }
    std::size_t sent = 0;
    asio::error_code ec;
    while (sent < count) {
        const std::size_t n = socket_.write_some(asio::buffer(bytes + sent, count - sent), ec);
        sent += n;
        if (!ec) {
            continue;
        }
        if (ec == asio::error::would_block || ec == asio::error::try_again) {
            // Kernel send buffer full: bounded co-operative wait, never a hard
            // block. Loopback command/result frames never come close to this.
            std::this_thread::yield();
            continue;
        }
        Logger::instance().warn("tcp send failed",
                                {{"peer", log_identity_}, {"error", socket_error_text(ec)}});
        closed_ = true;
        throw TransportError("tcp send failed (peer=" + log_identity_ + "): " + ec.message());
    }
}

void TcpByteStream::shutdown() {
    if (closed_) {
        return;
    }
    closed_ = true;
    asio::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

// ---- TcpConsoleTransport ----------------------------------------------------

TcpConsoleTransport::TcpConsoleTransport(ConsoleTransportOptions options)
    : options_(std::move(options)),
      acceptor_(io_),
      pending_socket_(io_),
      registry_() {
    options_.session.validate();
    if (!options_.resolve_token) {
        throw TransportError("a token resolver is required on the console transport");
    }
}

TcpConsoleTransport::~TcpConsoleTransport() {
    asio::error_code ec;
    acceptor_.close(ec);
    for (auto& connection : connections_) {
        if (connection.stream) {
            connection.stream->shutdown();
        }
    }
}

void TcpConsoleTransport::start() {
    asio::ip::tcp::resolver resolver(io_);
    asio::error_code ec;
    const asio::ip::tcp::resolver::results_type endpoints =
        resolver.resolve(options_.listen_address, std::to_string(options_.port), ec);
    if (ec) {
        throw TransportError("console listener: resolve '" + options_.listen_address + ":" +
                             std::to_string(options_.port) + "' failed: " + ec.message());
    }
    acceptor_.open(endpoints.begin()->endpoint().protocol(), ec);
    if (ec) {
        throw TransportError("console listener: open failed: " + ec.message());
    }
    acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        throw TransportError("console listener: set_option failed: " + ec.message());
    }
    acceptor_.bind(endpoints.begin()->endpoint(), ec);
    if (ec) {
        throw TransportError("console listener: bind failed: " + ec.message());
    }
    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw TransportError("console listener: listen failed: " + ec.message());
    }
    listening_port_ = acceptor_.local_endpoint(ec).port();
    if (ec) {
        throw TransportError("console listener: local_endpoint failed: " + ec.message());
    }
    started_ = true;
    Logger::instance().info("console listening",
                            {{"address", options_.listen_address},
                             {"port", std::to_string(listening_port_)}});
    begin_accept();
}

void TcpConsoleTransport::begin_accept() {
    acceptor_.async_accept(pending_socket_, [this](const asio::error_code& ec) {
        if (ec == asio::error::operation_aborted) {
            return; // stopping
        }
        if (ec) {
            Logger::instance().warn("console accept failed", {{"error", ec.message()}});
        } else {
            asio::error_code socket_ec;
            configure_client_socket(pending_socket_, socket_ec);
            if (socket_ec) {
                Logger::instance().warn("accepted socket configuration failed, dropping",
                                        {{"error", socket_ec.message()}});
            } else {
                // The console transport's io_ outlives every stream it spawns
                // (members destroy in reverse declaration order), so aliasing
                // via a non-owning shared_ptr is lifetime-correct here.
                auto console_io = std::shared_ptr<asio::io_context>(&io_, [](asio::io_context*) {});
                auto stream = std::make_unique<TcpByteStream>(std::move(console_io),
                                                              std::move(pending_socket_), "agent");
                auto session = std::make_shared<protocol::ConsoleSession>(
                    *stream, *stream, options_.session, options_.resolve_token);
                connections_.push_back(Connection{std::move(stream), std::move(session), "",
                                                 false, false, std::nullopt});
                Logger::instance().info("console accepted a connection",
                                        {{"sessions", std::to_string(connections_.size())}});
            }
        }
        pending_socket_ = asio::ip::tcp::socket(io_);
        if (started_) {
            begin_accept();
        }
    });
}

void TcpConsoleTransport::pump(TransportMillis now) {
    if (!started_) {
        throw TransportError("console transport pumped before start()");
    }
    // Runs ready accept completions only - never blocks.
    io_.poll();

    for (auto& connection : connections_) {
        if (connection.dead) {
            continue;
        }
        try {
            // Translate into this connection's session clock (accept = 0):
            // P3's auth/command/keepalive timers assume the session domain
            // starts with the session itself (which every P3 test asserts).
            if (!connection.epoch) {
                connection.epoch = now;
            }
            connection.session->pump(now - *connection.epoch);
        } catch (const TransportError& e) {
            // A dead send path does not upgrade the session state by itself;
            // treat this connection as bailed out (the peer's next dial-out
            // re-registers a fresh session via the registry).
            Logger::instance().warn("session pump failed on dead wire",
                                    {{"session", connection.identity.empty() ? "?" : connection.identity},
                                     {"error", e.what()}});
            connection.dead = true;
            continue;
        }
        // Register on transition into Established (once per connection):
        // AgentRegistry handles duplicate-identity supersession (P3).
        if (!connection.registered && connection.session->state() == protocol::SessionState::Established) {
            connection.identity = connection.session->identity();
            const bool superseded =
                registry_.register_session(connection.identity, connection.session);
            connection.registered = true;
            Logger::instance().info(
                "agent session registered",
                {{"agent", connection.identity},
                 {"superseded", superseded ? "true" : "false"}});
        }
        if (connection.session->state() == protocol::SessionState::Down) {
            connection.dead = true;
        }
    }

    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
                       [](const Connection& c) { return c.dead; }),
        connections_.end());
}

std::uint32_t TcpConsoleTransport::issue(const std::string& agent, const protocol::Cmd& command,
                                         const protocol::ResultHook& hook, TransportMillis now) {
    const std::shared_ptr<protocol::ConsoleSession> session = registry_.lookup(agent);
    if (!session) {
        throw TransportError("no session for agent '" + agent + "'");
    }
    if (session->state() != protocol::SessionState::Established) {
        throw TransportError("agent '" + agent + "' session is " +
                             protocol::session_state_name(session->state()) +
                             "; not eligible to receive commands");
    }
    // Command deadlines share the session's clock domain (see pump).
    protocol::SessionMillis session_now = 0;
    for (const Connection& connection : connections_) {
        if (connection.session == session) {
            session_now = now - connection.epoch.value_or(now);
            break;
        }
    }
    return session->start(command, session_now, hook);
}

bool TcpConsoleTransport::has_agent(const std::string& identity) const {
    return registry_.lookup(identity) != nullptr;
}

std::shared_ptr<protocol::ConsoleSession> TcpConsoleTransport::session_for(
    const std::string& identity) const {
    return registry_.lookup(identity);
}

std::vector<std::string> TcpConsoleTransport::registered_identities() const {
    std::vector<std::string> identities;
    for (const auto& connection : connections_) {
        if (connection.registered && !connection.identity.empty()) {
            identities.push_back(connection.identity);
        }
    }
    return identities;
}

// Task 5.1: the CLI's `agents` verb lists through the protocol registry so
// identities keep appearing with their session state even after a session
// goes down (registered_identities() only covers live connections).
std::vector<protocol::RegistryEntry> TcpConsoleTransport::registry_entries() {
    return registry_.list();
}

// ---- agent dialer -------------------------------------------------------------

std::int64_t next_backoff_ms(std::int64_t current, const DialPolicy& policy) {
    const std::int64_t first = policy.initial_backoff_ms;
    if (current <= 0) {
        return first;
    }
    const std::int64_t doubled = current * 2;
    return doubled > policy.max_backoff_ms ? policy.max_backoff_ms : doubled;
}

TcpAgentLink::TcpAgentLink(std::unique_ptr<TcpByteStream> stream, protocol::AgentIdentity identity,
                           std::string token, protocol::SessionConfig session)
    : stream_(std::move(stream)) {
    session_ = std::make_unique<protocol::AgentSession>(*stream_, *stream_, std::move(identity),
                                                        std::move(token), std::move(session));
}

TcpAgentLink::~TcpAgentLink() = default;

std::unique_ptr<TcpAgentLink> TcpAgentLink::connect(const AgentEndpoint& endpoint,
                                                    const protocol::AgentIdentity& identity,
                                                    const std::string& token,
                                                    const protocol::SessionConfig& session) {
    // The context must outlive the socket the link keeps, so the stream ends
    // up owning it (a function-local io_context dies with this frame, and any
    // later socket op on it is use-after-free).
    auto io = std::make_shared<asio::io_context>();
    asio::ip::tcp::socket socket(*io);
    asio::ip::tcp::resolver resolver(*io);
    asio::error_code ec;
    const asio::ip::tcp::resolver::results_type endpoints =
        resolver.resolve(endpoint.host, std::to_string(endpoint.port), ec);
    if (ec) {
        throw TransportError("dial-out: resolve '" + endpoint.host + ":" +
                             std::to_string(endpoint.port) + "' failed: " + ec.message());
    }
    // Synchronous connect: the caller (agent main / tests) owns the thread.
    asio::connect(socket, endpoints, ec);
    if (ec) {
        throw TransportError("dial-out: connect to '" + endpoint.host + ":" +
                             std::to_string(endpoint.port) + "' failed: " + ec.message());
    }
    configure_client_socket(socket, ec);
    if (ec) {
        throw TransportError("dial-out: socket configuration failed: " + ec.message());
    }
    auto stream = std::make_unique<TcpByteStream>(std::move(io), std::move(socket), identity.name);
    // Direct construction (not make_unique): the constructor is private, and
    // construction inside libstdc++'s make_unique would be checked in its
    // context, not this member's.
    return std::unique_ptr<TcpAgentLink>(
        new TcpAgentLink(std::move(stream), identity, token, session));
}

std::unique_ptr<TcpAgentLink> establish_with_retries(const AgentEndpoint& endpoint,
                                                     const protocol::AgentIdentity& identity,
                                                     const std::string& token,
                                                     const protocol::SessionConfig& session,
                                                     DialPolicy policy,
                                                     std::size_t max_attempts) {
    std::int64_t backoff = 0;
    std::size_t attempt = 0;
    for (;;) {
        ++attempt;
        try {
            return TcpAgentLink::connect(endpoint, identity, token, session);
        } catch (const TransportError& e) {
            Logger::instance().warn("dial-out attempt failed",
                                    {{"agent", identity.name},
                                     {"attempt", std::to_string(attempt)},
                                     {"error", e.what()}});
        }
        if (max_attempts != 0 && attempt >= max_attempts) {
            throw TransportError("dial-out: console unreachable after " + std::to_string(attempt) +
                                 " attempts (" + endpoint.host + ":" +
                                 std::to_string(endpoint.port) + ")");
        }
        backoff = next_backoff_ms(backoff, policy);
        Logger::instance().info("dial-out backed off",
                                {{"agent", identity.name},
                                 {"backoff_ms", std::to_string(backoff)}});
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
    }
}

} // namespace tether::transport
