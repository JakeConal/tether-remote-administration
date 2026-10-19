// Smoke test (Phase 1): proves the test framework + CTest integration end-to-end.
// Exercising one trivial unit: the protocol envelope version constant (design D3).
// This TU is the framework entry point (header-only doctest mode).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "protocol.hpp"

TEST_CASE("protocol envelope version constant is 2") {
    CHECK(tether::protocol::kEnvelopeVersion == 2);
}
