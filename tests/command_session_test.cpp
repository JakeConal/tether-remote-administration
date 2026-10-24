// tests/command_session_test.cpp - the agent-side wiring test for tasks
// 6.1-6.3: the capability-matrix dispatcher rides a REAL session pair over
// in-memory pipes (no sockets, injected clocks), with the file family on
// REAL temporary disk and the process family recording-faked. Scenarios map
// to the remote-commands delta:
//  - "unknown command is refused harmlessly": an unknown verb over the wire
//    returns the structured error naming the verb, no family call happens,
//    and the same session serves a real command right after;
//  - "out-of-matrix command returns a structured refusal and the session
//    continues": an unimplemented family reports the structured unsupported
//    result and the session keeps serving;
//  - "missing confirmation blocks a destructive command" at the agent
//    LAYER: an unconfirmed delete over the wire refuses with the disk
//    meanwhile untouched, and the confirmed twin proceeds;
//  - results stream as their structured shapes: listings become result text;
//    argv travels byte-for-byte to the capability layer;
//  - health (add-health-telemetry task 4.3): the telemetry family is bound
//    through install_commands with the dial-loop-owned HealthState, and an
//    issued `health` returns the design D5 JSON snapshot, parsed here, over
//    the established session;
//  - session churn + cadence (tasks 5.1/5.2): the counters survive a redial
//    on one process-wide state, and the cadence-gated pump fills the window
//    while commands interleave, with honest age after a delayed tick.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "command_handler.hpp"
#include "health.hpp"
#include "matrix.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace tp = tether::protocol;
namespace tsys = tether::syscalls;
namespace fs = std::filesystem;

namespace {

tp::SessionConfig test_config() { return tp::SessionConfig{100, 200, 100, 300}; }

tp::TokenResolver resolver_for(const std::string& token) {
    return [token](const std::string&) { return token; };
}

// A fully handshaken console+agent pair with the dispatcher installed on the
// agent side (the same install path the service main uses).
struct Harness {
    tp::InMemoryConnection wires;
    std::shared_ptr<tp::ConsoleSession> console;
    std::unique_ptr<tp::AgentSession> agent;

    static std::unique_ptr<Harness> connect() {
        auto harness = std::make_unique<Harness>();
        harness->console = std::make_shared<tp::ConsoleSession>(
            harness->wires.to_console, harness->wires.to_agent, test_config(),
            resolver_for("tok"));
        harness->agent = std::make_unique<tp::AgentSession>(
            harness->wires.to_agent, harness->wires.to_console, tp::AgentIdentity{"agent-1", "h1"},
            "tok", test_config());
        harness->agent->begin(0);
        harness->console->pump(1);
        harness->agent->pump(2);
        harness->console->pump(3);
        REQUIRE(harness->console->state() == tp::SessionState::Established);
        return harness;
    }

    // Console issues the command; the agent's dispatch + reply happen inside
    // one pump; the console picks the correlated result in the next pump.
    tp::CommandView issue(const tp::Cmd& command) {
        const std::uint32_t seq = console->start(command, 10);
        agent->pump(11);
        console->pump(12);
        const auto view = console->command(seq);
        REQUIRE(view.has_value());
        REQUIRE(view->status != tp::CommandStatus::Pending); // sync commands end within one pump
        return *view;
    }

    std::string text_of(const tp::CommandView& view) const {
        std::string text;
        for (const tp::Message& piece : view.pieces) {
            if (const auto* result_text = std::get_if<tp::ResultText>(&piece)) {
                text += result_text->text;
            }
        }
        return text;
    }
};

// Recording fake process layer (faked at the session level: real-family
// behavior is proven in process_family_test).
struct FakeProcessControl final : tsys::IProcessControl {
    int list_calls = 0;
    int start_calls = 0;
    int stop_calls = 0;
    std::vector<std::vector<std::string>> start_log;

    std::vector<tsys::ProcessInfo> list() override {
        ++list_calls;
        return {tsys::ProcessInfo{111, "name-one"}, tsys::ProcessInfo{222, "name-two"}};
    }
    std::uint32_t start(const std::vector<std::string>& argv) override {
        ++start_calls;
        start_log.push_back(argv);
        return 4242;
    }
    void stop(std::uint32_t) override { ++stop_calls; }
};

// The telemetry seam's fake: cumulative counters advance on every successful
// read (HealthState diffs them into CPU); memory/process/thread values are
// deterministic so the snapshot can be asserted after a real wire round trip.
struct FakeHealthProbe final : tsys::IHealthHostProbe {
    int read_calls = 0;

