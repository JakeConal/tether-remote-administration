// lib/protocol/session.cpp - session state machines (task 3.4). See
// session.hpp for the contract and wire conventions; logging follows D8
// (structured fields with session identity and seq context).
#include "session.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "logging.hpp"

namespace tether::protocol {

namespace {

std::vector<common::LogField> seq_fields(std::uint32_t seq) {
    std::vector<common::LogField> fields;
    fields.push_back(common::LogField{"seq", std::to_string(seq)});
    return fields;
}

void log_session(common::LogLevel level, const std::string& message, const std::string& identity,
                 std::vector<common::LogField> extra = {}) {
    std::vector<common::LogField> fields;
    fields.push_back(common::LogField{"session", identity});
    for (auto& field : extra) {
        fields.push_back(std::move(field));
    }
    common::Logger::instance().write(level, message, std::move(fields));
}

// Only FILE_META / FILE_CHUNK / RESULT_TEXT / FILE_END reach the result path;
// an inbound ERR is handled separately (it carries its own code).
bool is_terminal_result(const Message& piece) {
    return std::holds_alternative<ResultText>(piece) || std::holds_alternative<FileEnd>(piece);
}

const char* failure_reason_text(HandshakeFailure failure) {
    switch (failure) {
        case HandshakeFailure::NotBegun: return "answer arrived before any challenge";
        case HandshakeFailure::UnknownNonce: return "unknown nonce";
        case HandshakeFailure::NonceReplay: return "replayed nonce";
        case HandshakeFailure::BadHmac: return "malformed hmac answer";
        case HandshakeFailure::WrongToken: return "hmac does not verify with the provisioned token";
    }
    return "handshake failure";
}

// Shared inbound-drain loop over the transport seam: returns every frame
// currently complete (none is lost when EOF lands with complete frames in the
// same read window; the caller decides delivery order), plus the outcome.
enum class DrainOutcome { Ok, StreamError, EndOfStream };

DrainOutcome drain_inbound(IByteSource& source, FrameDecoder& decoder,
                           std::vector<Frame>& frames, std::string& error_text) {
    for (;;) {
        unsigned char buf[4096];
        const ReadResult result = source.read(buf, sizeof(buf));
        if (result.count != 0) {
            try {
                decoder.feed(buf, result.count, frames);
            } catch (const ProtocolError& e) {
                error_text = e.what();
                return DrainOutcome::StreamError;
            }
        }
        if (result.end_of_stream) {
            return DrainOutcome::EndOfStream;
        }
        if (result.count == 0) {
            return DrainOutcome::Ok; // wire drained (and not closed)
        }
    }
}

} // namespace

// ---- shared config/state names ---------------------------------------------

void SessionConfig::validate() const {
    if (command_timeout <= 0 || auth_timeout <= 0 || keepalive_interval <= 0 ||
        keepalive_window <= 0) {
        throw SessionError("session durations must be positive (command/auth/keepalive)");
    }
    if (keepalive_window < keepalive_interval) {
        throw SessionError("keepalive_window must be at least keepalive_interval");
    }
}

const char* session_state_name(SessionState state) {
    switch (state) {
        case SessionState::Connecting: return "connecting";
        case SessionState::Established: return "established";
        case SessionState::Stale: return "stale";
        case SessionState::Down: return "down";
    }
    return "unknown";
}

// ---- ConsoleSession ---------------------------------------------------------

ConsoleSession::ConsoleSession(IByteSource& source, IByteSink& sink, SessionConfig config,
                               TokenResolver resolve_token)
    : source_(source),
      sink_(sink),
      config_(config),
      resolve_token_(std::move(resolve_token)) {
    config_.validate();
    if (!resolve_token_) {
        throw SessionError("a token resolver is required on the console side");
    }
}

void ConsoleSession::pump(SessionMillis now) {
    if (state_ == SessionState::Down) {
        return; // dead: no dispatch, no timers, no anything
    }

    std::vector<Frame> frames;
    std::string error_text;
    const DrainOutcome outcome = drain_inbound(source_, decoder_, frames, error_text);
    if (outcome == DrainOutcome::StreamError) {
        go_down("stream error: " + error_text);
        return;
    }

    for (const Frame& frame : frames) {
        if (state_ == SessionState::Down) {
            break; // a violation mid-loop stops all further dispatch
        }
        try {
            handle_frame(frame, now);
        } catch (const ProtocolError& e) {
            go_down(std::string("payload handling error: ") + e.what());
            break;
        }
    }
    frames.clear();

    if (state_ == SessionState::Down) {
        return;
    }
    if (outcome == DrainOutcome::EndOfStream) {
        try {
            decoder_.finish();
        } catch (const ProtocolError&) {
            go_down("stream error: truncated frame at end of stream");
            return;
        }
        go_down("stream ended by peer (EOF)");
        return;
    }

    run_timers(now);
}

void ConsoleSession::handle_frame(const Frame& frame, SessionMillis now) {
    // Every valid inbound frame is a life sign: PONGs and traffic alike.
    last_seen_ = now;

    Message message;
    try {
        message = decode_payload(frame.type, frame.payload);
    } catch (const ProtocolError& e) {
        go_down(std::string("protocol violation: peer sent a malformed payload: ") + e.what());
        return;
    }

    if (state_ == SessionState::Connecting) {
        if (std::holds_alternative<Hello>(message)) {
            const Hello& hello = std::get<Hello>(message);
            identity_ = hello.agent_name;
            const std::string token = resolve_token_(hello.agent_name);
            if (token.empty()) {
                send_frame(MsgType::Err, 0,
                           encode_payload(Err{"auth_failed",
                                              "unknown agent identity '" + identity_ + "'"}));
                go_down("refused: unknown agent identity '" + identity_ + "'");
                return;
            }
            challenger_ = std::make_unique<Challenger>(token);
            const Challenge issued = challenger_->begin();
            send_frame(MsgType::Challenge, 0, encode_payload(issued));
            log_session(common::LogLevel::Info, "auth challenge issued", identity_,
                        {common::LogField{"step", "challenge"}});
            return;
        }
        if (std::holds_alternative<Challenge>(message)) {
            // The agent's answer re-uses CHALLENGE (flagged D4 overload).
            if (!challenger_) {
                go_down("protocol violation: challenge answer before HELLO");
                return;
            }
            AuthOk ok;
            const auto failure = challenger_->verify(std::get<Challenge>(message), ok);
            if (!failure.has_value()) {
                send_frame(MsgType::AuthOk, 0, encode_payload(AuthOk{}));
                state_ = SessionState::Established;
                last_ping_ = now; // first keepalive fires interval after setup
                log_session(common::LogLevel::Info, "auth ok", identity_);
                return;
            }
            send_frame(MsgType::Err, 0,
                       encode_payload(Err{"auth_failed", failure_reason_text(*failure)}));
            go_down(std::string("refused: ") + failure_reason_text(*failure));
            return;
        }
        if (std::holds_alternative<Err>(message)) {
            go_down("refused during handshake by peer: " + std::get<Err>(message).code);
            return;
        }
        go_down("protocol violation: unexpected " + std::string(msg_type_name(frame.type)) +
                " during handshake");
        return;
    }

    if (state_ == SessionState::Established) {
        if (frame.type == MsgType::ResultText || frame.type == MsgType::FileMeta ||
            frame.type == MsgType::FileChunk || frame.type == MsgType::FileEnd) {
            deliver_result(frame.seq, message, is_terminal_result(message));
            return;
        }
        if (frame.type == MsgType::Err) {
            const Err& error = std::get<Err>(message);
            if (frame.seq == 0) {
                // Connection-level complaint, attached to no command: it must
                // not corrupt command tracking.
                log_session(common::LogLevel::Warn, "session-level error from agent", identity_,
                            {common::LogField{"code", error.code}});
                return;
            }
            fail_command(frame.seq, error.code,
                         error.code == "cancelled" ? CommandStatus::Cancelled
                                                   : CommandStatus::Failed);
            return;
        }
        if (frame.type == MsgType::Ping) {
            send_frame(MsgType::Pong, 0, encode_payload(Pong{}));
            return;
        }
        if (frame.type == MsgType::Pong) {
            return; // life sign recorded at the top
        }
        if (frame.type == MsgType::Cancel) {
            // CANCEL is console -> agent; inbound is a peer bug, not fatal.
            log_session(common::LogLevel::Warn,
                        "inbound CANCEL ignored (only the console issues cancels)", identity_);
            return;
        }
        go_down("protocol violation: unexpected " + std::string(msg_type_name(frame.type)) +
                " while established");
        return;
    }

    if (state_ == SessionState::Stale) {
        // Superseded: ineligible for any work; inbound frames are noise.
        log_session(common::LogLevel::Debug, "inbound frame ignored (stale session)", identity_);
        return;
    }
}

std::uint32_t ConsoleSession::start(const Cmd& command, SessionMillis now, const ResultHook& hook,
                                    const CommandOptions& options) {
    if (state_ != SessionState::Established) {
        throw SessionError("cannot dispatch '" + command.command + "' while the session is " +
                           std::string(session_state_name(state_)));
    }
    const std::uint32_t seq = next_seq_++;
    Tracker tracker;
    tracker.status = CommandStatus::Pending;
    tracker.command = command.command;
    tracker.deadline = now + options.timeout.value_or(config_.command_timeout);
    tracker.hook = hook;
    send_frame(MsgType::Cmd, seq, encode_payload(command));
    outstanding_.emplace(seq, std::move(tracker));
    log_session(common::LogLevel::Info, "command dispatched", identity_, seq_fields(seq));
    return seq;
}

bool ConsoleSession::cancel(std::uint32_t seq) {
    if (state_ != SessionState::Established) {
        throw SessionError("cannot cancel while the session is " +
                           std::string(session_state_name(state_)));
    }
    if (outstanding_.find(seq) == outstanding_.end()) {
        log_session(common::LogLevel::Warn, "cancel ignored: no pending command", identity_,
                    seq_fields(seq));
        return false;
    }
    send_frame(MsgType::Cancel, seq, encode_payload(Cancel{}));
    log_session(common::LogLevel::Info, "cancel dispatched", identity_, seq_fields(seq));
    return true;
}

std::optional<CommandView> ConsoleSession::command(std::uint32_t seq) const {
    if (const auto it = outstanding_.find(seq); it != outstanding_.end()) {
        const Tracker& tracker = it->second;
        return CommandView{seq, tracker.status, tracker.command, tracker.pieces,
                           tracker.error_code};
    }
    if (const auto it = finished_.find(seq); it != finished_.end()) {
        const Tracker& tracker = it->second;
        return CommandView{seq, tracker.status, tracker.command, tracker.pieces,
                           tracker.error_code};
    }
    return std::nullopt;
}

std::vector<std::uint32_t> ConsoleSession::outstanding_seqs() const {
    std::vector<std::uint32_t> seqs;
    seqs.reserve(outstanding_.size());
    for (const auto& entry : outstanding_) {
        seqs.push_back(entry.first);
    }
    return seqs;
}

void ConsoleSession::mark_stale() {
    if (state_ == SessionState::Established) {
        state_ = SessionState::Stale;
        log_session(common::LogLevel::Info,
                    "session marked stale (superseded by a newer session)", identity_);
    }
}

void ConsoleSession::send_frame(MsgType type, std::uint32_t seq,
                                const std::vector<unsigned char>& payload) {
    const std::vector<unsigned char> wire = encode_frame(Frame{type, seq, payload});
    sink_.write(wire.data(), wire.size());
}

void ConsoleSession::deliver_result(std::uint32_t seq, const Message& piece, bool terminal) {
    auto it = outstanding_.find(seq);
    if (it == outstanding_.end()) {
        if (finished_.find(seq) != finished_.end()) {
            log_session(common::LogLevel::Debug, "late result ignored (command is terminal)",
                        identity_, seq_fields(seq));
        } else {
            log_session(common::LogLevel::Warn, "result ignored: unknown command seq", identity_,
                        seq_fields(seq));
        }
        return;
    }
    Tracker& tracker = it->second;
    tracker.pieces.push_back(piece);
    if (tracker.hook) {
        tracker.hook(seq, piece, terminal);
    }
    if (terminal) {
        tracker.status = CommandStatus::Completed;
        log_session(common::LogLevel::Info, "command completed", identity_, seq_fields(seq));
        finished_.emplace(seq, std::move(tracker));
        outstanding_.erase(it);
    }
}

void ConsoleSession::fail_command(std::uint32_t seq, const std::string& code,
                                  CommandStatus status) {
    auto it = outstanding_.find(seq);
    if (it == outstanding_.end()) {
        if (finished_.find(seq) != finished_.end()) {
            log_session(common::LogLevel::Debug, "late failure ignored (command is terminal)",
                        identity_, seq_fields(seq));
        }
        return; // unknown seqs are noise, never session damage
    }
    Tracker& tracker = it->second;
    tracker.pieces.push_back(Err{code, "command ended: " + code});
    tracker.status = status;
    tracker.error_code = code;
    if (tracker.hook) {
        tracker.hook(seq, tracker.pieces.back(), true);
    }
    log_session(common::LogLevel::Warn,
                status == CommandStatus::Cancelled ? "command cancelled" : "command failed",
                identity_, seq_fields(seq));
    finished_.emplace(seq, std::move(tracker));
    outstanding_.erase(it);
}

void ConsoleSession::run_timers(SessionMillis now) {
    if (state_ == SessionState::Connecting) {
        if (now - opened_at_ >= config_.auth_timeout) {
            send_frame(MsgType::Err, 0,
                       encode_payload(Err{"auth_failed", "handshake did not complete in time"}));
            go_down("refused: handshake did not complete within auth_timeout");
        }
        return;
    }
    if (state_ != SessionState::Established) {
        return; // Stale: timers frozen until re-registration
    }

    // 1) Per-command deadlines: a timeout marks exactly its own command
    // failed and leaves the session Established.
    std::vector<std::uint32_t> timed_out;
    for (const auto& entry : outstanding_) {
        if (now >= entry.second.deadline) {
            timed_out.push_back(entry.first);
        }
    }
    for (const std::uint32_t seq : timed_out) {
        fail_command(seq, "timeout", CommandStatus::Failed);
    }

    // 2) Keepalive cadence: one PING per configured interval.
    if (now - last_ping_ >= config_.keepalive_interval) {
        send_frame(MsgType::Ping, 0, encode_payload(Ping{}));
        last_ping_ = now;
        log_session(common::LogLevel::Debug, "keepalive ping sent", identity_);
    }

    // 3) Dead-peer window: no life sign for keepalive_window -> Down.
    if (now - last_seen_ >= config_.keepalive_window) {
        go_down("keepalive window elapsed without any life sign from the peer");
    }
}

void ConsoleSession::go_down(std::string reason) {
    if (state_ == SessionState::Down) {
        return;
    }
    state_ = SessionState::Down;
    log_session(common::LogLevel::Warn, "session down", identity_,
                {common::LogField{"reason", reason}});
}

// ---- AgentSession -----------------------------------------------------------

AgentSession::AgentSession(IByteSource& source, IByteSink& sink, AgentIdentity identity,
                           std::string token, SessionConfig config)
    : source_(source),
      sink_(sink),
      identity_(std::move(identity)),
      token_(std::move(token)),
      config_(config),
      responder_(std::make_unique<Responder>(token_)) {
    config_.validate();
}

void AgentSession::begin(SessionMillis now) {
    if (token_.empty()) {
        // config.hpp: empty-tokens are structurally loadable but a session is
        // where enforcement rejects them - never dial with no credential.
        throw SessionError("refusing to dial out with an empty provisioned token");
    }
    opened_at_ = now;
    last_seen_ = now;
    send_frame(MsgType::Hello, 0, encode_payload(Hello{kEnvelopeVersion, identity_.name, identity_.id}));
    log_session(common::LogLevel::Info, "dial-out initiated (HELLO sent)", identity_.name);
}

void AgentSession::pump(SessionMillis now) {
    if (state_ == SessionState::Down) {
        return;
    }

    std::vector<Frame> frames;
    std::string error_text;
    const DrainOutcome outcome = drain_inbound(source_, decoder_, frames, error_text);
    if (outcome == DrainOutcome::StreamError) {
        go_down("stream error: " + error_text);
        return;
    }

    for (const Frame& frame : frames) {
        if (state_ == SessionState::Down) {
            break;
        }
        try {
            handle_frame(frame, now);
        } catch (const ProtocolError& e) {
            go_down(std::string("payload handling error: ") + e.what());
            break;
        }
    }
    frames.clear();

    if (state_ == SessionState::Down) {
        return;
    }
    if (outcome == DrainOutcome::EndOfStream) {
        try {
            decoder_.finish();
        } catch (const ProtocolError&) {
            go_down("stream error: truncated frame at end of stream");
            return;
        }
        go_down("stream ended by peer (EOF): console is gone");
        return;
    }

    run_timers(now);
}

void AgentSession::handle_frame(const Frame& frame, SessionMillis now) {
    last_seen_ = now;

    Message message;
    try {
        message = decode_payload(frame.type, frame.payload);
    } catch (const ProtocolError& e) {
        go_down(std::string("protocol violation: peer sent a malformed payload: ") + e.what());
        return;
    }

    if (state_ == SessionState::Connecting) {
        if (std::holds_alternative<Challenge>(message)) {
            // Step 2 -> 3 of D4: compute the HMAC answer over the raw nonce.
            const Challenge answer =
                responder_->respond(std::get<Challenge>(message));
            send_frame(MsgType::Challenge, 0, encode_payload(answer));
            log_session(common::LogLevel::Info, "auth challenge answered", identity_.name,
                        {common::LogField{"step", "answer"}});
            return;
        }
        if (std::holds_alternative<AuthOk>(message)) {
            state_ = SessionState::Established;
            last_ping_ = now; // agent-side keepalive cadence starts here too
            log_session(common::LogLevel::Info, "auth ok (established)", identity_.name);
            return;
        }
        if (std::holds_alternative<Err>(message)) {
            go_down("refused by console during handshake: " + std::get<Err>(message).code);
            return;
        }
        go_down("protocol violation: unexpected " + std::string(msg_type_name(frame.type)) +
                " during handshake");
        return;
    }

    if (state_ == SessionState::Established) {
        if (frame.type == MsgType::Cmd) {
            if (frame.seq == 0) {
                go_down("protocol violation: CMD with seq 0");
                return;
            }
            auto flag = std::make_shared<bool>(false);
            in_flight_.emplace(frame.seq, InFlight{flag});
            const std::function<bool()> is_cancelled = [flag] { return *flag; };
            if (handler_) {
                handler_(frame.seq, std::get<Cmd>(message), is_cancelled);
            } else {
                send_result(
                    frame.seq,
                    Err{"no_handler", "agent has no command handler for '" +
                                          std::get<Cmd>(message).command + "'"});
            }
            return;
        }
        if (frame.type == MsgType::Cancel) {
            auto it = in_flight_.find(frame.seq);
            if (it == in_flight_.end()) {
                // Late cancel of an already-reported command: normal and ignored.
                log_session(common::LogLevel::Debug,
                            "cancel ignored (command not in flight)", identity_.name,
                            seq_fields(frame.seq));
                return;
            }
            *it->second.cancelled = true;
            in_flight_.erase(it);
            // The acknowledgement is NOT a send_result(): that API tracks
            // in-flight bookkeeping the cancel just closed; this is a direct
            // session reply (Err carries the target seq in the envelope).
            send_frame(MsgType::Err, frame.seq,
                       encode_payload(Err{"cancelled", "cancelled by the console while in flight"}));
            log_session(common::LogLevel::Info, "command cancelled by console", identity_.name,
                        seq_fields(frame.seq));
            return;
        }
        // Anything else inbound while established is a peer bug.
        if (frame.type == MsgType::Ping) {
            send_frame(MsgType::Pong, 0, encode_payload(Pong{}));
            return;
        }
        if (frame.type == MsgType::Pong) {
            return; // life sign recorded above
        }
        if (frame.type == MsgType::Err && frame.seq == 0) {
            log_session(common::LogLevel::Warn, "session-level error from console",
                        identity_.name,
                        {common::LogField{"code", std::get<Err>(message).code}});
            return;
        }
        go_down("protocol violation: unexpected " + std::string(msg_type_name(frame.type)) +
                " while established");
    }
}

void AgentSession::send_result(std::uint32_t seq, const Message& piece) {
    if (state_ != SessionState::Established) {
        throw SessionError("cannot report results while the session is " +
                           std::string(session_state_name(state_)));
    }
    const MsgType type = message_type_of(piece);
    if (type != MsgType::ResultText && type != MsgType::FileMeta && type != MsgType::FileChunk &&
        type != MsgType::FileEnd && type != MsgType::Err) {
        throw SessionError("agent -> console results carry RESULT_TEXT/FILE_*/ERR, not " +
                           std::string(msg_type_name(type)));
    }
    if (in_flight_.find(seq) == in_flight_.end()) {
        throw SessionError("no in-flight command with seq " + std::to_string(seq) +
                           " (already terminal or cancelled)");
    }
    send_frame(type, seq, encode_payload(piece));
    if (is_terminal_result(piece) || std::holds_alternative<Err>(piece)) {
        in_flight_.erase(seq);
    }
}

void AgentSession::send_frame(MsgType type, std::uint32_t seq,
                              const std::vector<unsigned char>& payload) {
    const std::vector<unsigned char> wire = encode_frame(Frame{type, seq, payload});
    sink_.write(wire.data(), wire.size());
}

void AgentSession::run_timers(SessionMillis now) {
    if (state_ == SessionState::Connecting) {
        if (now - opened_at_ >= config_.auth_timeout) {
            go_down("refused: console did not complete the handshake within auth_timeout");
        }
        return;
    }
    if (state_ != SessionState::Established) {
        return;
    }
    if (now - last_ping_ >= config_.keepalive_interval) {
        send_frame(MsgType::Ping, 0, encode_payload(Ping{}));
        last_ping_ = now;
        log_session(common::LogLevel::Debug, "keepalive ping sent", identity_.name);
    }
    if (now - last_seen_ >= config_.keepalive_window) {
        go_down("keepalive window elapsed without any life sign from the console");
    }
}

void AgentSession::go_down(std::string reason) {
    if (state_ == SessionState::Down) {
        return;
    }
    state_ = SessionState::Down;
    log_session(common::LogLevel::Warn, "session down", identity_.name,
                {common::LogField{"reason", reason}});
}

// ---- AgentRegistry ----------------------------------------------------------

bool AgentRegistry::register_session(const std::string& identity,
                                     const std::shared_ptr<ConsoleSession>& session) {
    bool superseded = false;
    const auto it = by_identity_.find(identity);
    if (it != by_identity_.end() && it->second->state() == SessionState::Established) {
        it->second->mark_stale();
        superseded = true;
    }
    by_identity_[identity] = session;
    return superseded;
}

std::shared_ptr<ConsoleSession> AgentRegistry::lookup(const std::string& identity) const {
    const auto it = by_identity_.find(identity);
    return it != by_identity_.end() ? it->second : nullptr;
}

} // namespace tether::protocol
