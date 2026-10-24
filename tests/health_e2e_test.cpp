// tests/health_e2e_test.cpp - add-health-telemetry task 7.1: a focused
// end-to-end health scenario over a REAL loopback TCP session.
//
// Unlike cli_integration_test.cpp's canned responder, this harness wires the
// full production command path:
//
//   run_cli -> TcpOperConsole -> TcpConsoleTransport -> live sockets ->
//   AgentSession -> agent::install_commands -> the REAL capability dispatcher
//   -> the telemetry family -> HealthTelemetryControl -> HealthState
//
// The host probe is the only injected seam: a deterministic stepped probe
// keeps CPU/memory values assertable without depending on this machine's load.
// The per-OS probes themselves are covered by health_test.cpp's smoke checks.
//
// The scenario:
//   - an operator-issued `exec <agent> health` returns one parseable snapshot
//     with the full D5 field set (latest, window, counters, freshness);
//   - a `monitor <agent> --json` invocation observes the agent, the test then
//     forces a redial (socket shutdown + fresh HELLO), and the monitor reports
//     the unavailable gap, resumes, and counts exactly one reconnect;
//   - the agent's own counter agrees: post-reconnect snapshots carry
//     "reconnects": 1;
//   - the monitor invocation leaves exactly ONE audit record, with its
//     internal health polls deliberately unaudited.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "cli.hpp"
#include "command_handler.hpp"
#include "health.hpp"
#include "logging.hpp"
#include "tcp.hpp"
#include "tcp_console.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <istream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace tc = tether::cli;
namespace tp = tether::protocol;
namespace tt = tether::transport;
namespace tsys = tether::syscalls;

