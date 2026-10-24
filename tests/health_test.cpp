// add-health-telemetry tasks 2.1/2.2: the health sampler engine, over a
// fake clock (every instant is passed in; no test sleeps). Pins: window
// fill/drain, the baseline-only first tick (no fabricated CPU), aggregate
// math over the configured span, honest staleness (sample age + coverage),
// the uptime/reconnect counters, counter-reset robustness, and the
// single-line snapshot JSON field set. Tasks 3.1/3.2 add one REAL-clock
// smoke test for the OS host probe (plausible ranges; the 50 ms real gap
// there is what makes consecutive cumulative CPU counters differ).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "health.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

namespace {

using tether::syscalls::HealthMillis;
using tether::syscalls::HealthSnapshot;
using tether::syscalls::HealthState;
using tether::syscalls::HostReading;
using tether::syscalls::make_health_host_probe;

HostReading reading(std::uint64_t busy, std::uint64_t total, std::uint64_t used,
                    std::uint64_t total_mem, std::uint32_t processes,
                    std::uint32_t threads) {
    HostReading r;
    r.cpu_busy = busy;
    r.cpu_total = total;
    r.memory_used = used;
    r.memory_total = total_mem;
    r.processes = processes;
    r.threads = threads;
    return r;
}

// Parses the one-line snapshot; every key the design documents must exist.
nlohmann::json parsed_json(const HealthState& state, HealthMillis now) {
    const std::string text = state.snapshot_json(now);
    REQUIRE(text.find('\n') == std::string::npos); // single line, no header
    return nlohmann::json::parse(text);
}

constexpr double kEpsilon = 0.001;

} // namespace

