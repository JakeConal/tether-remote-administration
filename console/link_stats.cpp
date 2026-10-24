// console/link_stats.cpp - latency and throughput math for the monitor.
#include "link_stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace tether::cli {

void LatencySampler::issued(std::uint32_t seq, std::int64_t at_ms) {
    pending_[seq] = at_ms;
}

void LatencySampler::completed(std::uint32_t seq, std::int64_t at_ms) {
    const auto it = pending_.find(seq);
    if (it == pending_.end()) {
        return;
    }
    const std::int64_t latency = at_ms > it->second ? at_ms - it->second : 0;
    pending_.erase(it);
    completed_.push_back(latency);
    while (completed_.size() > kCapacity) {
        completed_.pop_front();
    }
}

std::optional<std::int64_t> LatencySampler::latest_ms() const {
    if (completed_.empty()) {
        return std::nullopt;
    }
    return completed_.back();
}

std::optional<std::int64_t> LatencySampler::p50_ms() const {
    if (completed_.empty()) {
        return std::nullopt;
    }
    std::vector<std::int64_t> sorted(completed_.begin(), completed_.end());
    const std::size_t middle = (sorted.size() - 1) / 2;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(middle),
                     sorted.end());
    return sorted[middle];
}

std::optional<std::int64_t> LatencySampler::max_ms() const {
    if (completed_.empty()) {
        return std::nullopt;
    }
    return *std::max_element(completed_.begin(), completed_.end());
}

void LatencySampler::clear() {
    pending_.clear();
    completed_.clear();
}

double ByteRateMeter::observe(std::uint64_t total_bytes, std::int64_t at_ms) {
    if (!previous_bytes_.has_value() || !previous_at_ms_.has_value()) {
        previous_bytes_ = total_bytes;
        previous_at_ms_ = at_ms;
        return 0.0;
    }
    const std::int64_t elapsed_ms = at_ms - *previous_at_ms_;
    const bool monotonic = elapsed_ms > 0 && total_bytes >= *previous_bytes_;
    if (!monotonic) {
        previous_bytes_ = total_bytes;
        previous_at_ms_ = at_ms;
        return 0.0;
    }
    const std::uint64_t delta = total_bytes - *previous_bytes_;
    previous_bytes_ = total_bytes;
    previous_at_ms_ = at_ms;
    return static_cast<double>(delta) * 1000.0 / static_cast<double>(elapsed_ms);
}

void ByteRateMeter::reset() {
    previous_bytes_.reset();
    previous_at_ms_.reset();
}

} // namespace tether::cli
