// agent/command_handler.cpp - the dispatcher->session adapter (see
// command_handler.hpp for the layer's contract). The heavy lifting is the
// dispatcher's; everything below is placement and safety: piece shaping,
// the oversized-result file channel, and never throwing out of pump().
#include "command_handler.hpp"

#include "logging.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <string>

namespace tether::agent {

namespace {

inline constexpr std::size_t kMaxInlineTextBytes = 128 * 1024; // single RESULT_TEXT ceiling
inline constexpr std::size_t kFileChunkBytes    = 128 * 1024;  // FILE_CHUNK slice size

std::string joined_lines(const std::vector<std::string>& lines) {
    if (lines.empty()) {
        return {};
    }
    std::string text;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            text += '\n';
        }
        text += lines[i];
    }
    text += '\n'; // listings end newline-terminated for clean stdout output
    return text;
}

} // namespace

std::vector<protocol::Message> outcome_to_pieces(const syscalls::Outcome& outcome) {
    if (!outcome.ok) {
        // Exactly one terminal piece: the coded failure, message intact.
        return {protocol::Err{outcome.code, outcome.message}};
    }
    if (!outcome.file.has_value()) {
        const std::string text = joined_lines(outcome.lines);
        if (!text.empty() && text.size() <= kMaxInlineTextBytes) {
            return {protocol::ResultText{text}};
        }
        // Oversized listing or an empty one (an empty RESULT_TEXT is
        // rejected at decode and would poison the session): both ride the
        // file stream below with a synthesized "result.txt" deliverable.
    }
    const std::string delivered_text = joined_lines(outcome.lines);
    // Copy (not bind-a-temporary) so both branches share one type cleanly.
    const syscalls::Outcome::Deliverable deliverable = outcome.file.has_value()
        ? *outcome.file
        : syscalls::Outcome::Deliverable{
              "result.txt",
              std::vector<unsigned char>(delivered_text.begin(), delivered_text.end())};
    std::vector<protocol::Message> pieces;
    pieces.push_back(protocol::FileMeta{
        deliverable.name, static_cast<std::uint64_t>(deliverable.bytes.size())});
    for (std::size_t offset = 0; offset < deliverable.bytes.size(); offset += kFileChunkBytes) {
        const std::size_t count =
            std::min(kFileChunkBytes, deliverable.bytes.size() - offset);
        const auto begin =
            deliverable.bytes.begin() + static_cast<std::ptrdiff_t>(offset);
        const auto end =
            deliverable.bytes.begin() + static_cast<std::ptrdiff_t>(offset + count);
        pieces.emplace_back(protocol::FileChunk{std::vector<unsigned char>(begin, end)});
    }
    pieces.emplace_back(protocol::FileEnd{});
    return pieces;
}

namespace {

// Shared handler body for both install overloads: `send` binds the owning
// surface's send_result (session or link). Runs synchronously inside the
// owning session's pump(); every send failure degrades to a log instead of
// escaping (the CANCEL-ack race is a normal session event, not a crash).
void dispatch_and_reply(syscalls::CommandDispatcher& dispatcher,
                        const std::function<void(const protocol::Message&)>& send,
                        std::uint32_t seq, const protocol::Cmd& command,
                        const std::function<bool()>& is_cancelled) {
    if (is_cancelled()) {
        return; // the session already acked the CANCEL; sending is a handler bug
    }
    syscalls::Outcome outcome;
    try {
        outcome = dispatcher.dispatch(command.command, command.args);
    } catch (const std::exception& error) {
        // Defensive: the dispatcher classifies internally; this is the belt
        // for anything it did not anticipate.
        outcome.ok = false;
        outcome.code = "internal_error";
        outcome.message = error.what();
    }
    if (is_cancelled()) {
        return;
    }
    for (const protocol::Message& piece : outcome_to_pieces(outcome)) {
        try {
            send(piece);
        } catch (const protocol::SessionError& error) {
            // A CANCEL completed this seq mid-send (normal race): drop the
            // rest instead of wedging the pump loop.
            common::Logger::instance().warn(
                "result dropped after cancel",
                {common::LogField{"seq", std::to_string(seq)},
                 common::LogField{"detail", error.what()}});
            return;
        }
    }
}

} // namespace

void install_commands(syscalls::CommandDispatcher& dispatcher, protocol::AgentSession& session) {
    // The captured `session` is the handler's own - used only during that
    // session's pump() invocations.
    session.set_command_handler(
        [&dispatcher, &session](std::uint32_t seq, const protocol::Cmd& command,
                                const std::function<bool()>& is_cancelled) {
            dispatch_and_reply(dispatcher,
                               [&session, seq](const protocol::Message& piece) {
                                   session.send_result(seq, piece);
                               },
                               seq, command, is_cancelled);
        });
}

void install_commands(syscalls::CommandDispatcher& dispatcher,
                      tether::transport::TcpAgentLink& link) {
    // The link forwards set_command_handler to its owned AgentSession, and
    // send_result likewise - the same flow one level up.
    link.set_command_handler(
        [&dispatcher, &link](std::uint32_t seq, const protocol::Cmd& command,
                             const std::function<bool()>& is_cancelled) {
            dispatch_and_reply(dispatcher,
                               [&link, seq](const protocol::Message& piece) {
                                   link.send_result(seq, piece);
                               },
                               seq, command, is_cancelled);
        });
}

} // namespace tether::agent
