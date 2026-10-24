// tests/matrix_test.cpp - task 6.1: the capability matrix dispatch layer
// (design D6) over recording fakes only - no sockets, no real children, no
// real files. Scenarios map to the remote-commands delta verbatim:
//  - closed set: an unknown verb is rejected with a structured error naming
//    the verb and ZERO family calls ("unknown command is refused harmlessly"
//    -> no system effect)
//  - matrix: a known verb outside the platform matrix resolves to one
//    structured unsupported refusal naming platform and family, and the
//    dispatcher keeps serving ("out-of-matrix capture command returns a
//    structured refusal" -> same session continues later commands)
//  - completeness: every representative of the defined set resolves to a
//    definitive outcome - success with the family implementation, or the
//    structured unsupported result where the family has not landed yet
//    ("every defined command resolves to a definitive outcome")
//  - certifications: destructive verbs refuse at this layer without the
//    confirmation token, nothing executed; parameters flow to the family as
//    literal data
//  - telemetry (add-health-telemetry tasks 4.1/4.2): `health` is the 20th
//    verb of a new family; the matrix/family gates still precede argument
//    validation, any parameter refuses with no sample taken, and a failing
//    probe maps to sample_unavailable while the window stays empty (never
//    zeros)
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "health.hpp"
#include "matrix.hpp"

#include <memory>
#include <string>
#include <vector>

namespace tsys = tether::syscalls;

namespace {

// Recording fakes: call counts ARE the "no system effect" instrument.
struct FakeProcessControl final : tsys::IProcessControl {
    int list_calls = 0;
    int start_calls = 0;
    int stop_calls = 0;
    std::vector<std::vector<std::string>> start_log;
    std::vector<std::string> stop_log;

    std::vector<tsys::ProcessInfo> list() override {
        ++list_calls;
        return {tsys::ProcessInfo{111, "name-one"}, tsys::ProcessInfo{222, "name-two"}};
    }
    std::uint32_t start(const std::vector<std::string>& argv) override {
        ++start_calls;
        start_log.push_back(argv);
        return 4242;
    }
    void stop(std::uint32_t pid) override {
        ++stop_calls;
        stop_log.push_back(std::to_string(pid));
    }
};

struct FakeFileSystem final : tsys::IFileSystem {
    int list_calls = 0;
    int remove_calls = 0;
    int move_calls = 0;
    int copy_calls = 0;
    std::string last_removed;

    std::vector<tsys::FileEntry> list(const std::string&) override {
        ++list_calls;
        return {tsys::FileEntry{"document.txt", 12, false},
                tsys::FileEntry{"folder", 0, true}};
    }
    void remove(const std::string& path) override {
        ++remove_calls;
        last_removed = path;
    }
    void move(const std::string&, const std::string&) override { ++move_calls; }
    void copy(const std::string&, const std::string&) override { ++copy_calls; }
};

struct Fakes {
    std::shared_ptr<FakeProcessControl> process = std::make_shared<FakeProcessControl>();
    std::shared_ptr<FakeFileSystem> file = std::make_shared<FakeFileSystem>();

    void wire_into(tsys::CommandDispatcher& dispatcher) const {
        // The tests NEED the recording fakes, so the wiring casts the typed
        // pointers to the concrete recorders (same objects, same calls).
        dispatcher.set_process_control(process);
        dispatcher.set_file_system(file);
    }

    int total_family_calls() const {
        return process->list_calls + process->start_calls + process->stop_calls +
               file->list_calls + file->remove_calls + file->move_calls + file->copy_calls;
    }
};

// The telemetry family's recording probe: cumulative counters advance on
// every successful read (the engine diffs them), and `fail` drives the
// fail-loudly path.
struct FakeHealthProbe final : tsys::IHealthHostProbe {
    int read_calls = 0;
    bool fail = false;

    tsys::HostReading read() override {
        ++read_calls;
        if (fail) {
            throw tsys::SyscallError("sample_unavailable", "host interfaces unavailable");
        }
        const std::uint64_t step = static_cast<std::uint64_t>(read_calls);
        tsys::HostReading reading;
        reading.cpu_busy = step * 400;    // 40% of the total delta
        reading.cpu_total = step * 1000;
        reading.memory_used = 512 + step;
        reading.memory_total = 4096;
        reading.processes = 40 + static_cast<std::uint32_t>(read_calls);
        reading.threads = 100 + static_cast<std::uint32_t>(read_calls);
        return reading;
    }
};

// The REAL HealthState + HealthTelemetryControl over the fake probe, with a
// fixed clock: the matrix tests exercise the production control path, not a
// hand-written telemetry stub.
struct TelemetryFixture {
    FakeHealthProbe probe;
    tsys::HealthState state{1, 60, tsys::HealthMillis{0}};
    std::shared_ptr<tsys::ITelemetryControl> control;

