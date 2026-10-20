// tests/tcp_loopback_test.cpp - task 4.1: the TCP transport end-to-end on
// real loopback sockets, with the P3 session state machine driving handshake,
// keepalive and seq over live wires:
//  - dial-out retry/backoff helpers (pure) and a bounded-attempts dialer
//    against an unreachable console
//  - an agent dials out, authenticates (HMAC over the real socket), a
//    TEST-STUB responder executes a protocol command, and the console
//    receives the result correlated to its seq (the stub lives in test code
//    on purpose: command execution wiring is Phase 5)
//  - an agent whose socket dies abruptly is detected Down on the next console
//    pump (immediate path)
//  - an agent that stays connected but silent is detected within the
//    configured keepalive window (150 ms + tolerance; the exact window math
//    is pinned by P3's clock-injected suite)
//  - two agents register distinct identities; the console addresses both
//  - a spawned REAL tether-agent process dials out (design D1), authenticates,
//    and a hard process kill mid-session brings the console session down
//
// Real waits stay small: per-step waits are bounded (silent-death check ~
// window+tolerance, kill detection <= 100 ms) and the only longer one is the
// one-time child-process startup, asserted to complete quickly.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "session.hpp"
#include "tcp.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <filesystem>
#include <fstream>

#include <cstdlib> // getenv

namespace tt = tether::transport;
namespace tp = tether::protocol;