    tsys::HostReading read() override {
        ++read_calls;
        const std::uint64_t step = static_cast<std::uint64_t>(read_calls);
        tsys::HostReading reading;
        reading.cpu_busy = step * 400; // 40% of the total delta
        reading.cpu_total = step * 1000;
        reading.memory_used = 512 + step;
        reading.memory_total = 4096;
        reading.processes = 40 + static_cast<std::uint32_t>(read_calls);
        reading.threads = 100 + static_cast<std::uint32_t>(read_calls);
        return reading;
    }
};

std::atomic<int> g_dir_counter{0};

struct DiskFixture {
    DiskFixture() : root(fs::temp_directory_path() /
                         ("tether-command-tests-" + std::to_string(g_dir_counter++))) {
        std::error_code ec;
        fs::create_directories(root, ec);
        REQUIRE_MESSAGE(!ec, "could not create the test directory");
    }
    ~DiskFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path root;

    void write(const fs::path& path, const std::string& content) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        out << content;
    }
    bool exists(const fs::path& path) const {
        std::error_code ec;
        return fs::exists(path, ec) && !ec;
    }
};

} // namespace

TEST_CASE("an unknown command over the wire is refused harmlessly") {
    std::shared_ptr<FakeProcessControl> process = std::make_shared<FakeProcessControl>();
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(process);
    dispatcher.set_file_system(tsys::make_file_system());

    const auto harness = Harness::connect();
    tether::agent::install_commands(dispatcher, *harness->agent);

    const tp::CommandView refused =
        harness->issue(tp::Cmd{"shellexec", {"powershell", "-enc", "abc"}});
    REQUIRE(refused.status == tp::CommandStatus::Failed);
    CHECK(refused.error_code == "unknown_command");

    // The console tracker ends ERR commands with its own synthesized marker
    // piece ("command ended: <code>", frozen session-layer behavior pinned by
    // session_test); the agent's verb-naming refusal rides the wire ERR
    // payload, asserted at the executor level in the next case.
    REQUIRE(refused.pieces.size() == 1);
    const auto* err = std::get_if<tp::Err>(&refused.pieces.back());
    REQUIRE(err != nullptr);
    CHECK(err->code == "unknown_command");
    CHECK(err->message == "command ended: unknown_command");

    // No system effect: the dispatcher's instrumented counter stays at zero.
    CHECK(process->list_calls == 0);
    CHECK(process->start_calls == 0);

    // The session remains usable, and the agent did not wedge or close.
    CHECK(harness->agent->state() == tp::SessionState::Established);
    const tp::CommandView after = harness->issue(tp::Cmd{"list", {}});
    CHECK(after.status == tp::CommandStatus::Completed);
    CHECK(process->list_calls == 1);
}

TEST_CASE("the agent composes the structured refusal naming the verb on the wire") {
    // Executor-level view of the same refusal (spec: "the initiator receives
    // a structured error identifying the invalid verb"): what dispatch()
    // produces for an unknown verb is exactly one ERR piece whose message
    // carries the verb - the agent puts that on the wire verbatim.
    tsys::CommandDispatcher dispatcher;
    const std::vector<tp::Message> refused =
        tether::agent::outcome_to_pieces(dispatcher.dispatch("shellexec", {}));
    REQUIRE(refused.size() == 1);
    const auto* err = std::get_if<tp::Err>(&refused.front());
    REQUIRE(err != nullptr);
    CHECK(err->code == "unknown_command");
    CHECK(err->message.find("shellexec") != std::string::npos);

    // The unsupported and missing-confirmation refusals shape the same way.
    const std::vector<tp::Message> unsupported =
        tether::agent::outcome_to_pieces(dispatcher.dispatch("screenshot", {}));
    REQUIRE(unsupported.size() == 1);
    const auto* gap = std::get_if<tp::Err>(&unsupported.front());
    REQUIRE(gap != nullptr);
    CHECK(gap->code == "unsupported_platform");
    CHECK(gap->message.find("screenshot") != std::string::npos);
    CHECK(gap->message.find("capture") != std::string::npos);
}

TEST_CASE("an unsupported command is a structured refusal and the session keeps serving") {
    auto process = std::make_shared<FakeProcessControl>();
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(process); // process live; capture/power are not
    dispatcher.set_file_system(tsys::make_file_system());

    const auto harness = Harness::connect();
    tether::agent::install_commands(dispatcher, *harness->agent);

    const tp::CommandView refused = harness->issue(tp::Cmd{"screenshot", {}});
    REQUIRE(refused.status == tp::CommandStatus::Failed);
    CHECK(refused.error_code == "unsupported_platform");

    const tp::CommandView minus_confirmation = harness->issue(tp::Cmd{"delete", {"x"}});
    CHECK(minus_confirmation.status == tp::CommandStatus::Failed);
    CHECK(minus_confirmation.error_code == "missing_confirmation");

    // A later, evaluated command rides the same session untouched.
    const tp::CommandView after = harness->issue(tp::Cmd{"list", {}});
    CHECK(after.status == tp::CommandStatus::Completed);
    CHECK(harness->text_of(after) == "111 name-one\n222 name-two\n");
}

