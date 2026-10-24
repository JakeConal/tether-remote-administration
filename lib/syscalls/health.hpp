// lib/syscalls/health.hpp - the health sampler engine (add-health-telemetry,
// tasks 2.1/2.2, design D2/D3/D5/D9) plus the dispatcher-facing telemetry
// control (task 4.2).
//
// HealthState is the pure, testable core: it owns the rolling window, the
// agent-self counters (uptime / reconnects), the staleness bookkeeping, and
// every piece of aggregate math. It does NOT sample anything: tick(now,
// reading) is fed raw host counter readings by the caller (phase 3 adds the
// OSbacked IHealthHostProbe implementations; the agent's established-session
// pump loop drives the cadence, D3 - no thread, no queue).
//
// Layer rules honored: no common/protocol/transport headers are included
// (tether-syscalls stays self-contained), so the constructor takes the
// telemetry cadence as plain int seconds rather than a config type. The
// snapshot JSON is numeric-only and therefore hand-rolled (no JSON library
// in this layer); it is single-line and matches design D5's field names.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace tether::syscalls {

// Millis in the caller's clock domain (the agent's own steady_clock ms and
// every test's fake clock). Interval math here never calls a clock itself.
using HealthMillis = std::int64_t;

// One instant raw host reading handed to tick(). The CPU fields are
// *cumulative* counter values (busy and total, probe-defined granularity
// since boot); HealthState diffs consecutive readings into the percentage,
// so a first tick has no CPU value (baseline-only, never a fabricated 0%).
struct HostReading {
    std::uint64_t cpu_busy = 0;
    std::uint64_t cpu_total = 0;
    std::uint64_t memory_used = 0;
    std::uint64_t memory_total = 0;
    std::uint32_t processes = 0;
    std::uint32_t threads = 0;
};

// The OS-backed host probe (phase 3, tasks 3.1/3.2). One `read()` per
// cadence tick; the implementations touch their platform interfaces directly
// (Windows: GetSystemTimes / GlobalMemoryStatusEx / Toolhelp32;
// Linux: /proc/stat, /proc/meminfo, /proc/loadavg, the /proc walk) and the
// dispatcher mapping (phase 4) turns any failure into the structured
// `sample_unavailable` refusal - noise in host sampling never fabricates
// values (health-telemetry spec).
class IHealthHostProbe {
public:
    virtual ~IHealthHostProbe() = default;
    virtual HostReading read() = 0; // throws SyscallError("sample_unavailable", ...) on failure
};

// The per-OS probe factory (same seams pattern as make_process_control).
std::unique_ptr<IHealthHostProbe> make_health_host_probe();

// The direct, numeric snapshot (health's answer to `health`). Field names
// mirror the JSON exactly (design D5); the optionals exist because the
// contract forbids fabricated values: a window that has not ticked yet, or a
// window of baseline-only samples, reports host values but no CPU aggregate.
struct HealthSnapshot {
    // Agent-self counters.
    std::uint64_t uptime_seconds = 0;
    unsigned reconnects = 0;
    // Latest host sample (has_latest absent is only possible before any
    // tick: the command layer surfaces that as a structured refusal).
    bool has_latest = false;
    std::optional<double> cpu_percent;
    std::uint64_t memory_used = 0;
    std::uint64_t memory_total = 0;
    std::uint32_t processes = 0;
    std::uint32_t threads = 0;
    HealthMillis sampled_at_ms = 0;
    // Window aggregates over the rolling span.
    int span_seconds = 0;
    std::size_t ticks = 0;
    std::optional<double> cpu_percent_avg;
    std::optional<double> cpu_percent_max;
    std::optional<std::uint64_t> memory_used_max;
    // Effective tick coverage of the window: ticks / expected ticks
    // (expected = span / interval), <= 1. The honest staleness companion:
    // a delayed sampler reports fewer ticks, not a denser-than-real window.
    double window_coverage = 0.0;
    // Freshness.
    HealthMillis sample_age_ms = 0;
    int interval_s = 0;

