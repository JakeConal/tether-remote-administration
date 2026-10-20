// tests/cli_confirm_test.cpp - task 5.4: operator-side confirmation for
// destructive commands through run_cli() with injected streams and a
// recording fake console. Scenarios map to the destructive-verbs requirement
// of the operator-console delta:
//  - shutdown/restart/deletion are refused BEFORE DISPATCH without
//    confirmation: a structured refusal on stderr, a non-success exit code,
//    and ZERO dispatch calls (nothing was sent anywhere)
//  - confirmed commands (--yes) dispatch
//  - the interactive path is testable: a scripted stdin answer ("y"/"yes")
//    confirms, anything else refuses
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

// Recording fake: dispatch counters are the "nothing was sent" witness.
struct ConfirmSpy final : tc::IOperConsole {
    std::vector<tp::RegistryEntry> rows;
    int dispatch_calls = 0;
    std::vector<tp::Cmd> dispatch_log;
    int cancel_calls = 0;

    std::vector<tp::RegistryEntry> list_agents() override { return rows; }
    bool has_agent(const std::string& identity) override {
        for (const tp::RegistryEntry& row : rows) {
            if (row.identity == identity) {
                return true;
            }
        }
        return false;
    }
    std::optional<tp::CommandView> dispatch(const std::string&, const tp::Cmd& command,
                                            const tp::ResultHook& hook) override {
        ++dispatch_calls;
        dispatch_log.push_back(command);
        hook(7, tp::ResultText{"executed"}, false);
        tp::CommandView view;
        view.seq = 7;
        view.status = tp::CommandStatus::Completed;
        return view;
    }
    tc::CancelOutcome cancel(const std::string&, std::uint32_t) override {
        ++cancel_calls;
        return tc::CancelOutcome::Sent;
    }
};

struct Forwarder final : tc::IOperConsole {
    ConfirmSpy* target = nullptr;
    explicit Forwarder(ConfirmSpy& t) : target(&t) {}
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

// args: the full argv shape; stdin_bytes: scripted confirmation input.
RunResult drive(ConfirmSpy& spy, const std::vector<std::string>& args,
                const std::string& stdin_bytes, bool stdout_is_tty, bool stdin_is_tty) {
    std::ostringstream out;
    std::ostringstream err;
    std::istringstream in(stdin_bytes);
    RunResult result;
    result.exit_code =
        tc::run_cli(args, tc::CliStreams{out, err, in, stdout_is_tty, stdin_is_tty},
                    [&spy](const std::string&) {
                        return std::unique_ptr<tc::IOperConsole>(new Forwarder(spy));
                    });
    result.out = out.str();
    result.err = err.str();
    return result;
}

} // namespace

TEST_CASE("unconfirmed destructive commands fail locally with nothing sent") {
    ConfirmSpy spy;
    spy.rows = {{"agent-1", tp::RegistryState::Live}};

    // Non-terminal stdin without --yes: the CLI refuses before dispatch.
    const RunResult result =
        drive(spy, {"tether-console", "exec", "agent-1", "shutdown"}, "", false, false);
    CHECK(result.exit_code == tc::kExitRefused);
    CHECK(result.err.find("error: refusal:") != std::string::npos);
    CHECK(result.err.find("'shutdown'") != std::string::npos);
    CHECK(result.err.find("nothing was sent") != std::string::npos);
    CHECK(result.out.empty()); // stdout never carries refusals
    CHECK(spy.dispatch_calls == 0);
    CHECK(spy.cancel_calls == 0);
}

TEST_CASE("the same destructive command dispatches once confirmed with --yes") {
    ConfirmSpy spy;
    spy.rows = {{"agent-1", tp::RegistryState::Live}};

    const RunResult result =
        drive(spy, {"tether-console", "exec", "agent-1", "shutdown", "--yes"}, "", false, false);
    CHECK(result.exit_code == tc::kExitOk);
    CHECK(result.out == "executed");
    REQUIRE(spy.dispatch_calls == 1);
    CHECK(spy.dispatch_log[0].command == "shutdown");
}

TEST_CASE("interactive confirmation: scripted y answers dispatch, n refuses") {
    ConfirmSpy spy;
    spy.rows = {{"agent-1", tp::RegistryState::Live}};

    // stdin answers the prompt: the gate reads 'y' -> dispatch proceeds.
    RunResult result =
        drive(spy, {"tether-console", "exec", "agent-1", "restart"}, "y\n", true, true);
    CHECK(result.exit_code == tc::kExitOk);
    CHECK(result.out == "executed");
    REQUIRE(spy.dispatch_calls == 1);
    CHECK(spy.dispatch_log[0].command == "restart");
    // The prompt itself rode stderr.
    CHECK(result.err.find("confirm?") != std::string::npos);

    ConfirmSpy refused;
    refused.rows = {{"agent-1", tp::RegistryState::Live}};
    result = drive(refused, {"tether-console", "exec", "agent-1", "restart"}, "n\n", true, true);
    CHECK(result.exit_code == tc::kExitRefused);
    CHECK(refused.dispatch_calls == 0);

    // An empty/EOF answer refuses safely too.
    ConfirmSpy eof;
    eof.rows = {{"agent-1", tp::RegistryState::Live}};
    result = drive(eof, {"tether-console", "exec", "agent-1", "shutdown"}, "", true, true);
    CHECK(result.exit_code == tc::kExitRefused);
    CHECK(eof.dispatch_calls == 0);
}

TEST_CASE("file deletion is likewise gated (and 'yes' works case-insensitively)") {
    ConfirmSpy spy;
    spy.rows = {{"agent-1", tp::RegistryState::Live}};

    RunResult result =
        drive(spy, {"tether-console", "exec", "agent-1", "delete", "/tmp/a.txt"}, "YES\n", true,
              true);
    CHECK(result.exit_code == tc::kExitOk);
    REQUIRE(spy.dispatch_calls == 1);
    CHECK(spy.dispatch_log[0].command == "delete");
    REQUIRE(spy.dispatch_log[0].args.size() == 1);
    CHECK(spy.dispatch_log[0].args[0] == "/tmp/a.txt");

    ConfirmSpy refused;
    refused.rows = {{"agent-1", tp::RegistryState::Live}};
    result = drive(refused, {"tether-console", "exec", "agent-1", "delete", "/tmp/a.txt"}, "",
                   false, false);
    CHECK(result.exit_code == tc::kExitRefused);
    CHECK(refused.dispatch_calls == 0);
}

TEST_CASE("non-destructive commands dispatch without any confirmation") {
    ConfirmSpy spy;
    spy.rows = {{"agent-1", tp::RegistryState::Live}};

    const RunResult result =
        drive(spy, {"tether-console", "exec", "agent-1", "ls"}, "", false, false);
    CHECK(result.exit_code == tc::kExitOk);
    REQUIRE(spy.dispatch_calls == 1);
}

TEST_CASE("unknown agent still precedes the confirmation gate (both pre-dispatch)") {
    ConfirmSpy spy;
    spy.rows = {{"agent-1", tp::RegistryState::Live}};

    const RunResult result =
        drive(spy, {"tether-console", "exec", "ghost", "shutdown"}, "", false, false);
    CHECK(result.exit_code == tc::kExitUnknownAgent);
    CHECK(result.err.find("error: unknown agent 'ghost'") != std::string::npos);
    CHECK(spy.dispatch_calls == 0);
}
