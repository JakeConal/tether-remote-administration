// console/link_stats.hpp - console-local link measurements (design D7, task
// 6.4): per-issued-seq end-to-end latency and interval throughput math.
//
// Both are pure and clock-injected so the unit suite feeds canned timestamps;
// TcpOperConsole owns one of each and samples them through
// IOperConsole::link_telemetry() while the monitor renders an observation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>

namespace tether::cli {

// Issue -> terminal latency keyed by the session's command seq. The first
// terminal hook stamps completion; a dispatch that never completes leaves the
// pending entry to be overwritten when a later session reuses the seq.
class LatencySampler {
public:
    // Recent completed samples kept for the median/max reading. A monitor at
    // one poll per second stays far below this cap for hours.
    static constexpr std::size_t kCapacity = 4096;

    void issued(std::uint32_t seq, std::int64_t at_ms);
    // No-op when `seq` was never issued (or already completed).
    void completed(std::uint32_t seq, std::int64_t at_ms);

    std::optional<std::int64_t> latest_ms() const;
    // Lower median of the completed samples (nearest-rank; even counts pick
    // the lower middle value).
    std::optional<std::int64_t> p50_ms() const;
    std::optional<std::int64_t> max_ms() const;
    std::size_t samples() const noexcept { return completed_.size(); }

    void clear();

private:
    std::map<std::uint32_t, std::int64_t> pending_;
    std::deque<std::int64_t> completed_;
};

// Throughput over the recent interval: each observe() returns the byte rate
// since the previous observation. The first observation has no interval and
// returns 0; a non-monotonic clock or a shrunken counter (a fresh totals
// object) re-anchors rather than reporting a bogus rate.
class ByteRateMeter {
public:
    double observe(std::uint64_t total_bytes, std::int64_t at_ms);

    void reset();

private:
    std::optional<std::uint64_t> previous_bytes_;
    std::optional<std::int64_t> previous_at_ms_;
};

} // namespace tether::cli