TEST_CASE("destructive file commands confirm at the agent layer over the wire") {
    DiskFixture disk;
    const fs::path guarded = disk.root / "guarded.txt";
    disk.write(guarded, "payload");

    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(tsys::make_process_control());
    dispatcher.set_file_system(tsys::make_file_system());

    const auto harness = Harness::connect();
    tether::agent::install_commands(dispatcher, *harness->agent);

    // Unconfirmed: structured refusal at the agent; the file lives on.
    const tp::CommandView refused = harness->issue(tp::Cmd{"delete", {guarded.string()}});
    REQUIRE(refused.status == tp::CommandStatus::Failed);
    CHECK(refused.error_code == "missing_confirmation");
    CHECK(disk.exists(guarded));

    // Confirmed (token in args): the exact same session proceeds, the file
    // is gone; NOTHING was written or crashed in between.
    const tp::CommandView confirmed =
        harness->issue(tp::Cmd{"delete", {tsys::kConfirmMarker, guarded.string()}});
    CHECK(confirmed.status == tp::CommandStatus::Completed);
    CHECK(harness->text_of(confirmed).find("deleted") == 0);
    CHECK_FALSE(disk.exists(guarded));
}

TEST_CASE("start argv arrives at the capability layer byte-for-byte over the wire") {
    std::shared_ptr<FakeProcessControl> process = std::make_shared<FakeProcessControl>();
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(process);
    dispatcher.set_file_system(tsys::make_file_system());

    const auto harness = Harness::connect();
    tether::agent::install_commands(dispatcher, *harness->agent);

    const std::vector<std::string> argv{"prog;a|x&y>z", "with spaces", "q\"uote"};
    const tp::CommandView view = harness->issue(tp::Cmd{"start", argv});
    REQUIRE(view.status == tp::CommandStatus::Completed);
    CHECK(harness->text_of(view) == "started pid 4242\n");

    REQUIRE(process->start_log.size() == 1);
    CHECK(process->start_log[0] == argv); // the metacharacters are data end to end
}

TEST_CASE("health over an established session is a parsable snapshot") {
    std::shared_ptr<FakeProcessControl> process = std::make_shared<FakeProcessControl>();
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(process);
    dispatcher.set_file_system(tsys::make_file_system());

    // The dial-loop-owned sampler: uptime anchored 9 s before the command and
    // one reconnect already counted (the first establishment is the anchor).
    FakeHealthProbe probe;
    tsys::HealthState health(1, 60, tsys::steady_clock_millis() - 9000);
    health.note_established(0);
    health.note_established(1000);
    const std::shared_ptr<tsys::ITelemetryControl> telemetry =
        tsys::make_health_telemetry_control(health, probe);
    telemetry->sample(); // baseline-only first tick
    telemetry->sample(); // first CPU-bearing tick

    const auto harness = Harness::connect();
    tether::agent::install_commands(dispatcher, *harness->agent, telemetry);

    const tp::CommandView view = harness->issue(tp::Cmd{"health", {}});
    REQUIRE(view.status == tp::CommandStatus::Completed);
    const std::string text = harness->text_of(view);
    REQUIRE(!text.empty());
    CHECK(text.back() == '\n'); // the adapter's line terminator

    nlohmann::json snapshot;
    REQUIRE_NOTHROW(snapshot = nlohmann::json::parse(text));
    CHECK(snapshot.at("uptime_seconds").get<std::int64_t>() >= 9);
    CHECK(snapshot.at("reconnects").get<unsigned>() == 1);
    CHECK(snapshot.at("latest").at("cpu_percent").get<double>() == doctest::Approx(40.0));
    CHECK(snapshot.at("latest").at("memory_used").get<std::uint64_t>() == 514);
    CHECK(snapshot.at("latest").at("memory_total").get<std::uint64_t>() == 4096);
    CHECK(snapshot.at("latest").at("processes").get<std::uint32_t>() == 42);
    CHECK(snapshot.at("latest").at("threads").get<std::uint32_t>() == 102);
    CHECK(snapshot.at("window").at("ticks").get<std::size_t>() == 2);
    CHECK(snapshot.at("window").at("window_coverage").get<double>() > 0.0);
    CHECK(snapshot.at("sample_age_ms").get<std::int64_t>() >= 0);
    CHECK(snapshot.at("interval_s").get<int>() == 1);

    // The arity gate rides the same session: refused, nothing sampled, and the
    // session keeps serving.
    const int reads_before = probe.read_calls;
    const tp::CommandView refused = harness->issue(tp::Cmd{"health", {"1"}});
    REQUIRE(refused.status == tp::CommandStatus::Failed);
    CHECK(refused.error_code == "invalid_args");
    CHECK(probe.read_calls == reads_before);
    CHECK(harness->agent->state() == tp::SessionState::Established);
}

