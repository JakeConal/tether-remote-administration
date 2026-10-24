// tests/cli_stats_test.cpp - add-health-telemetry task 6.4: console-local link
// instrumentation with canned bytes and timestamps (no sockets, no sleeping).
// The counters wrap the same IByteSource/IByteSink seam the real accepted
// session uses; the sampler and rate math are what TcpOperConsole feeds into
// monitor observations.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "byte_counters.hpp"
#include "link_stats.hpp"

#include <cstdint>
#include <vector>

namespace tc = tether::cli;
namespace tp = tether::protocol;
namespace tt = tether::transport;

TEST_CASE("counting byte decorators accumulate both wire directions") {
    tp::InMemoryPipe pipe;
    tt::ByteTotals totals;
    CHECK(totals.total_bytes() == 0);

    tt::ByteCountingSink sink{pipe, totals};
    const std::vector<unsigned char> outbound{'a', 'b', 'c', 'd', 'e'};
    sink.write(outbound.data(), outbound.size());
    CHECK(totals.written_bytes == 5);
    CHECK(totals.read_bytes == 0);
    CHECK(totals.total_bytes() == 5);

    tt::ByteCountingSource source{pipe, totals};
    unsigned char buffer[8]{};
    const tp::ReadResult first = source.read(buffer, 3);
    CHECK(first.count == 3);
    CHECK(first.end_of_stream == false);
    CHECK(totals.read_bytes == 3);

    const tp::ReadResult second = source.read(buffer, 8);
    CHECK(second.count == 2);
    CHECK(totals.read_bytes == 5);

    // An empty read is a real observation but moves no bytes.
    const tp::ReadResult empty = source.read(buffer, 8);
    CHECK(empty.count == 0);
    CHECK(empty.end_of_stream == false);
    CHECK(totals.read_bytes == 5);

    // EOF after a close moves no bytes either.
    pipe.close_writer();
    const tp::ReadResult eof = source.read(buffer, 8);
    CHECK(eof.count == 0);
    CHECK(eof.end_of_stream == true);
    CHECK(totals.read_bytes == 5);
    CHECK(totals.total_bytes() == 10);
}

TEST_CASE("latency sampler records issue-to-terminal per seq") {
    tc::LatencySampler sampler;
    CHECK_FALSE(sampler.latest_ms().has_value());
    CHECK_FALSE(sampler.p50_ms().has_value());
    CHECK_FALSE(sampler.max_ms().has_value());
    CHECK(sampler.samples() == 0);

    sampler.issued(1, 1000);
    CHECK(sampler.samples() == 0); // pending is not a sample yet
    sampler.completed(1, 1012);
    CHECK(sampler.latest_ms() == std::int64_t{12});
    CHECK(sampler.p50_ms() == std::int64_t{12});
    CHECK(sampler.max_ms() == std::int64_t{12});
    CHECK(sampler.samples() == 1);

    sampler.issued(2, 2000);
    sampler.completed(2, 2004);
    CHECK(sampler.latest_ms() == std::int64_t{4});
    // Lower median for an even count: [4, 12] -> 4.
    CHECK(sampler.p50_ms() == std::int64_t{4});
    CHECK(sampler.max_ms() == std::int64_t{12});

    sampler.issued(3, 3000);
    sampler.completed(3, 3021);
    CHECK(sampler.latest_ms() == std::int64_t{21});
    CHECK(sampler.p50_ms() == std::int64_t{12});
    CHECK(sampler.max_ms() == std::int64_t{21});
    CHECK(sampler.samples() == 3);

    // An unknown completion is ignored (never fabricates a sample).
    sampler.completed(99, 5000);
    CHECK(sampler.samples() == 3);

    // A stale pending entry for a reused seq is overwritten, not double-counted.
    sampler.issued(4, 4000);
    sampler.issued(4, 4100);
    sampler.completed(4, 4105);
    CHECK(sampler.samples() == 4);
    CHECK(sampler.latest_ms() == std::int64_t{5});

    sampler.clear();
    CHECK(sampler.samples() == 0);
    CHECK_FALSE(sampler.latest_ms().has_value());
}

TEST_CASE("throughput math reports a rate only over a real interval") {
    tc::ByteRateMeter meter;
    // The first observation anchors; no prior interval exists yet.
    CHECK(meter.observe(0, 1000) == 0.0);
    CHECK(meter.observe(1000, 2000) == doctest::Approx(1000.0));
    CHECK(meter.observe(3000, 3000) == doctest::Approx(2000.0));

    // A non-monotonic clock re-anchors rather than reporting a bogus rate.
    CHECK(meter.observe(4000, 3000) == 0.0);
    CHECK(meter.observe(5000, 3500) == doctest::Approx(2000.0));

    // A shrunken counter (fresh totals object) re-anchors the same way.
    CHECK(meter.observe(100, 4000) == 0.0);
    CHECK(meter.observe(1100, 4500) == doctest::Approx(2000.0));

    // Zero elapsed time is not an infinite rate.
    CHECK(meter.observe(2100, 4500) == 0.0);

    meter.reset();
    CHECK(meter.observe(9999, 5000) == 0.0);
    CHECK(meter.observe(10999, 5500) == doctest::Approx(2000.0));
}
