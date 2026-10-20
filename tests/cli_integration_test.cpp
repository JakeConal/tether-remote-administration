// tests/cli_integration_test.cpp - task 5.5: an end-to-end CLI-through-
// protocol integration: a FAKE agent over the REAL session stack (run_cli ->
// TcpOperConsole -> TcpConsoleTransport -> ConsoleSession over live loopback
// sockets -> AgentSession) executes a command issued via the CLI verbs:
//  - an agent dials out and authenticates (HMAC over the socket), then the
//    `agents` verb lists it live through the CLI (task 5.1's registry)
//  - `exec agent-1 ls` dispatches a real CMD frame; the stub responder's
//    RESULT_TEXT lands on stdout verbatim; stderr stays clean; exit 0
//  - `exec agent-1 shutdown` without confirmation is refused BEFORE any
//    dispatch (no frame reaches the agent); with --yes it dispatches
//  - exec on an unknown agent fails pre-dispatch with the unknown-agent exit
//
// The in-process construction follows the loopback suite's driving pattern
// (real wall clock, small bounded waits) instead of spawning binaries, per
// the phase's determinism direction.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cli.hpp"
#include "logging.hpp"
#include "tcp.hpp"
#include "tcp_console.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace tc = tether::cli;
namespace tp = tether::protocol;
namespace tt = tether::transport;

