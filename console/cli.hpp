// console/cli.hpp - the operator verb surface (design D5, tasks 5.2-5.4):
// verb-style command lines, structured stderr errors, scriptable exit codes,
// streamed standard streams and confirmation gates for destructive commands.
//
// run_cli() is the dispatch entrypoint: it parses arguments (CLI11), resolves
// the verb against an IOperConsole supplied by the caller, and streams every
// result piece to the injected streams as it arrives. Tests drive it fully
// in-process with string streams and a fake console; the service main wires it
// to the real TCP transport stack.
//
// Stream rules (operator-console delta): result text and file bytes go to
// `streams.out` byte-exactly, progress/status/reporting goes to `streams.err`,
// and confirmation answers are read from `streams.in`. Nothing but result
// content is ever written to `out` - piped or redirected stdout stays clean.
//
// Exit codes (operator-console delta requires distinguishable non-success
// codes but fixes no numerics; this documented choice keeps every failure
// class scriptable):
//   0 success
//   1 runtime/command/transport failure (agent ERR, timeout, dispatch or
//     console setup failure)
//   2 usage failure: unknown verb, malformed args (nothing gets dispatched)
//   3 unknown agent, detected against the registry before any dispatch
//   4 unconfirmed destructive command refused before dispatch
#pragma once

#include "session.hpp"

#include <functional>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tether::cli {

inline constexpr int kExitOk = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitUnknownAgent = 3;
inline constexpr int kExitRefused = 4;

// The streams the CLI writes to. `stdout_is_tty`/`stdin_is_tty` describe the
// real execution environment: the interactive confirmation path runs only
// when stdin looks like a terminal, otherwise a destructive command without
// --yes is refused before dispatch.
struct CliStreams {
    std::ostream& out;      // result bytes (stdout)
    std::ostream& err;      // status/progress/errors (stderr)
    std::istream& in;       // interactive confirmation answers (stdin)
    bool stdout_is_tty = true;
    bool stdin_is_tty = false;
};

// Why a cancel dispatch resolved the way it did (the CLI maps outcomes to
// exit codes 0/1; "unknown agent" is pre-checked by the verb layer and never
// leaves this interface as an option).
enum class CancelOutcome { Sent, NotPending, NotEligible };

// The operator-side command surface. Production backs this with the TCP
// console transport (tcp_console.hpp); tests inject fakes. `dispatch` issues
// the command to the named agent and drives the session stack until the
// command is terminal, delivering every result piece through `hook` in wire
// order; it returns the terminal view (transport loss yields std::nullopt or
// throws TransportError/SessionError - the verb layer reports either as exit
// code 1 with the error on stderr).
class IOperConsole {
public:
    virtual ~IOperConsole() = default;

    virtual std::vector<protocol::RegistryEntry> list_agents() = 0;
    virtual bool has_agent(const std::string& identity) = 0;
    virtual std::optional<protocol::CommandView> dispatch(
        const std::string& agent, const protocol::Cmd& command,
        const protocol::ResultHook& hook) = 0;
    virtual CancelOutcome cancel(const std::string& agent, std::uint32_t seq) = 0;
};

// Console construction failure: the verb surface reports it on stderr with
// the machine-parseable `error:` prefix and exit code 1.
class CliSetupError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Builds (and, for the real backend, starts) the console for `config_path`.
// Called lazily AFTER verb parsing, so unknown verbs fail without ever
// touching configuration or the registry.
using ConsoleFactory =
    std::function<std::unique_ptr<IOperConsole>(const std::string& config_path)>;

// Destructive commands subject to the confirmation gate (operator-console
// delta: shutdown, restart, and file deletion). These match the protocol
// command words the remote-commands delta names for those families;
// if a Phase 6 command rename shifts them, this set is what moves.
bool is_destructive_command(const std::string& command);

// The verb dispatch entrypoint. `arguments` is the argv-style token list
// INCLUDING the program name (arguments[0] is skipped); the verb surface is:
//   agents
//   exec <agent> <command> [args...] [--yes]
//   cancel <agent> <seq>
int run_cli(const std::vector<std::string>& arguments, CliStreams streams,
            const ConsoleFactory& make_console);

// argc/argv convenience wrapper.
int run_cli(int argc, const char* const argv[], CliStreams streams,
            const ConsoleFactory& make_console);

} // namespace tether::cli