    TelemetryFixture() {
        control = tsys::make_health_telemetry_control(
            state, probe, [] { return tsys::HealthMillis{5000}; });
    }
};

// The argument shape each verb of the implemented families dispatches with
// (destructive rows carry the confirmation marker as args[0]).
std::vector<std::string> args_for(const std::string& verb) {
    if (verb == "delete") {
        return {tsys::kConfirmMarker, "a"};
    }
    if (verb == "move" || verb == "copy") {
        return {tsys::kConfirmMarker, "a", "b"};
    }
    if (verb == "start") {
        return {"prog"};
    }
    if (verb == "stop") {
        return {"4242"};
    }
    return {};
}

// Verbs whose families are implemented in this build (all of them on both
// platforms; the matrix itself is also exercised with a crafted row below).
bool is_implemented_family(tsys::Family family) {
    return family == tsys::Family::Process || family == tsys::Family::File;
}

} // namespace

TEST_CASE("an unknown verb is rejected with no system effect at all") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher;
    fakes.wire_into(dispatcher);

    const tsys::Outcome outcome = dispatcher.dispatch("shellexec", {"powershell", "-e", "dir"});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.code == "unknown_command");
    CHECK(outcome.message.find("shellexec") != std::string::npos);

    // The closed-set gate sits before every capability call: the accounts
    // below are the proof that nothing ran (spec: no system effect).
    CHECK(fakes.process->list_calls == 0);
    CHECK(fakes.process->start_calls == 0);
    CHECK(fakes.process->stop_calls == 0);
    CHECK(fakes.file->list_calls == 0);
    CHECK(fakes.file->remove_calls == 0);
    CHECK(fakes.file->move_calls == 0);
    CHECK(fakes.file->copy_calls == 0);

    // The rejection is also the END of the story for that verb: the same
    // dispatcher object serves a real command immediately after.
    const tsys::Outcome after = dispatcher.dispatch("ls", {});
    CHECK(after.ok);
    CHECK(fakes.file->list_calls == 1);
}

TEST_CASE("the empty command word is outside the closed set") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher;
    fakes.wire_into(dispatcher);

    const tsys::Outcome outcome = dispatcher.dispatch("", {});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.code == "unknown_command");
    CHECK(fakes.total_family_calls() == 0);
}

TEST_CASE("every defined command resolves to a definitive outcome") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher;
    fakes.wire_into(dispatcher);

    const std::vector<tsys::CommandSpec>& set = tsys::command_set();
    REQUIRE(set.size() == 20); // the accepted set: v2's 19 + the telemetry verb
    for (std::size_t i = 0; i < set.size(); ++i) {
        for (std::size_t j = i + 1; j < set.size(); ++j) {
            REQUIRE(set[j].verb != set[i].verb); // uniqueness keeps the set closed
        }
    }

    for (const tsys::CommandSpec& spec : set) {
        CAPTURE(spec.verb);
        const tsys::Outcome outcome = dispatcher.dispatch(spec.verb, args_for(spec.verb));
        // "never an unclassified failure": every one of the 20 verbs ends in
        // a recognized class, success included.
        const bool definitive = outcome.ok || !outcome.code.empty();
        REQUIRE(definitive);
        if (is_implemented_family(spec.family)) {
            // With the family wired, supported verbs execute.
            CHECK(outcome.ok);
            CHECK(outcome.code == "ok");
        } else {
            // Families not wired into this test's dispatcher hold the
            // structured unsupported resolution (naming platform + family).
            CHECK_FALSE(outcome.ok);
            CHECK(outcome.code == "unsupported_platform");
            CHECK(outcome.message.find(spec.verb) != std::string::npos);
        }
    }
    // The 8 implemented-family verbs all executed through their interfaces:
    // list, start, stop (by pid - no enumeration needed), ls, delete, move,
    // copy in that loop.
    CHECK(fakes.process->list_calls == 1);
    CHECK(fakes.process->start_calls == 1);
    CHECK(fakes.process->stop_calls == 1);
    CHECK(fakes.file->list_calls == 1);
    CHECK(fakes.file->remove_calls == 1);
    CHECK(fakes.file->move_calls == 1);
    CHECK(fakes.file->copy_calls == 1);
}

