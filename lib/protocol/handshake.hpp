// lib/protocol/handshake.hpp - HMAC challenge-response authentication over a
// provisioned pre-shared token (design D4, task 3.3).
//
// Flow (per the secure-session delta and D4):
//   1. agent -> console   HELLO   {protocol_version, agent_name, agent_id}
//   2. console -> agent   CHALLENGE {nonce}                     (initiating form)
//   3. agent -> console   CHALLENGE {nonce, hmac_response}       (answer form)
//   4. console -> agent   AUTH_OK
//
// Flagged deviation (recorded for the phase report): D3's one-byte CHALLENGE
// type is overloaded bidirectionally - the agent's answer re-uses CHALLENGE
// with an extra `hmac_response` field instead of introducing a new type value.
// This keeps D3's type enum unchanged; both wire forms decode from the single
// MsgType::Challenge byte.
//
// Wire conventions:
// - The nonce is 16 random bytes from std::random_device, hex-encoded (32
//   lowercase hex chars) for transport in JSON.
// - hmac_response = HMAC-SHA256(key = provisioned token, message = the raw
//   16 decoded nonce bytes), hex-encoded (64 chars).
// - Nonces are single-use: the console remembers issued/checked nonces and
//   rejects replayed or unknown nonces as authentication failures.
// - No shared secret ever crosses the wire; comparison is constant-time.
#pragma once

#include "payloads.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace tether::protocol {

// HMAC-SHA256 output length key by mbedtls.
inline constexpr std::size_t kHmacBytes = 32;

// The primitive used by both sides: HMAC-SHA256 over `raw` bytes keyed with
// the provisioned token. Implemented through mbedtls in handshake.cpp.
std::array<unsigned char, kHmacBytes> hmac_sha256(const std::string& token,
                                                  const unsigned char* raw, std::size_t len);

// Hex convenience form (used on both peers, tested against RFC 4231 vectors).
std::string hmac_sha256_hex(const std::string& token, const std::string& raw);

// A fresh 128-bit nonce, hex-encoded, from std::random_device.
// Really-random per session; the test suite only consumes its shape.
std::string generate_nonce_hex();

// True for 32-char lowercase hex strings.
bool is_valid_nonce_hex(const std::string& nonce_hex);

// Reasons a challenge response can be refused (each becomes an ERR/refusal on
// the session layer; the console never issues AUTH_OK after any of them).
enum class HandshakeFailure {
    NotBegun,      // verify() called before begin() issued the challenge
    UnknownNonce,  // answer's nonce differs from the issued one
    NonceReplay,   // the issued nonce was already consumed by a first answer
    BadHmac,       // hmac_response is not a 64-char hex string
    WrongToken,    // HMAC does not verify against the provisioned token
};

// Console-side handshake state machine (pure: operates on Messages only, the
// session layer shuttles them over the envelope).
class Challenger {
public:
    // `token` is the token bound to the connecting agent's identity; the
    // session layer resolves it before constructing the challenger.
    explicit Challenger(std::string token);

    // Step 2: issue the challenge. Called exactly once per handshake.
    Challenge begin();

    // Verify the agent's answer. Any call consumes the pending nonce
    // (single-use): on the FIRST call a wrong/absent answer fails the
    // handshake and a later retry of the same answer reports NonceReplay.
    // Returns the AUTH_OK payload to send on success.
    std::optional<HandshakeFailure> verify(const Challenge& answer, AuthOk& out_ok);

private:
    std::string token_;
    std::string pending_nonce_;
    bool begun_ = false;
    bool consumed_ = false;
};

// Agent-side handshake state machine (pure likewise).
class Responder {
public:
    explicit Responder(std::string token);

    // Step 3: answer a challenge by echoing the nonce plus the HMAC of the
    // raw nonce bytes under the provisioned token. Throws ProtocolError on a
    // malformed challenge (non-hex nonce).
    Challenge respond(const Challenge& challenge);

private:
    std::string token_;
};

} // namespace tether::protocol