namespace {

std::atomic<int> g_dir_counter{0};

tt::TransportMillis now_ms() {
    return std::int64_t{std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count()};
}

// Session timing for the loopback: real wall-clock, tiny values so the suite
// stays well under a second.
tp::SessionConfig loopback_config() {
    tp::SessionConfig config;
    config.command_timeout = 3000;
    config.auth_timeout = 2000;
    config.keepalive_interval = 50;
    config.keepalive_window = 150;
    return config;
}

tp::TokenResolver resolver_from(std::map<std::string, std::string> tokens,
                                const std::string& fallback = {}) {
    return [tokens = std::move(tokens), fallback](const std::string& identity) {
        const auto it = tokens.find(identity);
        return it == tokens.end() ? fallback : it->second;
    };
}

tt::ConsoleTransportOptions loopback_console_options(const std::map<std::string, std::string>& tokens,
                                                     tp::SessionConfig session) {
    tt::ConsoleTransportOptions options;
    options.listen_address = "127.0.0.1";
    options.port = 0; // ephemeral
    options.session = std::move(session);
    options.resolve_token = resolver_from(tokens);
    return options;
}

std::string result_contents(const tp::CommandView& view) {
    std::string text;
    for (const tp::Message& piece : view.pieces) {
        if (std::holds_alternative<tp::ResultText>(piece)) {
            text += std::get<tp::ResultText>(piece).text;
        }
    }
    return text;
}

bool established(const tt::TcpConsoleTransport& console, const std::string& identity) {
    const auto session = console.session_for(identity);
    return session && session->state() == tp::SessionState::Established;
}

// Drives console+agent pumps (real wall now) until the handshake completes.
// Framing rides real async sockets, so deadline-based with small sleeps.
bool handshake_until_established(tt::TcpConsoleTransport& console,
                                 std::unique_ptr<tt::TcpAgentLink>& agent,
                                 const std::string& identity,
                                 tt::TransportMillis budget_ms = 1500) {
    agent->begin_handshake(now_ms());
    const tt::TransportMillis deadline = now_ms() + budget_ms;
    for (;;) {
        const tt::TransportMillis now = now_ms();
        console.pump(now);
        agent->pump(now);
        if (agent->established() && established(console, identity)) {
            return true;
        }
        if (now >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Pumps console+agent until the seq leaves Pending (or the deadline passes).
// Real sockets deliver asynchronously, so this is deadline-based with small
// sleeps rather than a fixed pump count.
bool pump_until_terminal(tt::TcpConsoleTransport& console, std::unique_ptr<tt::TcpAgentLink>& agent,
                         const std::string& identity, std::uint32_t seq, int budget_ms = 400) {
    const tt::TransportMillis deadline = now_ms() + budget_ms;
    for (;;) {
        const auto view = console.session_for(identity)->command(seq);
        if (view && view->status != tp::CommandStatus::Pending) {
            return true;
        }
        if (now_ms() >= deadline) {
            return false;
        }
        const tt::TransportMillis now = now_ms();
        agent->pump(now);
        console.pump(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Pumps only the console for up to `budget_ms` of real time; reports whether
// the named session went Down meanwhile (tolerance-friendly detection check).
bool console_goes_down_within(tt::TcpConsoleTransport& console, const std::string& identity,
                              int budget_ms) {
    const tt::TransportMillis deadline = now_ms() + budget_ms;
    for (;;) {
        console.pump(now_ms());
        const auto session = console.session_for(identity);
        if (session && session->state() == tp::SessionState::Down) {
            return true;
        }
        if (now_ms() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ---- spawned-process helper (cross-platform, real child process) --------------

struct SpawnedAgent {
#if defined(_WIN32)
    PROCESS_INFORMATION process{};
#else
    ::pid_t process = -1;
#endif
    std::filesystem::path config_path;

    bool valid() const noexcept {
#if defined(_WIN32)
        return process.hProcess != nullptr;
#else
        return process > 0;
#endif
    }

    void kill() {
        if (!valid()) {
            return;
        }
#if defined(_WIN32)
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 2000);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        process = PROCESS_INFORMATION{};
#else
        ::kill(process, SIGKILL);
        int status = 0;
        ::waitpid(process, &status, 0);
        process = -1;
#endif
    }
};

std::filesystem::path locate_agent_binary() {
    // TETHER_AGENT_EXE wins when provided (advanced use); otherwise probe the
    // conventional build outputs relative to THIS test binary (build/tests).
    if (const char* from_env = std::getenv("TETHER_AGENT_EXE")) {
        std::error_code ignored;
        std::filesystem::path candidate(from_env);
        if (std::filesystem::exists(candidate, ignored)) {
            return candidate;
        }
    }
    char buffer[4096]{};
#if defined(_WIN32)
    const DWORD length = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
    REQUIRE(length > 0);
#else
    const ssize_t length =
        ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    REQUIRE(length > 0);
    buffer[length] = '\0';
#endif
    const std::filesystem::path here = std::filesystem::path(buffer).parent_path();

    // Multi-config generators (MSVC) nest per-config outputs one level deeper
    // (build/tests/Release for the test, build/agent/Release for the agent),
    // so both single- and multi-config relatives are probed.
    for (const std::filesystem::path candidate :
         {here / ".." / "agent" / "Release" / "tether-agent.exe",
          here / ".." / "agent" / "Debug" / "tether-agent.exe",
          here / ".." / "agent" / "tether-agent.exe",
          here / ".." / "agent" / "tether-agent",
          here / ".." / ".." / "agent" / "Release" / "tether-agent.exe",
          here / ".." / ".." / "agent" / "Debug" / "tether-agent.exe",
          here / ".." / ".." / "agent" / "tether-agent.exe",
          here / ".." / ".." / "agent" / "tether-agent"}) {
        std::error_code ignored;
        if (std::filesystem::exists(candidate, ignored)) {
            return std::filesystem::weakly_canonical(candidate);
        }
    }
    return {};
}

SpawnedAgent spawn_tether_agent(const std::string& console_host, std::uint16_t console_port,
                                const std::string& agent_name, const std::string& token) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("tether-tcp-tests-" + std::to_string(g_dir_counter++));
    std::filesystem::create_directories(dir);
    const std::filesystem::path config_path = dir / "agent.json";
    {
        std::ofstream out(config_path, std::ios::binary | std::ios::trunc);
        out << "{\n";
        out << "  \"console_address\": \"" << console_host << "\",\n";
        out << "  \"console_port\": " << console_port << ",\n";
        out << "  \"agent_name\": \"" << agent_name << "\",\n";
        out << "  \"token\": \"" << token << "\"\n";
        out << "}\n";
    }

    const std::filesystem::path exe = locate_agent_binary();
    REQUIRE_MESSAGE(!exe.empty(), "tether-agent binary not found next to the tests");

    SpawnedAgent spawned;
    spawned.config_path = config_path;
    const std::string command_line =
        "\"" + exe.string() + "\" --config \"" + config_path.string() + "\"";
#if defined(_WIN32)
    // CreateProcessA takes (and may rewrite) a mutable LPSTR command line, so
    // the command rides a writable buffer instead of string::data() (const).
    std::vector<char> cmd_buf(command_line.begin(), command_line.end());
    cmd_buf.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    REQUIRE(CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           nullptr, &startup, &spawned.process));
#else
    spawned.process = ::fork();
    REQUIRE(spawned.process >= 0);
    if (spawned.process == 0) {
        // execv's argv is char** by (legacy) contract; own mutable buffers
        // instead of const_cast-ing std::string internals.
        const std::string exe_path = exe.string();
        const std::string cfg_path = config_path.string();
        std::vector<char> exe_buf(exe_path.begin(), exe_path.end());
        std::vector<char> cfg_buf(cfg_path.begin(), cfg_path.end());
        exe_buf.push_back('\0');
        cfg_buf.push_back('\0');
        char arg_config[] = "--config";
        char* argv[] = {exe_buf.data(), arg_config, cfg_buf.data(), nullptr};
        ::execv(exe_buf.data(), argv);
        ::_exit(127); // exec failed: exit fast, never wander as a test process
    }
#endif
    return spawned;
}

} // namespace

// ---- retry/backoff helpers ------------------------------------------------------

TEST_CASE("dial-out backoff doubles and stops at the cap") {
    const tt::DialPolicy policy{100, 1500};
    std::int64_t backoff = 0;
    backoff = tt::next_backoff_ms(backoff, policy);
    CHECK(backoff == 100);
    backoff = tt::next_backoff_ms(backoff, policy);
    CHECK(backoff == 200);
    backoff = tt::next_backoff_ms(backoff, policy);
    CHECK(backoff == 400);
    backoff = tt::next_backoff_ms(backoff, policy);
    CHECK(backoff == 800);
    backoff = tt::next_backoff_ms(backoff, policy); // 800 * 2 would exceed the cap
    CHECK(backoff == 1500); // capped: bounded interval, never an unbounded stall
    backoff = tt::next_backoff_ms(backoff, policy);
    CHECK(backoff == 1500); // the cap keeps holding through repeated failures
}

TEST_CASE("dial-out with a bounded budget fails explicitly on an unreachable console") {
    const tt::AgentEndpoint dead{"127.0.0.1", 1}; // nothing listens there
    const tt::DialPolicy fast{1, 2};              // tiny real waits (~3 ms total)
    CHECK_THROWS_AS(tt::establish_with_retries(dead, {"agent-1", "host-1"}, "tok",
                                               loopback_config(), fast, /*max_attempts=*/3),
                    tt::TransportError);
}

// ---- live loopback --------------------------------------------------------------

// The seq-to-view join goes through the registry's live session (P3 owns the
// command trackers); `session_for` never returns null once the handshake is
// registered, which the handshake helper already proved.
namespace {
auto view_of(const tt::TcpConsoleTransport& console, const std::string& identity,
             std::uint32_t seq) {
    return console.session_for(identity)->command(seq);
}
} // namespace

TEST_CASE("agent dials out, authenticates, and a protocol command round-trips the console") {
    tt::TcpConsoleTransport console(loopback_console_options(
        {{"agent-1", "tok-1"}}, loopback_config()));
    console.start();
    REQUIRE(console.listening_port() != 0);

    std::unique_ptr<tt::TcpAgentLink> agent = tt::TcpAgentLink::connect(
        {"127.0.0.1", console.listening_port()}, {"agent-1", "host-1"}, "tok-1",
        loopback_config());
    REQUIRE(handshake_until_established(console, agent, "agent-1"));
    CHECK(console.has_agent("agent-1"));

    // TEST-STUB responder (test code only): reply with a correlated RESULT_TEXT.
    agent->set_command_handler([&](std::uint32_t seq, const tp::Cmd& command,
                                   const std::function<bool()>&) {
        agent->send_result(seq, tp::ResultText{"stub-responder:" + command.command});
    });

    std::vector<std::string> hook_log;
    tp::ResultHook hook = [&](std::uint32_t, const tp::Message& piece, bool) {
        if (std::holds_alternative<tp::ResultText>(piece)) {
            hook_log.push_back(std::get<tp::ResultText>(piece).text);
        }
    };

    const std::uint32_t seq = console.issue("agent-1", tp::Cmd{"exec", {"ls"}}, hook, now_ms());
    REQUIRE(pump_until_terminal(console, agent, "agent-1", seq));
    auto view = view_of(console, "agent-1", seq);
    REQUIRE(view.has_value());
    CHECK(view->status == tp::CommandStatus::Completed);
    CHECK(result_contents(*view) == "stub-responder:exec"); // correlated to THIS seq
    REQUIRE(hook_log.size() == 1);
    CHECK(hook_log[0] == "stub-responder:exec");

    // Pipelined second command: seq attribution stays stable per command.
    const std::uint32_t seq_second =
        console.issue("agent-1", tp::Cmd{"who", {}}, hook, now_ms());
    REQUIRE(pump_until_terminal(console, agent, "agent-1", seq_second));
    view = view_of(console, "agent-1", seq_second);
    REQUIRE(view.has_value());
    CHECK(view->status == tp::CommandStatus::Completed);
    CHECK(result_contents(*view) == "stub-responder:who");
    CHECK(result_contents(*view_of(console, "agent-1", seq)) == "stub-responder:exec"); // first intact
}

TEST_CASE("an abruptly closed agent wire is detected down on the next console pump") {
    tt::TcpConsoleTransport console(loopback_console_options({{"agent-2", "tok-2"}},
                                                             loopback_config()));
    console.start();

    std::unique_ptr<tt::TcpAgentLink> agent = tt::TcpAgentLink::connect(
        {"127.0.0.1", console.listening_port()}, {"agent-2", "host-2"}, "tok-2",
        loopback_config());
    agent->set_command_handler(
        [&](std::uint32_t s, const tp::Cmd&, const std::function<bool()>&) {
            agent->send_result(s, tp::ResultText{"stub"});
        });
    REQUIRE(handshake_until_established(console, agent, "agent-2"));
    REQUIRE(console.has_agent("agent-2"));

    // The agent machine loses the process: the wire dies right here, no
    // disconnect frame (abrupt path). Detection is immediate, not await-based.
    agent->shutdown_stream();
    agent.reset(); // tear down the socket fully
    const tt::TransportMillis began = now_ms();
    CHECK(console_goes_down_within(console, "agent-2", 100));
    CHECK(now_ms() - began < static_cast<tt::TransportMillis>(loopback_config().keepalive_window));

    // The registry view stays honest: down sessions are not command-eligible.
    const auto session = console.session_for("agent-2");
    REQUIRE(session != nullptr);
    CHECK(session->state() == tp::SessionState::Down);
    CHECK_THROWS_AS(console.issue("agent-2", tp::Cmd{"exec", {}}, nullptr, now_ms()),
                    tt::TransportError);
}

TEST_CASE("a silently vanished agent is detected down within the keepalive window") {
    const tp::SessionConfig config = loopback_config(); // interval 50 ms, window 150 ms
    tt::TcpConsoleTransport console(loopback_console_options({{"agent-3", "tok-3"}}, config));
    console.start();

    std::unique_ptr<tt::TcpAgentLink> agent = tt::TcpAgentLink::connect(
        {"127.0.0.1", console.listening_port()}, {"agent-3", "host-3"}, "tok-3", config);
    REQUIRE(handshake_until_established(console, agent, "agent-3"));

    // The agent process is silent now: nobody pumps it; the console's PINGs go
    // unanswered. Detection must land inside [window - slack, window + tol].
    const int window = static_cast<int>(config.keepalive_window);
    const tt::TransportMillis began = now_ms();
    CHECK(console_goes_down_within(console, "agent-3", window + 100));
    const int detected = static_cast<int>(now_ms() - began);
    CHECK(detected >= window - 10);  // not before the window (within pump slack)
    CHECK(detected < window + 100);  // but within the window + tolerance
}

TEST_CASE("two agents register distinct identities and stay addressable") {
    tt::TcpConsoleTransport console(loopback_console_options(
        {{"agent-a", "tok-a"}, {"agent-b", "tok-b"}}, loopback_config()));
    console.start();

    std::unique_ptr<tt::TcpAgentLink> agent_a = tt::TcpAgentLink::connect(
        {"127.0.0.1", console.listening_port()}, {"agent-a", "host-a"}, "tok-a",
        loopback_config());
    std::unique_ptr<tt::TcpAgentLink> agent_b = tt::TcpAgentLink::connect(
        {"127.0.0.1", console.listening_port()}, {"agent-b", "host-b"}, "tok-b",
        loopback_config());
    REQUIRE(handshake_until_established(console, agent_a, "agent-a"));
    REQUIRE(handshake_until_established(console, agent_b, "agent-b"));
    CHECK(console.has_agent("agent-a"));
    CHECK(console.has_agent("agent-b"));

    agent_a->set_command_handler(
        [&](std::uint32_t s, const tp::Cmd&, const std::function<bool()>&) {
            agent_a->send_result(s, tp::ResultText{"from-a"});
        });
    agent_b->set_command_handler(
        [&](std::uint32_t s, const tp::Cmd&, const std::function<bool()>&) {
            agent_b->send_result(s, tp::ResultText{"from-b"});
        });

    const std::uint32_t seq_a = console.issue("agent-a", tp::Cmd{"who", {}}, nullptr, now_ms());
    const std::uint32_t seq_b = console.issue("agent-b", tp::Cmd{"who", {}}, nullptr, now_ms());
    const tt::TransportMillis deadline = now_ms() + 400;
    for (;;) {
        const auto view_a = console.session_for("agent-a")->command(seq_a);
        const auto view_b = console.session_for("agent-b")->command(seq_b);
        const bool a_done = view_a && view_a->status != tp::CommandStatus::Pending;
        const bool b_done = view_b && view_b->status != tp::CommandStatus::Pending;
        if ((a_done && b_done) || now_ms() >= deadline) {
            break;
        }
        const tt::TransportMillis now = now_ms();
        agent_a->pump(now);
        agent_b->pump(now);
        console.pump(now);
    }
    CHECK(result_contents(*view_of(console, "agent-a", seq_a)) == "from-a");
    CHECK(result_contents(*view_of(console, "agent-b", seq_b)) == "from-b");
}

TEST_CASE("an agent with a wrong token is refused and never reaches the registry") {
    tt::TcpConsoleTransport console(
        loopback_console_options({{"agent-1", "tok-1"}}, loopback_config()));
    console.start();

    std::unique_ptr<tt::TcpAgentLink> agent = tt::TcpAgentLink::connect(
        {"127.0.0.1", console.listening_port()}, {"agent-1", "host-1"}, "wrong-token",
        loopback_config());
    agent->begin_handshake(now_ms());
    // Refusal (wrong-HMAC rejection) rides the real wire back asynchronously;
    // deadline-bounded wait instead of a fixed pump count. A console refusal
    // to a wrong answer arrives in milliseconds; the auth window is the only
    // fallback, so a wrong-HMAC refusal must beat this deadline easily.
    bool refused = false;
    const tt::TransportMillis deadline = now_ms() + 1500;
    for (;;) {
        console.pump(now_ms());
        agent->pump(now_ms());
        refused = agent->down();
        if (refused || now_ms() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(refused);
    CHECK_FALSE(console.has_agent("agent-1")); // registration requires Established
}

TEST_CASE("a killed agent process is detected down (spawned production binary)") {
    tt::TcpConsoleTransport console(
        loopback_console_options({{"victim-agent", "victim-token"}}, tp::SessionConfig{}));
    console.start();

    SpawnedAgent agent =
        spawn_tether_agent("127.0.0.1", console.listening_port(), "victim-agent", "victim-token");
    REQUIRE(agent.valid());

    // The child dialed OUT (design D1) and authenticates; wait briefly for its
    // registration. This is process startup latency, not a keepalive wait.
    bool registered = false;
    const tt::TransportMillis registration_deadline = now_ms() + 3000;
    while (now_ms() < registration_deadline) {
        console.pump(now_ms());
        if (established(console, "victim-agent")) {
            registered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(registered);

    // Kill the process hard, mid-session; the console must detect the death
    // promptly (EOF/reset on the wire), certainly within the keepalive window.
    const tt::TransportMillis began = now_ms();
    agent.kill();
    CHECK(console_goes_down_within(console, "victim-agent", 100));
    CHECK(now_ms() - began < 100);
}
