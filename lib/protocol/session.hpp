// lib/protocol/session.hpp - the session state machines (task 3.4) beneath
// the secure-session delta: dial-out authentication handshake, seq-correlated
// commands/results, CANCEL, PING/PONG keepalive, per-command timeouts, and
// identity registration supersession.
//
// Sockets are Phase 4: each side is wired to (IByteSource, IByteSink) from
// bytes.hpp, and time is INJECTED - every pump()/start() call takes the
// current time in milliseconds (deterministic tests, no sleeps). Durations
// are injected via SessionConfig. pump() drains available inbound bytes,
// dispatches frames, then applies the timers (auth expiry, command deadlines,
// keepalive cadence, dead-peer window) at that instant.
//
// State flow:
//   Connecting  - handshake in progress (HELLO/CHALLENGE/AUTH_OK)
//   Established - fully authenticated and eligible
//   Stale       - console-only: a newer session took over the identity
//                 (ineligible for commands until re-registration)
//   Down        - refused / peer dead / stream error / clean EOF
//
// Wire conventions (for 7.1's protocol doc): HELLO/CHALLENGE/AUTH_OK/PING/
// PONG travel with seq = 0; CMD and every result/ERR frame carry the
// command's seq, and CANCEL carries its target's seq. Command seqs start at
// 1 so 0 can never collide with command traffic.
#pragma once

#include "bytes.hpp"
#include "handshake.hpp"
#include "payloads.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace tether::protocol {

using SessionMillis = std::int64_t;

struct SessionConfig {
    SessionMillis command_timeout = 30'000;    // per-command default, overridable
    SessionMillis auth_timeout = 10'000;       // handshake must complete within this
    SessionMillis keepalive_interval = 10'000; // PING cadence while Established
    SessionMillis keepalive_window = 30'000;   // no life sign this long -> Down

    // Fails fast on non-positive durations or a window smaller than the
    // interval, instead of wedging sessions later.
    void validate() const;
};

enum class SessionState { Connecting, Established, Stale, Down };
const char* session_state_name(SessionState state);

// Misuse of a session (issuing commands while unauthenticated, replying about
// an unknown command, ...) is an explicit error, not a silent no-op.
class SessionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class CommandStatus { Pending, Completed, Failed, Cancelled };

// A read-only snapshot of a command's state on the console.
// - pieces: decoded result messages in arrival order (RESULT_TEXT, or the
//   FILE_META/FILE_CHUNK/(FILE_END|ERR) stream).
// - error_code: "timeout", the agent's ERR code, or "cancelled".
struct CommandView {
    std::uint32_t seq = 0;
    CommandStatus status = CommandStatus::Pending;
    std::string command;
    std::vector<Message> pieces;
    std::string error_code;
};

struct CommandOptions {
    std::optional<SessionMillis> timeout; // overrides SessionConfig::command_timeout
};

using ResultHook = std::function<void(std::uint32_t seq, const Message& piece, bool terminal)>;
using TokenResolver = std::function<std::string(const std::string& agent_name)>;

// ---- Console (operator) side ----------------------------------------------

class ConsoleSession {
public:
    // `source`/`sink` carry this session's two wire directions (from
    // bytes.hpp). `resolve_token` maps a HELLO identity to that agent's
    // provisioned token; an empty return means an unknown identity, which is
    // refused without issuing a verifiable challenge (spec: no command
    // exchange with an agent that cannot authenticate).
    ConsoleSession(IByteSource& source, IByteSink& sink, SessionConfig config,
                   TokenResolver resolve_token);

    // Drain + dispatch inbound bytes, then run auth/timeout/keepalive timers.
    // Peer death after the keepalive window, stream EOF, truncation, or any
    // protocol violation downgrades the session to Down and logs it.
    void pump(SessionMillis now);

    SessionState state() const noexcept { return state_; }
    // Filled once a HELLO arrives; kept even after a refusal (for logs).
    const std::string& identity() const noexcept { return identity_; }

    // Issue a command at time `now`: allocates the next seq, records the
    // tracker, and sends CMD. Results arrive correlated by seq through
    // pump(). Throws SessionError unless Established (Stale is ineligible).
    std::uint32_t start(const Cmd& command, SessionMillis now, const ResultHook& hook = nullptr,
                        const CommandOptions& options = {});

    // Tell the agent to stop an in-flight command (spec: CANCEL to the agent).
    // Returns whether a pending command was targeted; the agent's
    // acknowledgement (ERR "cancelled" for that seq) completes the
    // transition to Cancelled. Nothing on the session is poisoned on cancel.
    bool cancel(std::uint32_t seq);

    // Tracking of pending and terminal states alike.
    std::optional<CommandView> command(std::uint32_t seq) const;
    std::vector<std::uint32_t> outstanding_seqs() const;

    // Registry wiring for identity supersession (AgentRegistry below).
    void mark_stale();

private:
    struct Tracker {
        CommandStatus status = CommandStatus::Pending;
        std::string command;
        std::vector<Message> pieces;
        std::string error_code;
        SessionMillis deadline = 0;
        ResultHook hook;
    };

    void handle_frame(const Frame& frame, SessionMillis now);
    void send_frame(MsgType type, std::uint32_t seq, const std::vector<unsigned char>& payload);
    void deliver_result(std::uint32_t seq, const Message& piece, bool terminal);
    void fail_command(std::uint32_t seq, const std::string& code, CommandStatus status);
    void go_down(std::string reason);
    void run_timers(SessionMillis now);