    // One-line JSON in the design D5 shape: uptime_seconds, reconnects,
    // latest{cpu_percent, memory_used, memory_total, processes, threads,
    // sampled_at_ms}, window{span_seconds, ticks, cpu_percent_avg?,
    // cpu_percent_max?, memory_used_max?, window_coverage}, sample_age_ms,
    // interval_s. `latest.cpu_percent` is JSON null until the first
    // post-baseline tick; window aggregate keys are omitted (never
    // zero-filled) until their data exists.
    std::string json() const;
};

class HealthState {
public:
    // started_at anchors uptime at agent process start (the constructor call
    // site in the agent's main is that moment); the sampler keeps counting
    // across every session establishment and redial.
    HealthState(int interval_s, int window_s, HealthMillis started_at);

    // One successful session establishment. The FIRST call is the anchor and
    // contributes no reconnect; every later call (a true re-registration
    // after a drop) increments the reconnect count.
    void note_established(HealthMillis now);

    // Advance the sampler by one cadence step with a fresh host reading.
    // The first tick stores the cumulative counter baseline and records the
    // reading without a CPU percentage; later ticks diff them. A counter
    // that went backwards (wrap/reboot artifact) re-baselines instead of
    // producing a bogus percentage.
    void tick(HealthMillis now, const HostReading& reading);

    HealthSnapshot snapshot(HealthMillis now) const;
    std::string snapshot_json(HealthMillis now) const { return snapshot(now).json(); }

private:
    struct Sample {
        HealthMillis wall_ms = 0;
        std::optional<double> cpu_percent;
        std::uint64_t memory_used = 0;
        std::uint64_t memory_total = 0;
        std::uint32_t processes = 0;
        std::uint32_t threads = 0;
    };

    // Window sizing: tick-indexed (design's risk note), expected ticks =
    // span/interval with a floor of 1, so a degenerate config still holds a
    // latest sample.
    std::size_t expected_ticks() const;

    const int interval_s_;
    const int window_s_;
    const HealthMillis started_at_;

    bool established_ = false;
    unsigned reconnects_ = 0;

    bool has_counter_base_ = false;
    std::uint64_t base_busy_ = 0;
    std::uint64_t base_total_ = 0;

    std::deque<Sample> window_;
};

// ---- the dispatcher-facing telemetry surface (task 4.2) --------------------
//
// The agent's pump loop calls sample() at the configured cadence (task 5.2);
// the `health` family call renders snapshot() on demand. Keeping sampling and
// rendering apart preserves D3's inline tick: the command path never
// fabricates freshness, and the window's tick cadence stays exactly the
// configured interval (an extra sample per poll would silently halve the
// window's wall span).
//
// A host-sampling failure is sticky until the next SUCCESSFUL sample, so a
// `health` that lands after a failed probe attempt refuses loudly instead of
// serving stale values as current (health-telemetry spec: "Sampling failure
// fails loudly, never with zeros").

// Monotonic milliseconds on the agent's steady clock (the HealthMillis
// domain every caller uses: the pump loop and the control's own reads).
HealthMillis steady_clock_millis();

class ITelemetryControl {
public:
    virtual ~ITelemetryControl() = default;

    // One host sample into the rolling window. The caller owns the cadence.
    // Never throws: a failed attempt is remembered for snapshot() to refuse.
    virtual void sample() = 0;

    // The design D5 snapshot JSON (single line). Throws
    // SyscallError("sample_unavailable", ...) when the latest sampling
    // attempt failed or nothing has been sampled yet - the family layer maps
    // that to the wire-visible structured refusal.
    virtual std::string snapshot() = 0;
};

// The production implementation over the shared sampler and OS probe.
class HealthTelemetryControl final : public ITelemetryControl {
public:
    HealthTelemetryControl(HealthState& state, IHealthHostProbe& probe,
                           std::function<HealthMillis()> clock);
    void sample() override;
    std::string snapshot() override;

private:
    HealthState& state_;
    IHealthHostProbe& probe_;
    std::function<HealthMillis()> clock_;
    std::string last_failure_; // empty == the latest attempt succeeded (or none yet)
};

std::shared_ptr<ITelemetryControl> make_health_telemetry_control(
    HealthState& state, IHealthHostProbe& probe,
    std::function<HealthMillis()> clock = steady_clock_millis);

} // namespace tether::syscalls
