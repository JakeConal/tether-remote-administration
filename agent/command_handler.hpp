// agent/command_handler.hpp - the agent-side command surface (tasks 6.1-6.3,
// binding: "wire the real dispatcher behind the session now for process/
// file; future families route through the same dispatcher"). This adapter
// sits between the session layer's CommandHandler seam and the syscalls
// Capability matrix:
//
//   protocol::Cmd -> syscalls::CommandDispatcher::dispatch -> Outcome ->
//   result pieces over the live session (RESULT_TEXT terminal, or the
//   FILE_META/FILE_CHUNK/FILE_END stream for oversized/byte payloads, or one
//   ERR terminal piece for failures).
//
// Guarantee shape (remote-commands delta):
//  - every command resolves to a definitive outcome: the dispatcher's
//    classification chain ends in success or a coded failure, and the
//    adapter always emits exactly one terminal piece;
//  - refusals (unknown verb, unsupported family, missing confirmation) reach
//    the capabilities as a structured ERR before anything runs, so the
//    session stays usable and no system effect happens;
//  - the handler never throws out of pump(): send races with a CANCEL are
//    recognised and dropped with a warn log instead of wedging the session.
#pragma once

#include "matrix.hpp"
#include "session.hpp"
#include "tcp.hpp"

#include <vector>

namespace tether::agent {

// Renders a dispatcher Outcome as the wire pieces the session layer sends:
//  - failure: one ERR piece carrying the outcome's code + message;
//  - success with text at or under 128 KiB: one terminal RESULT_TEXT piece
//    (lines joined, newline-terminated);
//  - success with larger text or an attached deliverable: the file stream -
//    FILE_META{name,size} + FILE_CHUNKs (128 KiB slices) + FILE_END. Empty
//    listings ride this channel too (an empty RESULT_TEXT is rejected at
//    decode and would poison the session).
std::vector<protocol::Message> outcome_to_pieces(const syscalls::Outcome& outcome);

// Installs dispatch as `session`'s command handler. `dispatcher` must outlive
// the session (the agent main keeps one across redials; tests scope them).
void install_commands(syscalls::CommandDispatcher& dispatcher, protocol::AgentSession& session);

// Same, for the TCP agent link (forwards to the link's owned AgentSession).
void install_commands(syscalls::CommandDispatcher& dispatcher, tether::transport::TcpAgentLink& link);

} // namespace tether::agent
