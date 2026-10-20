// tests/handshake_test.cpp - task 3.3 verification: the D4 HMAC
// challenge-response handshake over provisioned tokens. Includes an RFC 4231
// known-answer vector for the HMAC-SHA256 primitive, then the flow rules:
// correct tokens accepted, wrong/missing answers refused, and nonces are
// single-use (replayed or unknown nonces rejected). All tokens here are fake
// fixed strings; no real credentials in code.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "handshake.hpp"

namespace tp = tether::protocol;

namespace {

// Fake provisioned tokens for tests only (never real credentials).
const std::string kToken = "test-token-do-not-use-1";
const std::string kOtherToken = "test-token-do-not-use-2";

// Test-side hex decode: the wire nonce is hex, the HMAC covers the raw bytes.
std::string decode_raw_bytes(const std::string& hex) {
    std::string raw;
    raw.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        const int hi = nib(hex[i]);
        const int lo = nib(hex[i + 1]);
        REQUIRE(hi >= 0);
        REQUIRE(lo >= 0);
        raw.push_back(static_cast<char>((hi << 4) | lo));
    }
    return raw;
}

} // namespace

TEST_CASE("HMAC-SHA256 primitive matches RFC 4231 test case 2") {
    // Key "Jefe", data "what do ya want for nothing?"
    const std::string mac = tp::hmac_sha256_hex("Jefe", "what do ya want for nothing?");
    CHECK(mac == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // Determinism + key sensitivity.
    CHECK(mac == tp::hmac_sha256_hex("Jefe", "what do ya want for nothing?"));
    CHECK(mac != tp::hmac_sha256_hex("jefe", "what do ya want for nothing?"));
    CHECK(mac != tp::hmac_sha256_hex("Jefe", "what do ya want for nothing "));
}

TEST_CASE("nonces are 32 lowercase hex chars from real entropy") {
    for (int i = 0; i < 8; ++i) {
        const std::string nonce = tp::generate_nonce_hex();
        CHECK(nonce.size() == tp::kNonceHexChars);
        CHECK(tp::is_valid_nonce_hex(nonce));
    }
    // Two draws must not collide (16 bytes from random_device; collision
    // probability is negligible, and the console relies on this).
    CHECK(tp::generate_nonce_hex() != tp::generate_nonce_hex());
}

TEST_CASE("correct token passes: challenge answered and AUTH_OK issued") {
    tp::Challenger console(kToken);
    const tp::Challenge challenge = console.begin();
    CHECK(challenge.hmac_response.empty());
    CHECK(challenge.nonce.size() == tp::kNonceHexChars);

    tp::Responder agent(kToken);
    const tp::Challenge answer = agent.respond(challenge);
    CHECK(answer.nonce == challenge.nonce);
    CHECK(answer.hmac_response.size() == tp::kHmacHexChars);
    // The answer is exactly the HMAC of the RAW nonce bytes - reproducible.
    CHECK(answer.hmac_response == tp::hmac_sha256_hex(kToken, decode_raw_bytes(challenge.nonce)));

    tp::AuthOk ok;
    const auto failure = console.verify(answer, ok);
    CHECK_FALSE(failure.has_value()); // accepted
}

TEST_CASE("wrong token fails and refuses the session-compatible handshake") {
    tp::Challenger console(kToken);
    const tp::Challenge challenge = console.begin();

    // Agent answers with a token it was (wrongly) provisioned with.
    tp::Responder agent(kOtherToken);
    const tp::Challenge wrong = agent.respond(challenge);

    tp::AuthOk ok;
    auto failure = console.verify(wrong, ok);
    REQUIRE(failure.has_value());
    CHECK(*failure == tp::HandshakeFailure::WrongToken);

    // Refusal sticks: the薄膜 challenge was consumed by the failed attempt, so
    // the same answer replayed is rejected too - no second chance.
    failure = console.verify(wrong, ok);
    REQUIRE(failure.has_value());
    CHECK(*failure == tp::HandshakeFailure::NonceReplay);
}

TEST_CASE("missing or malformed responses are refused") {
    tp::Challenger console(kToken);
    const tp::Challenge challenge = console.begin();

    tp::AuthOk ok;
    // Empty hmac_response: the agent answered without computing the HMAC.
    const tp::Challenge empty_answer = tp::Challenge{challenge.nonce, ""};
    auto failure = console.verify(empty_answer, ok);
    REQUIRE(failure.has_value());
    CHECK(*failure == tp::HandshakeFailure::BadHmac);
    // A wrong answer consumed the nonce; retrying means replay.
    failure = console.verify(empty_answer, ok);
    REQUIRE(failure.has_value());
    CHECK(*failure == tp::HandshakeFailure::NonceReplay);

    // A non-hex hmac_response is refused the same way.
    tp::Challenger console_b(kToken);
    const tp::Challenge challenge_b = console_b.begin();
    const tp::Challenge garbage = tp::Challenge{challenge_b.nonce, "not-even-hex!"};
    const auto garbage_failure = console_b.verify(garbage, ok);
    REQUIRE(garbage_failure.has_value());
    CHECK(*garbage_failure == tp::HandshakeFailure::BadHmac);
}

TEST_CASE("replayed nonces are rejected") {
    tp::Challenger console(kToken);
    const tp::Challenge challenge = console.begin();

    tp::Responder agent(kToken);
    const tp::Challenge answer = agent.respond(challenge);

    tp::AuthOk ok;
    CHECK_FALSE(console.verify(answer, ok).has_value()); // first answer valid

    // The exact same answer sent again: replay, refuses even though valid.
    const auto replay = console.verify(answer, ok);
    REQUIRE(replay.has_value());
    CHECK(*replay == tp::HandshakeFailure::NonceReplay);

    // A third answer - even with a fresh-looking nonce - is a replay: the
    // single-use nonce from this handshake was consumed by the first answer.
    const tp::Challenge unknown{tp::generate_nonce_hex(), answer.hmac_response};
    const auto unknown_failure = console.verify(unknown, ok);
    REQUIRE(unknown_failure.has_value());
    CHECK(*unknown_failure == tp::HandshakeFailure::NonceReplay);
}

TEST_CASE("answers referencing unknown or non-issued nonces are rejected") {
    tp::Challenger console(kToken);
    const tp::Challenge challenge = console.begin();

    tp::Responder agent(kToken);
    const tp::Challenge answer = agent.respond(challenge);

    // Naive replay of a recorded answer against a DIFFERENT pending nonce.
    tp::Challenger console_b(kToken);
    console_b.begin();
    tp::AuthOk ok;
    const auto mismatch = console_b.verify(answer, ok);
    REQUIRE(mismatch.has_value());
    CHECK(*mismatch == tp::HandshakeFailure::UnknownNonce);
}

TEST_CASE("verify before begin is refused") {
    tp::Challenger console(kToken);
    tp::Responder agent(kToken);
    const tp::Challenge challenge = console.begin();
    const tp::Challenge answer = agent.respond(challenge);
    tp::Challenger fresh(kToken);
    tp::AuthOk ok;
    const auto failure = fresh.verify(answer, ok);
    REQUIRE(failure.has_value());
    CHECK(*failure == tp::HandshakeFailure::NotBegun);
}

TEST_CASE("begin twice is a programming error") {
    tp::Challenger console(kToken);
    static_cast<void>(console.begin());
    CHECK_THROWS_AS(console.begin(), tp::ProtocolError);
}

TEST_CASE("responder refuses malformed challenge nonces") {
    tp::Responder agent(kToken);
    CHECK_THROWS_AS(agent.respond(tp::Challenge{ "", ""}), tp::ProtocolError);
    CHECK_THROWS_AS(agent.respond(tp::Challenge{"zzzz", ""}), tp::ProtocolError);
    CHECK_THROWS_AS(agent.respond(tp::Challenge{"a1b2c3d4e5f60718293a4b5c6d7e8f9", ""}),
                    tp::ProtocolError); // 31 chars
}