namespace {

constexpr const char* kAgent = "e2e-health-agent";
constexpr const char* kToken = "e2e-health-token";
constexpr const char* kInstance = "e2e-health-host";

tt::TransportMillis now_ms() {
    return static_cast<tt::TransportMillis>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

tp::SessionConfig loopback_config() {
    tp::SessionConfig config;
    config.command_timeout = 5000;
    config.auth_timeout = 2000;
    config.keepalive_interval = 100;
    config.keepalive_window = 400;
    return config;
}

class NullLogSink final : public tether::common::LogSink {
public:
    void write(const tether::common::LogRecord&) override {}
};

// Deterministic host readings: every post-baseline CPU delta is 50/100, so
// every measured percentage is exactly 50%. Memory grows 64 MiB per read,
// which makes the window's memory_used_max distinguishable from latest.
class SteppedHealthProbe final : public tsys::IHealthHostProbe {
public:
    tsys::HostReading read() override {
        ++reads_;
        const std::uint64_t step = static_cast<std::uint64_t>(reads_);
        tsys::HostReading reading;
        reading.cpu_busy = 10'000 + step * 50;
        reading.cpu_total = 100'000 + step * 100;
        reading.memory_used = (512 + step * 64) * 1024 * 1024;
        reading.memory_total = 4ull * 1024 * 1024 * 1024;
        reading.processes = 42;
        reading.threads = 123;
        return reading;
    }

    int reads() const noexcept { return reads_; }

private:
    int reads_ = 0;
};

bool console_established(const tt::TcpConsoleTransport& console, const std::string& identity) {
    const auto session = console.session_for(identity);
    return session && session->state() == tp::SessionState::Established;
}

bool handshake_until_established(tt::TcpConsoleTransport& console, tt::TcpAgentLink& agent,
                                 tt::TransportMillis budget_ms = 3000) {
    const tt::TransportMillis deadline = now_ms() + budget_ms;
    for (;;) {
        const tt::TransportMillis now = now_ms();
        console.pump(now);
        agent.pump(now);
        if (agent.established() && console_established(console, kAgent)) {
            return true;
        }
        if (now >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// A per-invocation delegator over the one real TcpOperConsole the test owns.
// The health counter is incremented only after a COMPLETED health poll, which
// is the deterministic trigger the side pump uses to force the redial.
struct CountingConsole final : tc::IOperConsole {
    tc::TcpOperConsole* target = nullptr;
    int health_completed = 0;

    explicit CountingConsole(tc::TcpOperConsole& console) : target(&console) {}

    std::vector<tp::RegistryEntry> list_agents() override { return target->list_agents(); }
    bool has_agent(const std::string& identity) override { return target->has_agent(identity); }
    void pump() override { target->pump(); }
    bool is_live(const std::string& identity) override { return target->is_live(identity); }
    tc::LinkTelemetry link_telemetry() override { return target->link_telemetry(); }

    std::optional<tp::CommandView> dispatch(const std::string& agent, const tp::Cmd& command,
                                            const tp::ResultHook& hook) override {
        const std::optional<tp::CommandView> view = target->dispatch(agent, command, hook);
        if (view && view->status == tp::CommandStatus::Completed && command.command == "health") {
            ++health_completed;
        }
        return view;
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

enum class Phase { Live, Dropped, Reconnecting, Handshake, LiveAgain };

} // namespace

TEST_CASE("health telemetry works end to end across a forced agent redial") {
    tether::common::Logger::instance().set_sink(std::make_unique<NullLogSink>());
    tether::common::Logger::instance().set_min_level(tether::common::LogLevel::Error);

    // One sampler lives for the whole process, exactly as agent/main.cpp wires
    // it: the three explicit samples model the window that accumulated before
    // the operator arrived. Uptime is anchored five seconds in the past.
    tsys::HealthState health{1, 60, tsys::steady_clock_millis() - 5000};
    SteppedHealthProbe probe;
    const std::shared_ptr<tsys::ITelemetryControl> telemetry =
        tsys::make_health_telemetry_control(health, probe);
    telemetry->sample();
    telemetry->sample();
    telemetry->sample();

    tt::ConsoleTransportOptions options;
    options.listen_address = "127.0.0.1";
    options.port = 0;
    options.session = loopback_config();
    options.resolve_token = [](const std::string& identity) {
        return identity == kAgent ? std::string{kToken} : std::string{};
    };

    tt::TcpConsoleTransport console{options};
    console.start();
    REQUIRE(console.listening_port() != 0);

    tsys::CommandDispatcher dispatcher;

    const auto connect_agent = [&]() -> std::unique_ptr<tt::TcpAgentLink> {
        auto link = tt::TcpAgentLink::connect(
            {"127.0.0.1", console.listening_port()}, {kAgent, kInstance}, kToken,
            loopback_config());
        tether::agent::install_commands(dispatcher, *link, telemetry);
        link->begin_handshake(now_ms());
        return link;
    };

    std::unique_ptr<tt::TcpAgentLink> agent = connect_agent();
    REQUIRE(handshake_until_established(console, *agent));
    health.note_established(tsys::steady_clock_millis());

    // ---- 1. operator-issued health: the full snapshot contract -------------
    tc::TcpOperConsole exec_console{
        console, tt::monotonic_now_ms,
        [&] {
            if (agent) {
                agent->pump(tt::monotonic_now_ms());
                telemetry->pump();
            }
        }};

    std::ostringstream exec_out;
    std::ostringstream exec_err;
    std::istringstream exec_in;
    RunResult executed;
    executed.exit_code = tc::run_cli(
        {"tether-console", "exec", kAgent, "health"},
        tc::CliStreams{exec_out, exec_err, exec_in, false, false},
        [&](const std::string&) -> std::unique_ptr<tc::IOperConsole> {
            return std::make_unique<CountingConsole>(exec_console);
        });
    executed.out = exec_out.str();
    executed.err = exec_err.str();

    REQUIRE(executed.exit_code == tc::kExitOk);
    REQUIRE(executed.err.empty());
    const nlohmann::json snapshot = nlohmann::json::parse(executed.out);

    CHECK(snapshot.at("interval_s") == 1);
    CHECK(snapshot.at("uptime_seconds").get<std::uint64_t>() >= 4);
    CHECK(snapshot.at("reconnects") == 0);

    const nlohmann::json& latest = snapshot.at("latest");
    REQUIRE(latest.at("cpu_percent").is_number());
    CHECK(latest.at("cpu_percent").get<double>() == doctest::Approx(50.0));
    CHECK(latest.at("memory_total") == 4ull * 1024 * 1024 * 1024);
    CHECK(latest.at("processes") == 42);
    CHECK(latest.at("threads") == 123);
    CHECK(latest.at("sampled_at_ms").get<std::int64_t>() > 0);

    const nlohmann::json& window = snapshot.at("window");
    CHECK(window.at("span_seconds") == 60);
    CHECK(window.at("ticks").get<std::size_t>() >= 3);
    REQUIRE(window.at("cpu_percent_avg").is_number());
    CHECK(window.at("cpu_percent_avg").get<double>() == doctest::Approx(50.0));
    CHECK(window.at("cpu_percent_max").get<double>() == doctest::Approx(50.0));
    REQUIRE(window.at("memory_used_max").is_number_unsigned());
    CHECK(window.at("memory_used_max").get<std::uint64_t>() >=
          latest.at("memory_used").get<std::uint64_t>());
    CHECK(window.at("window_coverage").get<double>() > 0.0);
    CHECK(snapshot.at("sample_age_ms").get<std::int64_t>() >= 0);

    // ---- 2. monitor across a forced redial ----------------------------------
    // The side pump is the agent half of the loopback: it advances the real
    // link and sampler, and drives the scripted lifecycle once the monitor has
    // completed two observations. Dropping after a counted poll keeps the
    // ordering deterministic (no sleeps or wall-clock races).
    Phase phase = Phase::Live;
    CountingConsole* counted = nullptr;
    bool finished = false;
    std::uint64_t side_pumps = 0;

    const auto side_pump = [&] {
        if (++side_pumps > 20'000) {
            tc::request_monitor_stop();
            return;
        }
        const tt::TransportMillis now = now_ms();
        switch (phase) {
        case Phase::Live:
            if (agent) {
                agent->pump(now);
                telemetry->pump();
            }
            if (counted != nullptr && counted->health_completed >= 2) {
                agent->shutdown_stream();
                phase = Phase::Dropped;
            }
            break;
        case Phase::Dropped:
            // The console notices the closed socket on a transport pump; once
            // its session is gone the agent half is re-created and re-dials.
            if (!console_established(console, kAgent)) {
                agent.reset();
                phase = Phase::Reconnecting;
            }
            break;
        case Phase::Reconnecting:
            agent = connect_agent();
            phase = Phase::Handshake;
            break;
        case Phase::Handshake:
            if (agent) {
                agent->pump(now);
                telemetry->pump();
            }
            if (agent && agent->established() && console_established(console, kAgent)) {
                // The second establishment is a true re-registration: the
                // agent-side counter must read 1 from here on.
                health.note_established(tsys::steady_clock_millis());
                phase = Phase::LiveAgain;
            }
            break;
        case Phase::LiveAgain:
            if (agent) {
                agent->pump(now);
                telemetry->pump();
            }
            if (counted != nullptr && counted->health_completed >= 4) {
                finished = true;
                tc::request_monitor_stop();
            }
            break;
        }
    };

    tc::TcpOperConsole monitor_console{console, tt::monotonic_now_ms, side_pump};
    const auto audit = std::make_shared<tc::AuditStore>();

    std::ostringstream monitor_out;
    std::ostringstream monitor_err;
    std::istringstream monitor_in;
    RunResult monitored;
    monitored.exit_code = tc::run_cli(
        {"tether-console", "monitor", kAgent, "--json"},
        tc::CliStreams{monitor_out, monitor_err, monitor_in, false, false},
        [&](const std::string&) -> std::unique_ptr<tc::IOperConsole> {
            auto created = std::make_unique<CountingConsole>(monitor_console);
            counted = created.get();
            return created;
        },
        [&](const std::string&) -> std::shared_ptr<tc::AuditStore> { return audit; },
        tc::MonitorOptions{10});
    monitored.out = monitor_out.str();
    monitored.err = monitor_err.str();

    CHECK(finished);
    CHECK(monitored.exit_code == tc::kExitOk);
    CHECK(monitored.err.find("status: monitoring '" + std::string{kAgent} + "'") !=
          std::string::npos);
    CHECK(monitored.err.find("unavailable; waiting for re-registration") != std::string::npos);
    CHECK(monitored.err.find("is back; resuming observations") != std::string::npos);
    CHECK(monitored.err.find("monitor interrupted after") != std::string::npos);

    std::size_t ok_observations = 0;
    std::size_t unavailable_observations = 0;
    std::size_t error_observations = 0;
    bool saw_cpu = false;
    bool saw_console_reconnect = false;
    bool saw_agent_reconnect = false;
    std::istringstream lines{monitored.out};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        const nlohmann::json observation = nlohmann::json::parse(line);
        const std::string status = observation.at("status").get<std::string>();
        if (status == "unavailable") {
            ++unavailable_observations;
            continue;
        }
        if (status == "error") {
            // A poll that races the forced socket shutdown is a legitimate
            // transition observation; it must not be counted as a success,
            // and the next tick still has to report the unavailable gap.
            ++error_observations;
            continue;
        }
        REQUIRE(status == "ok");
        ++ok_observations;

        const nlohmann::json& health_json = observation.at("health");
        if (health_json.at("latest").at("cpu_percent").is_number()) {
            saw_cpu = true;
        }
        const nlohmann::json& console_json = observation.at("console");
        if (console_json.at("reconnects_observed") == 1) {
            saw_console_reconnect = true;
        }
        if (health_json.at("reconnects") == 1) {
            saw_agent_reconnect = true;
        }
    }

    CHECK(ok_observations >= 4); // two before the drop, at least two after
    CHECK(unavailable_observations >= 1);
    CHECK(error_observations <= 2); // a lost poll may race the forced shutdown
    CHECK(saw_cpu);
    CHECK(saw_console_reconnect);
    CHECK(saw_agent_reconnect);

    // ---- 3. one audit record for the invocation, not one per poll ----------
    const std::vector<tc::AuditRecord> records = audit->query();
    REQUIRE(records.size() == 1);
    const tc::AuditRecord& record = records.front();
    CHECK(record.command == "monitor");
    CHECK(record.agent == kAgent);
    CHECK(record.outcome == "executed");
    CHECK(record.seq == 0);
    REQUIRE(record.parameters.size() == 2);
    CHECK(record.parameters[0] == kAgent);
    CHECK(record.parameters[1] == "health");
    CHECK(record.detail.find("end_state=interrupted") != std::string::npos);
    CHECK(record.detail.find("reconnects_observed=1") != std::string::npos);
    CHECK(record.detail.find("observations=") != std::string::npos);
}