    IByteSource& source_;
    IByteSink& sink_;
    SessionConfig config_;
    TokenResolver resolve_token_;
    FrameDecoder decoder_;
    std::unique_ptr<Challenger> challenger_;
    SessionState state_ = SessionState::Connecting;
    std::string identity_;
    SessionMillis opened_at_ = 0;
    SessionMillis last_seen_ = 0;
    SessionMillis last_ping_ = 0;
    std::uint32_t next_seq_ = 1;
    std::map<std::uint32_t, Tracker> outstanding_;
    std::map<std::uint32_t, Tracker> finished_;
};

// ---- Agent (target) side ---------------------------------------------------

// Invoked per inbound CMD. `is_cancelled` flips when the console cancels
// this seq; the handler polls it to unwind early. Reports flow back through
// send_result(). Throwing from the handler propagates out of pump() - the
// dispatch layer in a later phase decides the recovery policy.
using CommandHandler = std::function<void(
    std::uint32_t seq, const Cmd& command, const std::function<bool()>& is_cancelled)>;

struct AgentIdentity {
    std::string name; // unique registry identity carried by HELLO
    std::string id;   // install instance identifier carried by HELLO
};

class AgentSession {
public:
    AgentSession(IByteSource& source, IByteSink& sink, AgentIdentity identity, std::string token,
                 SessionConfig config);

    // Handshake step 1: send HELLO. Throws SessionError on an empty
    // provisioned token (config.hpp documents that sessions reject empty
    // tokens; enforced here at the enforcement point).
    void begin(SessionMillis now);

    void pump(SessionMillis now);
    SessionState state() const noexcept { return state_; }

    void set_command_handler(CommandHandler handler) { handler_ = std::move(handler); }

    // Agent-side result flow: pieces may be partial (FILE_META/FILE_CHUNK)
    // until one terminal piece (RESULT_TEXT / FILE_END / ERR) closes the seq.
    // Throws SessionError for unknown/finished seqs or non-result messages;
    // in particular, sending after a CANCEL ack is a handler bug.
    void send_result(std::uint32_t seq, const Message& piece);

private:
    struct InFlight {
        std::shared_ptr<bool> cancelled;
    };

    void handle_frame(const Frame& frame, SessionMillis now);
    void send_frame(MsgType type, std::uint32_t seq, const std::vector<unsigned char>& payload);
    void go_down(std::string reason);
    void run_timers(SessionMillis now);

    IByteSource& source_;
    IByteSink& sink_;
    AgentIdentity identity_;
    std::string token_;
    SessionConfig config_;
    FrameDecoder decoder_;
    std::unique_ptr<Responder> responder_;
    SessionState state_ = SessionState::Connecting;
    SessionMillis opened_at_ = 0;
    SessionMillis last_seen_ = 0;
    SessionMillis last_ping_ = 0;
    std::map<std::uint32_t, InFlight> in_flight_;
    CommandHandler handler_;
};

// ---- Identity registration (secure-session delta: one live session per
// identity). -----------------------------------------------------------------

// 5.1: the CLI-facing listing state. It is DERIVED at query time from each
// bound session's state machine - the registry never keeps a parallel copy of
// the state, so the listing cannot drift from the session layer:
//   Established  -> live   (authenticated and eligible)
//   Stale        -> stale  (superseded but still connected)
//   Connecting / Down -> down (down for disconnected or detected-dead
//                                 agents; the transport never registers a
//                                 Connecting session, the bucket is a net)
enum class RegistryState { Live, Stale, Down };
const char* registry_state_name(RegistryState state);

// One listing row: an identity plus the derived state of one tracked session.
struct RegistryEntry {
    std::string identity;
    RegistryState state = RegistryState::Down;
};

class AgentRegistry {
public:
    // Registers (or replaces) the session bound to `identity`; returns
    // whether an older live session was superseded (marked stale). Distinct
    // identities simply coexist. Superseded-but-connected sessions keep
    // showing up in list() as stale until they go down (5.1).
    bool register_session(const std::string& identity,
                          const std::shared_ptr<ConsoleSession>& session);

    // The newest session for an identity (whatever its current state), or
    // null. Whether it is *eligible* is the caller's check: state must be
    // Established. This is what makes "commands served by the newest" hold.
    std::shared_ptr<ConsoleSession> lookup(const std::string& identity) const;

    std::size_t size() const noexcept { return by_identity_.size(); }

    // 5.1 listing accessors (spec: live/stale/down, listing changes as the
    // sessions change).
    bool has_agent(const std::string& identity) const;
    // Derived registry state of an identity's newest session; an unknown
    // identity reports Down.
    RegistryState state_of(const std::string& identity) const;
    // One row per identity (the newest session's state), sorted by identity,
    // followed by one `stale` row per still-connected superseded session of
    // that identity. Superseded sessions that went down are pruned here (their
    // listing life ends with the connection).
    std::vector<RegistryEntry> list();

private:
    std::map<std::string, std::shared_ptr<ConsoleSession>> by_identity_;
    // Superseded-but-tracked sessions per identity (5.1), pruned as they go
    // down. The newest session always lives in by_identity_.
    std::map<std::string, std::vector<std::shared_ptr<ConsoleSession>>> superseded_;
};

} // namespace tether::protocol