TEST_CASE("health counters survive session churn across a redial") {
    tsys::CommandDispatcher dispatcher;
    FakeHealthProbe probe;
    // The one process-wide sampler main() owns, anchored 5 s before the first
    // dial; links (and their sessions) come and go around it.
    tsys::HealthState health(1, 60, tsys::steady_clock_millis() - 5000);
    const std::shared_ptr<tsys::ITelemetryControl> telemetry =
        tsys::make_health_telemetry_control(health, probe);

    {
        const auto harness = Harness::connect();
        tether::agent::install_commands(dispatcher, *harness->agent, telemetry);
        health.note_established(tsys::steady_clock_millis()); // first dial: anchor
        telemetry->sample();
        telemetry->sample();

        const tp::CommandView view = harness->issue(tp::Cmd{"health", {}});
        REQUIRE(view.status == tp::CommandStatus::Completed);
        const nlohmann::json first = nlohmann::json::parse(harness->text_of(view));
        CHECK(first.at("reconnects").get<unsigned>() == 0);
        CHECK(first.at("uptime_seconds").get<std::int64_t>() >= 5);
    } // the link dies; main() redials

    {
        // Fresh session against the SAME state: the counters outlive the link.
        const auto harness = Harness::connect();
        tether::agent::install_commands(dispatcher, *harness->agent, telemetry);
        health.note_established(tsys::steady_clock_millis()); // re-establishment
        telemetry->sample(); // a dial-wait tick from the process-wide pump

        const tp::CommandView view = harness->issue(tp::Cmd{"health", {}});
        REQUIRE(view.status == tp::CommandStatus::Completed);
        const nlohmann::json second = nlohmann::json::parse(harness->text_of(view));
        CHECK(second.at("reconnects").get<unsigned>() == 1);
        CHECK(second.at("uptime_seconds").get<std::int64_t>() >= 5);
        CHECK(second.at("window").at("ticks").get<std::size_t>() == 3);
    }
}

TEST_CASE("the cadence pump fills the window while commands interleave") {
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(std::make_shared<FakeProcessControl>());
    dispatcher.set_file_system(tsys::make_file_system());

    FakeHealthProbe probe;
    tsys::HealthMillis now = 10'000;
    tsys::HealthState health(1, 60, 5'000);
    const std::shared_ptr<tsys::ITelemetryControl> telemetry =
        tsys::make_health_telemetry_control(health, probe, [&now] { return now; });

    const auto harness = Harness::connect();
    tether::agent::install_commands(dispatcher, *harness->agent, telemetry);

    // Four 1 s cadence ticks, each interleaved with a real command round trip
    // over the session: the pump never samples from inside the command path.
    for (int tick = 0; tick < 4; ++tick) {
        if (tick > 0) {
            now += 1000;
        }
        telemetry->pump();
        const tp::CommandView view = harness->issue(tp::Cmd{"health", {}});
        REQUIRE(view.status == tp::CommandStatus::Completed);
    }
    CHECK(probe.read_calls == 4);

    const nlohmann::json filled =
        nlohmann::json::parse(harness->text_of(harness->issue(tp::Cmd{"health", {}})));
    CHECK(filled.at("window").at("ticks").get<std::size_t>() == 4);
    CHECK(filled.at("window").at("window_coverage").get<double>() ==
          doctest::Approx(4.0 / 60.0));
    CHECK(filled.at("sample_age_ms").get<std::int64_t>() == 0);
    CHECK(filled.at("latest").at("cpu_percent").get<double>() == doctest::Approx(40.0));

    // A busy dispatcher delays the next tick: the snapshot ages honestly
    // instead of pretending the last reading is current.
    now += 5000;
    const nlohmann::json delayed =
        nlohmann::json::parse(harness->text_of(harness->issue(tp::Cmd{"health", {}})));
    CHECK(delayed.at("window").at("ticks").get<std::size_t>() == 4); // not refilled
    CHECK(delayed.at("sample_age_ms").get<std::int64_t>() == 5000);
    CHECK(delayed.at("latest").at("cpu_percent").get<double>() == doctest::Approx(40.0));

    // The overdue pump samples exactly once, re-anchoring the cadence.
    telemetry->pump();
    CHECK(probe.read_calls == 5);
    const nlohmann::json resumed =
        nlohmann::json::parse(harness->text_of(harness->issue(tp::Cmd{"health", {}})));
    CHECK(resumed.at("window").at("ticks").get<std::size_t>() == 5);
    CHECK(resumed.at("sample_age_ms").get<std::int64_t>() == 0);
}