TEST_CASE("out-of-matrix verbs return the structured refusal naming platform and family") {
    Fakes fakes;

    // The matrix mechanism test: one row pinned to a platform that does not
    // resolve it, exactly the shape a platform gap takes in real data (the
    // production rows keep all 20 verbs on both OSes; Wayland/partial-OS
    // gaps land as data edits in the family tasks).
    std::vector<tsys::CommandSpec> crafted = tsys::command_set();
    for (tsys::CommandSpec& spec : crafted) {
        if (std::string(spec.verb) == "screenshot") {
            spec.on_linux = false; // e.g. the platform where capture does not exist
        }
    }
    tsys::CommandDispatcher dispatcher(tsys::Platform::Linux, crafted);

    const tsys::Outcome outcome = dispatcher.dispatch("screenshot", {});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.code == "unsupported_platform");
    CHECK(outcome.message.find("linux") != std::string::npos);
    CHECK(outcome.message.find("capture") != std::string::npos);
    CHECK(outcome.message.find("screenshot") != std::string::npos);
    CHECK(fakes.total_family_calls() == 0);

    // Spec: "the same session continues to serve later commands" - the
    // refusal cannot wedge the dispatch layer either.
    fakes.wire_into(dispatcher);
    const tsys::Outcome after = dispatcher.dispatch("ls", {});
    CHECK(after.ok);
    CHECK(after.lines.size() == 2);
}

TEST_CASE("supported-but-unimplemented families hold the structured unsupported result") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher; // real platform, real matrix, no impls wired

    const tsys::Outcome outcome = dispatcher.dispatch("shutdown", {"confirm"});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.code == "unsupported_platform");
    CHECK(outcome.message.find("shutdown") != std::string::npos);
    CHECK(outcome.message.find("power") != std::string::npos);
    CHECK(outcome.message.find(tsys::platform_name(dispatcher.platform())) !=
          std::string::npos);
    // The gate refused before any capability call (no family is wired for
    // power, and the process/file fakes stay entirely untouched).
    CHECK(fakes.total_family_calls() == 0);
}

TEST_CASE("destructive verbs refuse at the agent layer without the confirmation token") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher;
    fakes.wire_into(dispatcher);

    for (const std::vector<std::string>& unconfirmed :
         {std::vector<std::string>{}, std::vector<std::string>{"guarded.txt"},
          std::vector<std::string>{"yes"}}) {
        CAPTURE(unconfirmed.size());
        const tsys::Outcome outcome = dispatcher.dispatch("delete", unconfirmed);
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.code == "missing_confirmation");
        CHECK(outcome.message.find("confirm") != std::string::npos);
        CHECK(fakes.file->remove_calls == 0); // nothing past the gate
    }

    // move and copy carry the same agent-layer gate (task 6.3 binding:
    // destructive = replace destinations).
    CHECK(dispatcher.dispatch("move", {"a", "b"}).code == "missing_confirmation");
    CHECK(dispatcher.dispatch("copy", {"a", "b"}).code == "missing_confirmation");
    CHECK(fakes.file->move_calls == 0);
    CHECK(fakes.file->copy_calls == 0);
    CHECK(dispatcher.dispatch("shutdown", {}).code == "unsupported_platform");
    CHECK(dispatcher.dispatch("restart", {}).code == "unsupported_platform");
}

TEST_CASE("a confirmed destructive verb executes at the family interface") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher;
    fakes.wire_into(dispatcher);

    const tsys::Outcome outcome =
        dispatcher.dispatch("delete", {tsys::kConfirmMarker, "guarded.txt"});
    CHECK(outcome.ok);
    CHECK(fakes.file->remove_calls == 1);
    CHECK(fakes.file->last_removed == "guarded.txt"); // the marker left the argv
}

TEST_CASE("parameters reach the family as literal typed data") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher;
    fakes.wire_into(dispatcher);

    // The metacharacter set of the parameters requirement, riding argv data:
    const std::vector<std::string> literal_args = {"a;b", "c|d", "e&f", "g>h"};
    const tsys::Outcome outcome = dispatcher.dispatch("start", literal_args);
    CHECK(outcome.ok);
    REQUIRE(fakes.process->start_log.size() == 1);
    CHECK(fakes.process->start_log[0] == literal_args); // argv unchanged, byte for byte

    // File paths are the same contract: the literal name is what the family
    // sees (and looks up - see file_family_test for the real-disk variant).
    dispatcher.dispatch("delete", {tsys::kConfirmMarker, "a;b|c&d>e"});
    CHECK(fakes.file->last_removed == "a;b|c&d>e");
}

// ---- add-health-telemetry: the telemetry family (tasks 4.1/4.2) -------------

TEST_CASE("health is the 20th verb, in the telemetry family, on both OSes") {
    const tsys::CommandSpec* health = tsys::find_command("health");
    REQUIRE(health != nullptr);
    CHECK(health->family == tsys::Family::Telemetry);
    CHECK_FALSE(health->destructive);
    CHECK(tsys::supported_on(*health, tsys::Platform::Windows));
    CHECK(tsys::supported_on(*health, tsys::Platform::Linux));
    CHECK(std::string(tsys::family_name(tsys::Family::Telemetry)) == "telemetry");

    // Exact-match closed set: near misses stay outside it.
    CHECK(tsys::find_command("healthz") == nullptr);
    CHECK(tsys::find_command("Health") == nullptr);
    CHECK(tsys::command_set().size() == 20);
}

