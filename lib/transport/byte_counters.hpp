// lib/transport/byte_counters.hpp - console-side link instrumentation
// (add-health-telemetry task 6.4).
//
// Counting decorators around the accepted session's byte seam. They are used
// on the CONSOLE side only (TcpConsoleTransport wraps each accepted
// TcpByteStream); the agent's dial path never constructs them, so no wire,
// protocol, or agent behavior changes. The counters feed the monitor's
// throughput reading (bytes moved over the recent cadence interval).
//
// Everything here is single-threaded by construction: the transports are
// poll()-driven on the owning thread (design D7: the console measures its own
// link locally).
#pragma once

#include "bytes.hpp"

#include <cstddef>
#include <cstdint>

namespace tether::transport {

// Shared by every accepted console connection; `total_bytes()` is what the
// throughput meter samples once per monitor interval.
struct ByteTotals {
    std::uint64_t read_bytes = 0;
    std::uint64_t written_bytes = 0;

    std::uint64_t total_bytes() const noexcept { return read_bytes + written_bytes; }
};

// Counts inbound bytes (agent -> console).
class ByteCountingSource final : public protocol::IByteSource {
public:
    ByteCountingSource(protocol::IByteSource& inner, ByteTotals& totals)
        : inner_(inner), totals_(totals) {}

    protocol::ReadResult read(unsigned char* dst, std::size_t max) override {
        const protocol::ReadResult result = inner_.read(dst, max);
        totals_.read_bytes += result.count;
        return result;
    }

private:
    protocol::IByteSource& inner_;
    ByteTotals& totals_;
};

// Counts outbound bytes (console -> agent).
class ByteCountingSink final : public protocol::IByteSink {
public:
    ByteCountingSink(protocol::IByteSink& inner, ByteTotals& totals)
        : inner_(inner), totals_(totals) {}

    void write(const unsigned char* bytes, std::size_t count) override {
        inner_.write(bytes, count);
        totals_.written_bytes += count;
    }

private:
    protocol::IByteSink& inner_;
    ByteTotals& totals_;
};

} // namespace tether::transport
