// tests/cli_verb_test.cpp - task 5.2: verb parsing and dispatch through
// run_cli() in-process with injected streams and a fake console (no sockets,
// no real time). Scenarios map to the verb-surface requirement and the
// scriptable exit-code delta:
//  - unknown verb fails with a structured stderr error and exit code 2 BEFORE
//    any console is constructed or dispatched
//  - unknown agent fails with exit code 3 and nothing dispatched
//  - `agents` lists identities with states on stdout (result channel only)
//  - exec/cancel dispatch via the console surface; command arguments arrive
//    as argv data; failure details go to stderr, exit 1
//  - the outcome classes scripts react to (success/failure/usage/unknown
//    agent) are distinguishable by exit code
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cli.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace tc = tether::cli;
namespace tp = tether::protocol;

namespace {

// A recording fake: nothing goes anywhere near a real session.
struct FakeConsole final : tc::IOperConsole {
    std::vector<tp::RegistryEntry> rows;
    int dispatch_calls = 0;
    int cancel_calls = 0;
    std::vector<std::pair<std::string, tp::Cmd>> dispatch_log;

    // Canned exec outcome: pieces stream through the hook first, then the
    // terminal view returns - mirroring the real session's flow.
    std::optional<tp::CommandView> canned_view;
    std::vector<tp::Message> canned_pieces;
    tc::CancelOutcome canned_cancel = tc::CancelOutcome::Sent;

    std::vector<tp::RegistryEntry> list_agents() override { return rows; }
    bool has_agent(const std::string& identity) override {
        for (const tp::RegistryEntry& row : rows) {
            if (row.identity == identity) {
                return true;
            }
        }
        return false;
    }
    std::optional<tp::CommandView> dispatch(const std::string& agent, const tp::Cmd& command,
                                            const tp::ResultHook& hook) override {
        ++dispatch_calls;
        dispatch_log.emplace_back(agent, command);
        for (const tp::Message& piece : canned_pieces) {
            hook(7, piece, false);
        }
        return canned_view;
    }
    tc::CancelOutcome cancel(const std::string&, std::uint32_t) override {
        ++cancel_calls;
        return canned_cancel;
    }

    tp::CommandView completed_view() {
        tp::CommandView view;
        view.seq = 7;
        view.status = tp::CommandStatus::Completed;
        view.command = "ls";
        return view;
    }
};

// run_cli owns its console through a factory; this wrapper keeps the
// recording state in the test-visible FakeConsole.
struct RecordingRef final : tc::IOperConsole {
    explicit RecordingRef(FakeConsole& target_) : target(&target_) {}

    FakeConsole* target = nullptr;

    std::vector<tp::RegistryEntry> list_agents() override { return target->list_agents(); }
    bool has_agent(const std::string& identity) override { return target->has_agent(identity); }
    std::optional<tp::CommandView> dispatch(const std::string& agent, const tp::Cmd& command,
                                            const tp::ResultHook& hook) override {
        return target->dispatch(agent, command, hook);
    }
    tc::CancelOutcome cancel(const std::string& agent, std::uint32_t seq) override {
        return target->cancel(agent, seq);
    }
};

struct RunResult {
    int exit_code = -1;
    std::string out;
    std::string err;
};

RunResult drive(FakeConsole& console, const std::vector<std::string>& args) {
    std::ostringstream out;
    std::ostringstream err;
    std::istringstream in;
    RunResult result;
    result.exit_code = tc::run_cli(args, tc::CliStreams{out, err, in, false, false},
                                   [&console](const std::string&) {
                                       return std::unique_ptr<tc::IOperConsole>(
                                           new RecordingRef(console));
                                   });
    result.out = out.str();
    result.err = err.str();
    return result;
}

} // namespace

TEST_CASE("unknown verb fails before any dispatch with exit 2") {
    FakeConsole console;
    const RunResult result = drive(console, {"tether-console", "frobnicate", "stuff"});
    CHECK(result.exit_code == tc::kExitUsage);
    CHECK(result.err.find("error: unknown verb 'frobnicate'") != std::string::npos);
    CHECK(result.out.empty());
    CHECK(console.dispatch_calls == 0);
    CHECK(console.cancel_calls == 0);
}

TEST_CASE("unknown agent fails before any dispatch with exit 3") {
    FakeConsole console;
    console.rows = {{"agent-1", tp::RegistryState::Live}};
    RunResult result = drive(console, {"tether-console", "exec", "ghost", "ls"});
    CHECK(result.exit_code == tc::kExitUnknownAgent);
    CHECK(result.err.find("error: unknown agent 'ghost'") != std::string::npos);
    CHECK(console.dispatch_calls == 0);

    // cancel refuses the same way, before dispatch
    result = drive(console, {"tether-console", "cancel", "ghost", "3"});
    CHECK(result.exit_code == tc::kExitUnknownAgent);
    CHECK(result.err.find("error: unknown agent 'ghost'") != std::string::npos);
    CHECK(console.cancel_calls == 0);
    CHECK(console.dispatch_calls == 0);
}

TEST_CASE("no verb at all is a usage failure") {
    FakeConsole console;
    const RunResult result = drive(console, {"tether-console"});
    CHECK(result.exit_code == tc::kExitUsage);
    CHECK(result.err.find("error:") != std::string::npos);
}