TEST_CASE("the matrix and family gates hold for the health row") {
    // Gate 2 (row/platform) precedes gate 5 (argument checks): pin health off
    // Linux and even a parameter-carrying call refuses structurally with no
    // sampling read at all.
    std::vector<tsys::CommandSpec> crafted = tsys::command_set();
    for (tsys::CommandSpec& spec : crafted) {
        if (std::string(spec.verb) == "health") {
            spec.on_linux = false;
        }
    }
    TelemetryFixture fixture;
    tsys::CommandDispatcher pinned(tsys::Platform::Linux, crafted);
    pinned.set_telemetry_control(fixture.control);
    const tsys::Outcome outside = pinned.dispatch("health", {"anything"});
    CHECK_FALSE(outside.ok);
    CHECK(outside.code == "unsupported_platform");
    CHECK(outside.message.find("linux") != std::string::npos);
    CHECK(outside.message.find("telemetry") != std::string::npos);
    CHECK(fixture.probe.read_calls == 0);

    // Gate 3 (family availability) also precedes gate 5: a supported but
    // unwired health refuses as unsupported, not as invalid_args.
    tsys::CommandDispatcher unwired;
    const tsys::Outcome gap = unwired.dispatch("health", {"anything"});
    CHECK_FALSE(gap.ok);
    CHECK(gap.code == "unsupported_platform");
    CHECK(gap.message.find("health") != std::string::npos);
    CHECK(gap.message.find("telemetry") != std::string::npos);
}

TEST_CASE("health rejects every parameter without sampling") {
    TelemetryFixture fixture;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_telemetry_control(fixture.control);

    for (const std::vector<std::string>& args :
         {std::vector<std::string>{""}, std::vector<std::string>{"1"},
          std::vector<std::string>{"confirm"}}) {
        CAPTURE(args.size());
        const tsys::Outcome outcome = dispatcher.dispatch("health", args);
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.code == "invalid_args");
        CHECK(outcome.message.find("health") != std::string::npos);
    }
    // The arity gate fired before the capability: the probe never ran and the
    // window is still empty.
    CHECK(fixture.probe.read_calls == 0);
    CHECK_FALSE(fixture.state.snapshot(tsys::HealthMillis{5000}).has_latest);
}

TEST_CASE("health serves the sampler's one-line snapshot") {
    TelemetryFixture fixture;
    fixture.control->sample(); // baseline-only first tick (no fabricated CPU)
    fixture.control->sample();
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_telemetry_control(fixture.control);

    const tsys::Outcome outcome = dispatcher.dispatch("health", {});
    CHECK(outcome.ok);
    CHECK(outcome.code == "ok");
    REQUIRE(outcome.lines.size() == 1);
    const std::string& snapshot = outcome.lines.front();
    CHECK(snapshot.front() == '{');
    CHECK(snapshot.back() == '}');
    CHECK(snapshot.find("\"uptime_seconds\"") != std::string::npos);
    CHECK(snapshot.find("\"latest\"") != std::string::npos);
    CHECK(snapshot.find("\"window\"") != std::string::npos);
    CHECK(snapshot.find("\"sample_age_ms\"") != std::string::npos);
    CHECK(snapshot.find("\"interval_s\"") != std::string::npos);
    CHECK(snapshot.find('\n') == std::string::npos); // the D5 single line
}

TEST_CASE("a failing probe maps to sample_unavailable, never zeros") {
    TelemetryFixture fixture;
    fixture.probe.fail = true;
    fixture.control->sample(); // the pump cadence attempt fails
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_telemetry_control(fixture.control);

    const tsys::Outcome outcome = dispatcher.dispatch("health", {});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.code == "sample_unavailable");
    CHECK(outcome.message.find("host interfaces unavailable") != std::string::npos);
    CHECK(fixture.probe.read_calls == 1);
    // Nothing was fabricated into the window.
    CHECK_FALSE(fixture.state.snapshot(tsys::HealthMillis{5000}).has_latest);

    // Honesty is per attempt: the next good sample clears the refusal.
    fixture.probe.fail = false;
    fixture.control->sample();
    const tsys::Outcome recovered = dispatcher.dispatch("health", {});
    CHECK(recovered.ok);
}

TEST_CASE("health before the first sample refuses instead of an empty snapshot") {
    TelemetryFixture fixture;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_telemetry_control(fixture.control);

    const tsys::Outcome outcome = dispatcher.dispatch("health", {});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.code == "sample_unavailable");
    CHECK(fixture.probe.read_calls == 0); // the command never samples on demand
}
