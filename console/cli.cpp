// console/cli.cpp - verb parsing, dispatch and streaming for the operator
// surface (tasks 5.2-5.4). See cli.hpp for the contract: exact verb names,
// exit codes, stream discipline and the confirmation gate.
//
// Guard rails encoded here:
//  - unknown verb fail before anything is constructed or dispatched;
//  - unknown agent fail against the registry before any dispatch;
//  - destructive commands are confirmed (--yes or interactive y/N) BEFORE
//    dispatch; a non-terminal stdin without --yes is refused locally with
//    nothing sent;
//  - all errors print `error: ...` on stderr; progress prints `status: ...`
//    on stderr; stdout carries result content only.
#include "cli.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <sstream>
#include <utility>

namespace tether::cli {

namespace {

// Confirmable commands ride `exec` as command words (operator-console delta:
// "Shutdown, restart, and file-deletion commands"; remote-commands delta
// names the families shutdown/restart and the file family list/delete/move/
// copy, whence `delete` as the deletion command word).
constexpr const char* kDestructiveCommands[] = {"shutdown", "restart", "delete"};

// ---- audit helpers (task 6.9) -------------------------------------------------

// Wall-clock stamp for audit records (audit time fields are wall time; the
// session/transport clocks inside a run stay monotonic).
std::int64_t wall_now_ms() {
    return std::int64_t{std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count()};
}

// Every audit write funnels through here: append failures fail the invocation
// loudly via CliSetupError (structured `error: ...` output, exit code 1) -
// never a silently skipped record.
void audit_append(AuditStore& store, const AuditRecord& record) {
    try {
        store.append(record);
    } catch (const AuditStoreError& error) {
        throw CliSetupError(std::string("audit write failed: ") + error.what());
    }
}

// One command record (record_type "command") with the invocation's wall time.
void record_command_attempt(const std::shared_ptr<AuditStore>& audit, const std::string& agent,
                            const std::string& command,
                            const std::vector<std::string>& parameters, std::uint32_t seq,
                            const std::string& outcome, const std::string& detail) {
    if (!audit) {
        return; // audit disabled (no configured storage): nothing to write
    }
    const std::int64_t now = wall_now_ms();
    AuditRecord record;
    record.time = format_audit_time_iso8601(now);
    record.time_epoch_ms = now;
    record.agent = agent;
    record.command = command;
    record.parameters = parameters;
    record.seq = seq;
    record.outcome = outcome;
    record.record_type = "command";
    record.detail = detail;
    audit_append(*audit, record);
}

// One background-job lifecycle record (record_type "job_event") referencing
// the initiating / finalizing command through its agent/command/parameters/
// seq identity fields.
void record_job_event(const std::shared_ptr<AuditStore>& audit, const std::string& agent,
                      const std::string& command, const std::vector<std::string>& parameters,
                      std::uint32_t seq, const std::string& event, const std::string& job,
                      const std::string& delivered) {
    if (!audit) {
        return;
    }
    const std::int64_t now = wall_now_ms();
    AuditRecord record;
    record.time = format_audit_time_iso8601(now);
    record.time_epoch_ms = now;
    record.agent = agent;
    record.command = command;
    record.parameters = parameters;
    record.seq = seq;
    record.outcome = "executed"; // the lifecycle event itself completed
    record.record_type = "job_event";
    record.event = event;
    record.job = job;
    record.delivered = delivered;
    audit_append(*audit, record);
}

// The four background-job lifecycle verbs (camera family + input family) map
// to their registry slot and the event whose evidence arrives on the result
// channel. Start events ride the RESULT-JSON convention the input family
// embeds (see parse_job_event_line); stop events travel as file-only streams
// (a delivered capture has no terminal RESULT text), so the verb + the
// FILE_META name is the fallback evidence the audit records.
struct VerbJobEvent {
    const char* verb;
    const char* job;
    const char* event;
};
constexpr std::array<VerbJobEvent, 4> kJobVerbEvents{{
    {"start_webcam", "camera", "started"},
    {"stop_webcam", "camera", "stopped"},
    {"start_keylogger", "input", "started"},
    {"stop_keylogger", "input", "stopped"},
}};

const VerbJobEvent* find_verb_job_event(const std::string& verb) {
    for (const VerbJobEvent& entry : kJobVerbEvents) {
        if (verb == entry.verb) {
            return &entry;
        }
    }
    return nullptr;
}

bool is_yes_answer(const std::string& line) {
    std::string trimmed;
    for (const char c : line) {
        if (c == ' ' || c == '\t' || c == '\r') {
            continue;
        }
        trimmed.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return trimmed == "y" || trimmed == "yes";
}

// Streams result pieces to the injected streams in wire order, tracking file
// delivery integrity (streams delta: a delivery that cannot complete reports
// a failure rather than looking like a truncated success).
struct ResultStreamer {
    std::ostream& out;
    std::ostream& err;
    bool delivery_broken = false;
    std::uint64_t expected_bytes = 0;
    std::uint64_t received_bytes = 0;

    void feed(const protocol::Message& piece) {
        if (const auto* text = std::get_if<protocol::ResultText>(&piece)) {
            out << text->text;
            return;
        }
        if (const auto* meta = std::get_if<protocol::FileMeta>(&piece)) {
            expected_bytes = meta->size;
            received_bytes = 0;
            err << "status: file '" << meta->name << "' (" << meta->size << " bytes)\n";
            return;
        }
        if (const auto* chunk = std::get_if<protocol::FileChunk>(&piece)) {
            out.write(reinterpret_cast<const char*>(chunk->bytes.data()),
                      static_cast<std::streamsize>(chunk->bytes.size()));
            received_bytes += chunk->bytes.size();
            return;
        }
        if (std::holds_alternative<protocol::FileEnd>(piece)) {
            if (expected_bytes != received_bytes) {
                err << "error: file delivery incomplete: expected " << expected_bytes
                    << " bytes, received " << received_bytes << "\n";
                delivery_broken = true;
            } else {
                err << "status: file received (" << received_bytes << " bytes)\n";
            }
            expected_bytes = 0;
            received_bytes = 0;
            return;
        }
        // Err pieces flow through the post-dispatch failure path below.
    }
};

void emit_unknown_verb(std::ostream& err, const std::string& verb) {
    err << "error: unknown verb '" << verb << "'\n";
    err << "status: verbs: agents | exec <agent> <command> [args] | cancel <agent> <seq>\n";
}

void emit_unknown_agent(std::ostream& err, const std::string& agent) {
    err << "error: unknown agent '" << agent << "'\n";
}

void emit_refusal(std::ostream& err, const std::string& agent, const std::string& command) {
    err << "error: refusal: '" << command << "' on '" << agent
        << "' is destructive and was not confirmed (nothing was sent)\n";
}

// Per-verb argument parsing (CLI11): returns kExitUsage on a malformed tail,
// and fills `extras` with the tokens CLI11 leaves over - including tokens that
// look like options (they are command arguments: data, never CLI flags).
// CLI11's vector overload consumes its argument stack-first, so parsing rides
// an argv-style array converted from the token list.
bool parse_tail(CLI::App& app, const std::vector<std::string>& tail, CliStreams streams,
                const char* usage, std::vector<std::string>& extras) {
    // CLI11's argv overload discards argv[0] as the program name; a verb tail
    // has none, so prefix a placeholder.
    std::vector<const char*> argv;
    argv.reserve(tail.size() + 1);
    static const char* kProgramNamePlaceholder = "tether";
    argv.push_back(kProgramNamePlaceholder);
    for (const std::string& token : tail) {
        argv.push_back(token.c_str());
    }
    try {
        app.parse(static_cast<int>(argv.size()), argv.data());
    } catch (const CLI::ParseError& e) {
        streams.err << "error: usage: " << usage << " (" << e.what() << ")\n";
        return false;
    }
    extras = app.remaining();
    return true;
}

// The console construction point of every verb that talks to the registry.
IOperConsole& ensure_console(std::unique_ptr<IOperConsole>& console,
                             const ConsoleFactory& make_console, const std::string& config_path) {
    if (!console) {
        try {
            console = make_console(config_path);
        } catch (const std::exception& error) {
            throw CliSetupError(std::string("console setup failed: ") + error.what());
        }
        if (!console) {
            throw CliSetupError("console setup failed: no console was constructed");
        }
    }
    return *console;
}

int run_agents(const std::vector<std::string>& tail, CliStreams streams,
               std::unique_ptr<IOperConsole>& console, const ConsoleFactory& make_console,
               const std::string& config_path) {
    CLI::App app{"list agents with their session state"};
    app.allow_extras();
    std::vector<std::string> extras;
    if (!parse_tail(app, tail, streams, "agents", extras)) {
        return kExitUsage;
    }
    if (!extras.empty()) {
        streams.err << "error: usage: 'agents' takes no arguments\n";
        return kExitUsage;
    }

    IOperConsole& console_ref = ensure_console(console, make_console, config_path);
    for (const protocol::RegistryEntry& entry : console_ref.list_agents()) {
        streams.out << entry.identity << " " << registry_state_name(entry.state) << "\n";
    }
    streams.out.flush();
    return kExitOk;
}

int run_exec(const std::vector<std::string>& tail, CliStreams streams,
             std::unique_ptr<IOperConsole>& console, const ConsoleFactory& make_console,
             const std::string& config_path, const std::shared_ptr<AuditStore>& audit) {
    std::string agent;
    std::string command;
    bool confirmed = false;
    CLI::App app{"exec <agent> <command> [args]"};
    app.add_option("agent", agent, "target agent identity");
    app.add_option("command", command, "command to execute on the agent");
    app.add_flag("--yes", confirmed, "confirm a destructive command");
    app.allow_extras(); // command arguments are data, not CLI options

    std::vector<std::string> args;
    if (!parse_tail(app, tail, streams, "exec <agent> <command> [args]", args)) {
        return kExitUsage;
    }
    if (agent.empty() || command.empty()) {
        streams.err << "error: usage: exec <agent> <command> [args]\n";
        return kExitUsage;
    }

    IOperConsole& console_ref = ensure_console(console, make_console, config_path);

    // Distinguishable from usage errors and unknown verbs, and pre-dispatch.
    if (!console_ref.has_agent(agent)) {
        record_command_attempt(audit, agent, command, args, 0, "refused",
                               "unknown agent '" + agent + "'");
        emit_unknown_agent(streams.err, agent);
        return kExitUnknownAgent;
    }

    // 5.4: the confirmation gate runs BEFORE dispatch; nothing reaches the
    // agent unless the operator confirmed. The refusal is one of the
    // audit-trail's explicit outcomes (nothing was dispatched: seq 0).
    if (is_destructive_command(command) && !confirmed) {
        if (!streams.stdin_is_tty) {
            record_command_attempt(audit, agent, command, args, 0, "refused",
                                   "destructive command refused: not confirmed (nothing was sent)");
            emit_refusal(streams.err, agent, command);
            return kExitRefused;
        }
        streams.err << "status: '" << command << "' targets '" << agent
                    << "'; confirm? [y/N] " << std::flush;
        std::string answer;
        if (!std::getline(streams.in, answer) || !is_yes_answer(answer)) {
            record_command_attempt(audit, agent, command, args, 0, "refused",
                                   "destructive command refused: not confirmed (nothing was sent)");
            emit_refusal(streams.err, agent, command);
            return kExitRefused;
        }
    }

    ResultStreamer streamer{streams.out, streams.err};
    // 6.9 hook-side evidence: the delivered capture's name (stop events) and
    // the machine-readable job event the agent embedded in its RESULT text
    // (start events; the frozen RESULT-JSON convention).
    std::string stop_delivery_name;
    bool job_event_recorded = false;
    const protocol::ResultHook hook = [&](std::uint32_t seq, const protocol::Message& piece,
                                          bool) {
        streamer.feed(piece);
        if (!audit) {
            return;
        }
        if (const auto* text = std::get_if<protocol::ResultText>(&piece)) {
            if (const auto job_event = parse_job_event_line(text->text)) {
                record_job_event(audit, agent, command, args, seq, job_event->event,
                                 job_event->job, "");
                job_event_recorded = true;
                return;
            }
        }
        if (const auto* meta = std::get_if<protocol::FileMeta>(&piece)) {
            stop_delivery_name = meta->name;
        }
    };

    std::optional<protocol::CommandView> view;
    try {
        view = console_ref.dispatch(agent, protocol::Cmd{command, args}, hook);
    } catch (const std::exception& error) {
        streams.out.flush();
        record_command_attempt(audit, agent, command, args, 0, "failed",
                               std::string("dispatch failed: ") + error.what());
        streams.err << "error: dispatch failed: " << error.what() << "\n";
        return kExitFailure;
    }
    streams.out.flush();

    if (!view.has_value()) {
        record_command_attempt(audit, agent, command, args, 0, "failed", "agent session lost");
        streams.err << "error: command outcome unavailable (agent session lost)\n";
        return kExitFailure;
    }
    switch (view->status) {
        case protocol::CommandStatus::Completed:
            if (streamer.delivery_broken) {
                record_command_attempt(audit, agent, command, args, view->seq, "failed",
                                       "file delivery incomplete");
                streams.err << "error: file delivery incomplete\n";
                return kExitFailure;
            }
            // 6.9: the verb-table fallback for job lifecycle events the
            // RESULT text did not already record (start results without the
            // JSON event; stop results that travel as file-only streams).
            if (const VerbJobEvent* mapped = find_verb_job_event(command)) {
                const bool started_by_table =
                    std::string(mapped->event) == "started" && !job_event_recorded;
                const bool stopped_by_table =
                    std::string(mapped->event) == "stopped" && !stop_delivery_name.empty();
                if (started_by_table || stopped_by_table) {
                    record_job_event(audit, agent, command, args, view->seq, mapped->event,
                                     mapped->job,
                                     stopped_by_table ? stop_delivery_name : std::string{});
                }
            }
            record_command_attempt(audit, agent, command, args, view->seq, "executed", "");
            return kExitOk; // stdout already carries the result bytes only
        case protocol::CommandStatus::Failed: {
            // Failure details never flood the result channel: the error line
            // rides stderr, stdout stays empty or result-only. The audit
            // outcome classifies the agent's coded refusal vs failure (6.9).
            std::string detail;
            for (auto piece = view->pieces.rbegin(); piece != view->pieces.rend(); ++piece) {
                if (const auto* err = std::get_if<protocol::Err>(&*piece)) {
                    detail = err->message;
                    break;
                }
            }
            record_command_attempt(audit, agent, command, args, view->seq,
                                   classify_command_outcome(view->error_code),
                                   detail.empty() ? view->error_code
                                                  : view->error_code + ": " + detail);
            streams.err << "error: command failed: " << view->error_code;
            if (!detail.empty()) {
                streams.err << ": " << detail;
            }
            streams.err << "\n";
            return kExitFailure;
        }
        case protocol::CommandStatus::Cancelled:
            record_command_attempt(audit, agent, command, args, view->seq, "failed",
                                   "command cancelled");
            streams.err << "error: command cancelled (seq " << view->seq << ")\n";
            return kExitFailure;
        case protocol::CommandStatus::Pending:
            record_command_attempt(audit, agent, command, args, view->seq, "failed",
                                   "command never reached a terminal outcome");
            streams.err << "error: command never reached a terminal outcome\n";
            return kExitFailure;
    }
    record_command_attempt(audit, agent, command, args, view->seq, "failed",
                           "command ended with an unclassified outcome");
    streams.err << "error: command ended with an unclassified outcome\n";
    return kExitFailure;
}

int run_cancel(const std::vector<std::string>& tail, CliStreams streams,
               std::unique_ptr<IOperConsole>& console, const ConsoleFactory& make_console,
               const std::string& config_path) {
    std::string agent;
    std::string seq_token;
    CLI::App app{"cancel <agent> <seq>"};
    app.add_option("agent", agent, "target agent identity");
    app.add_option("seq", seq_token, "in-flight command sequence");
    app.allow_extras();

    std::vector<std::string> extras;
    if (!parse_tail(app, tail, streams, "cancel <agent> <seq>", extras)) {
        return kExitUsage;
    }
    if (agent.empty() || seq_token.empty() || !extras.empty()) {
        streams.err << "error: usage: cancel <agent> <seq>\n";
        return kExitUsage;
    }
    std::uint32_t seq = 0;
    try {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(seq_token, &consumed);
        if (consumed != seq_token.size() || parsed > 0xFFFFFFFFul) {
            throw std::invalid_argument(seq_token);
        }
        seq = static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        streams.err << "error: usage: seq must be a non-negative integer, got '"
                    << seq_token << "'\n";
        return kExitUsage;
    }

    IOperConsole& console_ref = ensure_console(console, make_console, config_path);
    if (!console_ref.has_agent(agent)) {
        emit_unknown_agent(streams.err, agent);
        return kExitUnknownAgent;
    }

    switch (console_ref.cancel(agent, seq)) {
        case CancelOutcome::Sent:
            streams.err << "status: cancel sent for seq " << seq << "\n";
            return kExitOk;
        case CancelOutcome::NotPending:
            streams.err << "error: no in-flight command with seq " << seq << " on '"
                        << agent << "'\n";
            return kExitFailure;
        case CancelOutcome::NotEligible:
            streams.err << "error: agent '" << agent
                        << "' cannot be cancelled right now (session not eligible)\n";
            return kExitFailure;
    }
    streams.err << "error: cancel ended with an unclassified outcome\n";
    return kExitFailure;
}

} // namespace

bool is_destructive_command(const std::string& command) {
    return std::find(std::begin(kDestructiveCommands), std::end(kDestructiveCommands),
                     command) != std::end(kDestructiveCommands);
}

int run_cli(const std::vector<std::string>& arguments, CliStreams streams,
            const ConsoleFactory& make_console, const AuditStoreFactory& make_audit) {
    // Stage 1: global options (--config). Unknown tokens pass through so the
    // verb layer fully owns everything below argv[0]; an unknown verb therefore
    // fails before any console is constructed or configured.
    std::string config_path = "console.json";
    std::vector<std::string> verb_tokens;
    {
        // CLI11's argv overload already treats token[0] as the program name.
        std::vector<std::string> parse_tokens(arguments.begin(), arguments.end());
        CLI::App app{"tether operator console"};
        app.add_option("--config", config_path, "console config file");
        app.allow_extras();
        std::vector<const char*> argv;
        argv.reserve(parse_tokens.size());
        for (const std::string& token : parse_tokens) {
            argv.push_back(token.c_str());
        }
        try {
            app.parse(static_cast<int>(argv.size()), argv.data());
        } catch (const CLI::ParseError& e) {
            streams.err << "error: usage: " << e.what() << "\n";
            return kExitUsage;
        }
        verb_tokens = app.remaining();
    }
    if (verb_tokens.empty()) {
        streams.err << "error: no verb given (verbs: agents | exec <agent> <command> [args] | "
                       "cancel <agent> <seq>)\n";
        return kExitUsage;
    }

    const std::string verb = verb_tokens.front();
    const std::vector<std::string> tail(verb_tokens.begin() + 1, verb_tokens.end());

    // Unknown verbs stay side-effect-free: nothing is constructed, audited,
    // or configured (the usage check guards the audit construction below).
    bool known_verb = false;
    static const char* kKnownVerbs[] = {"agents", "exec", "cancel"};
    for (const char* known : kKnownVerbs) {
        if (verb == known) {
            known_verb = true;
            break;
        }
    }
    if (!known_verb) {
        emit_unknown_verb(streams.err, verb);
        return kExitUsage;
    }

    // 6.9: the invocation's audit trail, built once the config path is
    // known. A null store disables auditing (absent/empty `audit_path`);
    // factory failures fail the invocation before anything dispatches.
    std::shared_ptr<AuditStore> audit;
    if (make_audit) {
        try {
            audit = make_audit(config_path);
        } catch (const std::exception& error) {
            streams.err << "error: audit setup failed: " << error.what() << "\n";
            return kExitFailure;
        }
    }

    std::unique_ptr<IOperConsole> console;
    const auto setup_guard = [&](auto&& body) {
        try {
            return body();
        } catch (const CliSetupError& error) {
            streams.err << "error: " << error.what() << "\n";
            return kExitFailure;
        }
    };

    // The dispatch table covers every verb the spec names. Anything else is
    // a usage error before any dispatch (spec scenario: unknown verb).
    if (verb == "agents") {
        return setup_guard(
            [&] { return run_agents(tail, streams, console, make_console, config_path); });
    }
    if (verb == "exec") {
        return setup_guard(
            [&] { return run_exec(tail, streams, console, make_console, config_path, audit); });
    }
    if (verb == "cancel") {
        return setup_guard(
            [&] { return run_cancel(tail, streams, console, make_console, config_path); });
    }
    emit_unknown_verb(streams.err, verb);
    return kExitUsage;
}

int run_cli(int argc, const char* const argv[], CliStreams streams,
            const ConsoleFactory& make_console, const AuditStoreFactory& make_audit) {
    return run_cli(std::vector<std::string>(argv, argv + argc), std::move(streams),
                   make_console, make_audit);
}

} // namespace tether::cli