TEST_CASE("first tick is baseline-only: no fabricated CPU, host values present") {
    HealthState state(1, 60, HealthMillis{0});
    state.note_established(HealthMillis{0});
    state.tick(HealthMillis{1000},
               reading(0, 0, 4'000'000'000, 16'000'000'000, 214, 3406));

    const tether::syscalls::HealthSnapshot snap = state.snapshot(HealthMillis{1000});
    CHECK(snap.has_latest);
    CHECK_FALSE(snap.cpu_percent.has_value());
    CHECK(snap.memory_used == 4'000'000'000);
    CHECK(snap.memory_total == 16'000'000'000);
    CHECK(snap.processes == 214);
    CHECK(snap.threads == 3406);
    CHECK(snap.sampled_at_ms == 1000);
    CHECK(snap.ticks == 1);
    CHECK_FALSE(snap.cpu_percent_avg.has_value());
    CHECK_FALSE(snap.cpu_percent_max.has_value());
    REQUIRE(snap.memory_used_max.has_value());
    CHECK(*snap.memory_used_max == 4'000'000'000);
}

TEST_CASE("later ticks diff the counters into a percentage") {
    HealthState state(1, 60, HealthMillis{0});
    state.tick(HealthMillis{1000}, reading(0, 1000, 10, 20, 1, 2));
    state.tick(HealthMillis{2000}, reading(400, 2000, 11, 20, 1, 2));

    const tether::syscalls::HealthSnapshot snap = state.snapshot(HealthMillis{2000});
    REQUIRE(snap.cpu_percent.has_value());
    CHECK(*snap.cpu_percent == doctest::Approx(40.0).epsilon(kEpsilon));
    REQUIRE(snap.cpu_percent_avg.has_value());
    CHECK(*snap.cpu_percent_avg == doctest::Approx(40.0).epsilon(kEpsilon));
    REQUIRE(snap.cpu_percent_max.has_value());
    CHECK(*snap.cpu_percent_max == doctest::Approx(40.0).epsilon(kEpsilon));
}

TEST_CASE("aggregates cover the whole recorded span: cpu avg/max, memory max") {
    HealthState state(1, 60, HealthMillis{0});
    // Cumulative busy deltas 400 (baseline, no CPU), then 450/500/550/600 per
    // 1000 total -> cpu 45, 50, 55, 60 (the first tick never carries a value;
    // baseline-only is the design D2 contract, pinned by the tests above).
    std::uint64_t busy = 0;
    std::uint64_t used = 100;
    for (HealthMillis t{1000}; t <= HealthMillis{5000}; t += 1000) {
        busy += 400 + static_cast<std::uint64_t>(t / 1000 - 1) * 50;
        used += 100;
        state.tick(t, reading(busy, static_cast<std::uint64_t>(t), used, 20, 7, 9));
    }

    const tether::syscalls::HealthSnapshot snap = state.snapshot(HealthMillis{5000});
    REQUIRE(snap.cpu_percent_avg.has_value());
    CHECK(*snap.cpu_percent_avg == doctest::Approx(52.5).epsilon(kEpsilon));
    REQUIRE(snap.cpu_percent_max.has_value());
    CHECK(*snap.cpu_percent_max == doctest::Approx(60.0).epsilon(kEpsilon));
    REQUIRE(snap.memory_used_max.has_value());
    CHECK(*snap.memory_used_max == 600);
    CHECK(snap.ticks == 5);
    // latest is the newest tick (t = 5 s)
    CHECK(*snap.cpu_percent == doctest::Approx(60.0).epsilon(kEpsilon));
    CHECK(snap.sample_age_ms == 0);
}

TEST_CASE("window keeps the expected tick count and drains the oldest") {
    HealthState state(1, 3, HealthMillis{0}); // window 3 s, interval 1 s -> 3 ticks
    for (HealthMillis t{1000}; t <= HealthMillis{5000}; t += 1000) {
        state.tick(t, reading(0, 1000, t, 20, 1, 1));
    }
    const tether::syscalls::HealthSnapshot snap = state.snapshot(HealthMillis{5050});
    CHECK(snap.ticks == 3);
    CHECK(snap.span_seconds == 3);
    CHECK(snap.sampled_at_ms == 5000);            // oldest drained, newest kept
    CHECK(snap.sample_age_ms == 50);
    REQUIRE(snap.memory_used_max.has_value());
    CHECK(*snap.memory_used_max == 5000);         // stale maxima left the window
}

TEST_CASE("sample age grows honestly across gaps instead of freezing values") {
    HealthState state(1, 60, HealthMillis{0});
    state.tick(HealthMillis{1000}, reading(0, 1000, 10, 20, 1, 2));
    state.tick(HealthMillis{2000}, reading(400, 2000, 11, 20, 1, 2));
    // Nothing samples for ten seconds (a busy dispatcher behind a long
    // command); the window is tick-indexed and simply holds two real ticks.
    const tether::syscalls::HealthSnapshot snap = state.snapshot(HealthMillis{12000});
    CHECK(snap.sample_age_ms == 10000);
    CHECK(snap.ticks == 2);
    REQUIRE(snap.cpu_percent.has_value());
    CHECK(*snap.cpu_percent == doctest::Approx(40.0).epsilon(kEpsilon)); // still the honest last reading
    CHECK(snap.sampled_at_ms == 2000);
    CHECK(snap.window_coverage == doctest::Approx(2.0 / 60.0).epsilon(kEpsilon));
    REQUIRE(snap.memory_used_max.has_value());
    CHECK(*snap.memory_used_max == 11);
}

TEST_CASE("uptime anchors at process start; reconnects count real redials") {
    HealthState state(1, 60, HealthMillis{0});
    state.note_established(HealthMillis{100});
    CHECK(state.snapshot(HealthMillis{10100}).reconnects == 0);
    CHECK(state.snapshot(HealthMillis{10100}).uptime_seconds == 10);

    state.note_established(HealthMillis{20000}); // first drop + re-establish
    state.note_established(HealthMillis{30000}); // second
    CHECK(state.snapshot(HealthMillis{30000}).reconnects == 2);
    // Uptime still counts from the original process start, not the last dial.
    CHECK(state.snapshot(HealthMillis{61000}).uptime_seconds == 61);
}

TEST_CASE("counter reset re-baselines instead of reporting bogus CPU") {
    HealthState state(1, 60, HealthMillis{0});
    state.tick(HealthMillis{1000}, reading(0, 1000, 10, 20, 1, 2));
    state.tick(HealthMillis{2000}, reading(400, 2000, 11, 20, 1, 2));
    // The counters went backwards (reboot artifact): no percentage this tick.
    state.tick(HealthMillis{3000}, reading(10, 20, 12, 20, 1, 2));
    const tether::syscalls::HealthSnapshot snap = state.snapshot(HealthMillis{3000});
    CHECK_FALSE(snap.cpu_percent.has_value());
    CHECK(snap.ticks == 3);
    // And the next steady tick measures on top of the re-baseline normally.
    state.tick(HealthMillis{4000}, reading(30, 40, 13, 20, 1, 2));
    const tether::syscalls::HealthSnapshot after = state.snapshot(HealthMillis{4000});
    REQUIRE(after.cpu_percent.has_value());
    CHECK(*after.cpu_percent == doctest::Approx(100.0).epsilon(kEpsilon));
}

TEST_CASE("empty state reports no latest and zero coverage without data") {
    HealthState state(1, 60, HealthMillis{0});
    const tether::syscalls::HealthSnapshot snap = state.snapshot(HealthMillis{5000});
    CHECK_FALSE(snap.has_latest);
    CHECK(snap.ticks == 0);
    CHECK(snap.window_coverage == doctest::Approx(0.0));
    CHECK(snap.uptime_seconds == 5);
    CHECK(snap.reconnects == 0);
}

// ---- task 2.2: the single-line JSON field set (design D5) -------------------

TEST_CASE("snapshot JSON emits every documented key with real values") {
    HealthState state(1, 60, HealthMillis{0});
    state.note_established(HealthMillis{0});
    state.tick(HealthMillis{1000}, reading(0, 1000, 6'291'456, 16'777'216, 214, 3406));
    state.tick(HealthMillis{2000}, reading(400, 2000, 8'388'608, 16'777'216, 215, 3406));
    state.tick(HealthMillis{3412}, reading(1150, 3000, 9'000'000, 16'777'216, 216, 3407));

    const nlohmann::json parsed = parsed_json(state, HealthMillis{3412});

    // Top-level keys in the documented set.
    REQUIRE(parsed.is_object());
    CHECK(parsed.contains("uptime_seconds"));
    CHECK(parsed.contains("reconnects"));
    CHECK(parsed.contains("latest"));
    CHECK(parsed.contains("window"));
    CHECK(parsed.contains("sample_age_ms"));
    CHECK(parsed.contains("interval_s"));
    CHECK(parsed["uptime_seconds"] == 3);
    CHECK(parsed["reconnects"] == 0);
    CHECK(parsed["sample_age_ms"] == 0);
    CHECK(parsed["interval_s"] == 1);

    // latest.*
    const nlohmann::json& latest = parsed["latest"];
    CHECK(latest.contains("cpu_percent"));
    CHECK(latest["cpu_percent"].get<double>() ==
          doctest::Approx(75.0).epsilon(kEpsilon)); // (1150-400)/1000
    CHECK(latest["memory_used"] == 9'000'000);
    CHECK(latest["memory_total"] == 16'777'216);
    CHECK(latest["processes"] == 216);
    CHECK(latest["threads"] == 3407);
    CHECK(latest["sampled_at_ms"] == 3412);

    // window.*
    const nlohmann::json& window = parsed["window"];
    CHECK(window.contains("span_seconds"));
    CHECK(window.contains("ticks"));
    CHECK(window.contains("cpu_percent_avg"));
    CHECK(window.contains("cpu_percent_max"));
    CHECK(window.contains("memory_used_max"));
    CHECK(window.contains("window_coverage"));
    CHECK(window["span_seconds"] == 60);
    CHECK(window["ticks"] == 3);
    CHECK(window["cpu_percent_avg"].get<double>() ==
          doctest::Approx((40.0 + 75.0) / 2.0).epsilon(kEpsilon)); // baseline tick carries no CPU
    CHECK(window["cpu_percent_max"].get<double>() == doctest::Approx(75.0).epsilon(kEpsilon));
    CHECK(window["memory_used_max"] == 9'000'000);
    CHECK(window["window_coverage"].get<double>() == doctest::Approx(3.0 / 60.0).epsilon(kEpsilon));
}

TEST_CASE("baseline-only snapshot JSON carries null CPU instead of a zero") {
    HealthState state(1, 60, HealthMillis{0});
    state.tick(HealthMillis{1000}, reading(0, 0, 10, 20, 1, 2));

    const nlohmann::json parsed = parsed_json(state, HealthMillis{1000});
    REQUIRE(parsed.contains("latest"));
    REQUIRE(parsed["latest"].contains("cpu_percent"));
    CHECK(parsed["latest"]["cpu_percent"].is_null());
    // Window aggregates do not exist yet: omitted, never fabricated.
    const nlohmann::json& window = parsed["window"];
    CHECK_FALSE(window.contains("cpu_percent_avg"));
    CHECK_FALSE(window.contains("cpu_percent_max"));
    CHECK(window.contains("window_coverage"));
}

// Tasks 3.1/3.2: the one test that touches the real OS - the Windows probe
// (GetSystemTimes / GlobalMemoryStatusEx / Toolhelp32) or the Linux probe
// (/proc/stat, /proc/meminfo, /proc/loadavg, the /proc walk), whichever this
// build is. A ~50 ms real gap between two reads gives the cumulative CPU
// counters a chance to move; the smoke test pins plausible ranges only -
// presence is not asserted, since a dead-idle interval legitimately yields a
// CPU-less sample (the engine never fabricates).
TEST_CASE("real host probe smoke test: plausible ranges across a real gap") {
    const auto wall_now = []() -> HealthMillis {
        return HealthMillis{std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count()};
    };

    auto probe = make_health_host_probe();
    const HealthMillis start = wall_now();

    HealthState state(1, 60, start);
    state.note_established(start); // first establishment: not a reconnect
    state.tick(start, probe->read());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const HealthMillis end = wall_now();
    state.tick(end, probe->read());

    const HealthSnapshot snap = state.snapshot(end);
    CHECK(snap.has_latest);
    // Plausible range whenever CPU IS present; its presence is not required,
    // because the engine legitimately yields a CPU-less sample when the OS
    // counters did not move between the two reads (the honest counter-reset
    // path). The fake-clock suite already pins the diff math itself.
    if (snap.cpu_percent.has_value()) {
        CHECK(*snap.cpu_percent >= 0.0);
        CHECK(*snap.cpu_percent <= 100.0);
    }
    CHECK(snap.memory_total > 0);
    CHECK(snap.memory_used <= snap.memory_total);
    CHECK(snap.processes > 0); // this test process exists on every supported OS
    CHECK(snap.threads > 0);   // and it owns at least this test thread
    CHECK(snap.uptime_seconds == 0); // a sub-second session, honestly floored at 0
    CHECK(snap.reconnects == 0);
}