namespace {

tt::TransportMillis now_ms() {
    return std::int64_t{std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count()};
}

tp::SessionConfig loopback_config() {
    tp::SessionConfig config;
    config.command_timeout = 3000;
    config.auth_timeout = 2000;
    config.keepalive_interval = 50;
    config.keepalive_window = 150;
    return config;
}

bool console_established(const tt::TcpConsoleTransport& console, const std::string& identity) {
    const auto session = console.session_for(identity);
    return session && session->state() == tp::SessionState::Established;
}

// Drives console+agent pumps until the handshake completes (loopback pattern:
// real async sockets, bounded deadline, tiny sleeps).
bool handshake_until_established(tt::TcpConsoleTransport& console,
                                 std::unique_ptr<tt::TcpAgentLink>& agent,
                                 const std::string& identity, tt::TransportMillis budget_ms = 1500) {
    agent->begin_handshake(now_ms());
    const tt::TransportMillis deadline = now_ms() + budget_ms;
    for (;;) {
        const tt::TransportMillis now = now_ms();
        console.pump(now);
        agent->pump(now);
        if (agent->established() && console_established(console, identity)) {
            return true;
        }
        if (now >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Keeps both stack halves alive between CLI calls (keepalive cadence).
void pump_both_until(tt::TcpConsoleTransport& console, std::unique_ptr<tt::TcpAgentLink>& agent,
                     tt::TransportMillis budget_ms) {
    const tt::TransportMillis deadline = now_ms() + budget_ms;
    while (now_ms() < deadline) {
        const tt::TransportMillis now = now_ms();
        console.pump(now);
        agent->pump(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

class NullLogSink final : public tether::common::LogSink {
public:
    void write(const tether::common::LogRecord&) override {}
};

// The console factory the CLI uses: a per-call delegator over one real
// TcpOperConsole the test owns (the CLI constructs its console lazily).
struct BorrowedConsole final : tc::IOperConsole {
    tc::TcpOperConsole* target = nullptr;
    explicit BorrowedConsole(tc::TcpOperConsole& t) : target(&t) {}

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

RunResult run_verb(tc::TcpOperConsole& console, const std::vector<std::string>& args) {
    std::ostringstream out;
    std::ostringstream err;
    std::istringstream in;
    RunResult result;
    result.exit_code =
        tc::run_cli(args, tc::CliStreams{out, err, in, false, false},
                    [&console](const std::string&) {
                        return std::unique_ptr<tc::IOperConsole>(new BorrowedConsole(console));
                    });
    result.out = out.str();
    result.err = err.str();
    return result;
}

} // namespace

TEST_CASE("CLI verbs drive the real session stack end-to-end") {
    tether::common::Logger::instance().set_sink(std::make_unique<NullLogSink>());
    tether::common::Logger::instance().set_min_level(tether::common::LogLevel::Error);

    tt::ConsoleTransportOptions options;
    options.listen_address = "127.0.0.1";
    options.port = 0; // ephemeral
    options.session = loopback_config();
    options.resolve_token = [](const std::string& identity) {
        return identity == "agent-1" ? std::string{"tok-1"} : std::string{};
    };

    tt::TcpConsoleTransport console{options};
    console.start();
    REQUIRE(console.listening_port() != 0);

    std::atomic<int> commands_seen{0};
    std::unique_ptr<tt::TcpAgentLink> agent =
        tt::TcpAgentLink::connect({"127.0.0.1", console.listening_port()}, {"agent-1", "host-1"},
                                  "tok-1", loopback_config());
    // The fake agent's responder (test code by design): answers exec commands
    // with a correlated RESULT_TEXT and counts what the CLI dispatched.
    agent->set_command_handler(
        [&](std::uint32_t seq, const tp::Cmd& command, const std::function<bool()>&) {
            commands_seen.fetch_add(1);
            agent->send_result(seq, tp::ResultText{"stub-responder:" + command.command});
        });
    REQUIRE(handshake_until_established(console, agent, "agent-1"));

    tc::TcpOperConsole cli_console{
        console, tt::monotonic_now_ms,
        [&agent] { agent->pump(tt::monotonic_now_ms()); } /* side pump: the fake agent */};

    // The registry reflects the live session through the CLI listing.
    RunResult listing = run_verb(cli_console, {"tether-console", "agents"});
    CHECK(listing.exit_code == tc::kExitOk);
    CHECK(listing.out == "agent-1 live\n");
    CHECK(listing.err.empty());

    // exec issues a real command over the wire; the agent's text result is
    // the console's stdout for the invocation; stderr stays clean.
    RunResult exec = run_verb(cli_console, {"tether-console", "exec", "agent-1", "ls"});
    CHECK(exec.exit_code == tc::kExitOk);
    CHECK(exec.out == "stub-responder:ls"); // exact result bytes only
    CHECK(exec.err.empty());                // stderr stays clean on success
    CHECK(commands_seen.load() == 1);

    // The confirmation gate runs at the operator surface, before dispatch:
    // without --yes nothing reaches the agent.
    const int hits_before_refusal = commands_seen.load();
    RunResult refused =
        run_verb(cli_console, {"tether-console", "exec", "agent-1", "shutdown"});
    CHECK(refused.exit_code == tc::kExitRefused);
    CHECK(refused.out.empty());
    CHECK(refused.err.find("error: refusal:") != std::string::npos);
    CHECK(refused.err.find("nothing was sent") != std::string::npos);
    CHECK(commands_seen.load() == hits_before_refusal); // zero protocol frames sent

    // Confirmed: dispatch proceeds; the stub answers and the session stays up.
    RunResult confirmed =
        run_verb(cli_console, {"tether-console", "exec", "agent-1", "shutdown", "--yes"});
    CHECK(confirmed.exit_code == tc::kExitOk);
    CHECK(confirmed.out == "stub-responder:shutdown");
    CHECK(confirmed.err.empty());
    CHECK(commands_seen.load() == hits_before_refusal + 1);

    // Unknown agent, real registry: refused pre-dispatch, no frames.
    const int hits_before_unknown = commands_seen.load();
    RunResult unknown = run_verb(cli_console, {"tether-console", "exec", "ghost", "ls"});
    CHECK(unknown.exit_code == tc::kExitUnknownAgent);
    CHECK(unknown.err.find("error: unknown agent 'ghost'") != std::string::npos);
    CHECK(commands_seen.load() == hits_before_unknown);

    // The session survived everything (spec: failures never poison it).
    RunResult again = run_verb(cli_console, {"tether-console", "exec", "agent-1", "who"});
    CHECK(again.exit_code == tc::kExitOk);
    CHECK(again.out == "stub-responder:who");

    pump_both_until(console, agent, 40); // one quiet keepalive beat before teardown
}