TEST_CASE("agents lists identities and states on stdout only") {
    FakeConsole console;
    console.rows = {{"agent-1", tp::RegistryState::Live},
                    {"agent-2", tp::RegistryState::Down},
                    {"agent-3", tp::RegistryState::Stale}};
    const RunResult result = drive(console, {"tether-console", "agents"});
    CHECK(result.exit_code == tc::kExitOk);
    CHECK(result.out == "agent-1 live\nagent-2 down\nagent-3 stale\n");
    CHECK(result.err.empty()); // result channel carries nothing but the listing
}

TEST_CASE("exec dispatches to the named agent and streams the result to stdout") {
    FakeConsole console;
    console.rows = {{"agent-1", tp::RegistryState::Live}};
    console.canned_pieces = {tp::ResultText{"listing:"}};
    console.canned_view = console.completed_view();

    const RunResult result =
        drive(console, {"tether-console", "exec", "agent-1", "ls", "-la", "/tmp"});
    CHECK(result.exit_code == tc::kExitOk);
    CHECK(result.out == "listing:"); // result bytes only, verbatim
    CHECK(result.err.empty());       // quiet success: stderr stays clean

    REQUIRE(console.dispatch_log.size() == 1);
    CHECK(console.dispatch_log[0].first == "agent-1");
    CHECK(console.dispatch_log[0].second.command == "ls");
    REQUIRE(console.dispatch_log[0].second.args.size() == 2); // arguments are data
    CHECK(console.dispatch_log[0].second.args[0] == "-la");
    CHECK(console.dispatch_log[0].second.args[1] == "/tmp");
}

TEST_CASE("exec reports the agent's failure on stderr with exit 1") {
    FakeConsole console;
    console.rows = {{"agent-1", tp::RegistryState::Live}};
    console.canned_pieces = {tp::Err{"no_such_command", "ls is not a v2 command"}};
    console.canned_view = tp::CommandView{};
    console.canned_view->status = tp::CommandStatus::Failed;
    console.canned_view->error_code = "no_such_command";
    console.canned_view->pieces = console.canned_pieces;

    const RunResult result = drive(console, {"tether-console", "exec", "agent-1", "ls"});
    CHECK(result.exit_code == tc::kExitFailure);
    CHECK(result.out.empty()); // failure details never flood the result channel
    CHECK(result.err.find("error: command failed: no_such_command") != std::string::npos);
    CHECK(result.err.find("ls is not a v2 command") != std::string::npos);
}

TEST_CASE("cancel dispatches the cancel and reports its outcome") {
    FakeConsole console;
    console.rows = {{"agent-1", tp::RegistryState::Live}};
    RunResult sent = drive(console, {"tether-console", "cancel", "agent-1", "3"});
    CHECK(sent.exit_code == tc::kExitOk);
    CHECK(sent.err.find("status: cancel sent for seq 3") != std::string::npos);

    console.canned_cancel = tc::CancelOutcome::NotPending;
    sent = drive(console, {"tether-console", "cancel", "agent-1", "4"});
    CHECK(sent.exit_code == tc::kExitFailure);
    CHECK(sent.err.find("error: no in-flight command with seq 4") != std::string::npos);

    console.canned_cancel = tc::CancelOutcome::NotEligible;
    sent = drive(console, {"tether-console", "cancel", "agent-1", "5"});
    CHECK(sent.exit_code == tc::kExitFailure);
    CHECK(sent.err.find("error:") != std::string::npos);
}

TEST_CASE("malformed verb tails are usage failures (exit 2)") {
    FakeConsole console;
    console.rows = {{"agent-1", tp::RegistryState::Live}};
    CHECK(drive(console, {"tether-console", "exec", "agent-1"}).exit_code == tc::kExitUsage);
    CHECK(drive(console, {"tether-console", "cancel", "agent-1", "abc"}).exit_code ==
          tc::kExitUsage);
    CHECK(drive(console, {"tether-console", "agents", "extra"}).exit_code == tc::kExitUsage);
    CHECK(console.dispatch_calls == 0);
    CHECK(console.cancel_calls == 0);
}

TEST_CASE("scripts distinguish the outcome classes purely by exit code") {
    // The scripted loop scenario: same verb shape, different outcomes.
    FakeConsole ok;
    ok.rows = {{"agent-1", tp::RegistryState::Live}};
    ok.canned_pieces = {tp::ResultText{"ok"}};
    ok.canned_view = ok.completed_view();
    const int success = drive(ok, {"tether-console", "exec", "agent-1", "ls"}).exit_code;

    FakeConsole failed;
    failed.rows = {{"agent-1", tp::RegistryState::Live}};
    failed.canned_view = tp::CommandView{};
    failed.canned_view->status = tp::CommandStatus::Failed;
    failed.canned_view->error_code = "timeout";
    const int failure = drive(failed, {"tether-console", "exec", "agent-1", "ls"}).exit_code;

    FakeConsole console;
    const int usage = drive(console, {"tether-console", "exec"}).exit_code; // malformed tail
    const int unknown_agent = drive(console, {"tether-console", "exec", "ghost", "ls"}).exit_code;

    CHECK(success == 0);
    CHECK(failure != success);
    CHECK(usage != success);
    CHECK(unknown_agent != success);
    CHECK(usage != failure);
    CHECK(unknown_agent != failure);
    CHECK(console.dispatch_calls == 0);
}
