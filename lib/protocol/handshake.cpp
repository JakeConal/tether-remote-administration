// lib/protocol/handshake.cpp - HMAC-SHA256 challenge-response (D4, task 3.3).
// Crypto: mbedtls (added to the vcpkg manifest in this phase - the only
// manifest change allowed here). See handshake.hpp for flow and wire rules.
#include "handshake.hpp"

#include <mbedtls/md.h>

#include <random>

namespace tether::protocol {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

bool decode_hex(const std::string& hex, unsigned char* out, std::size_t byte_count) {
    if (hex.size() != byte_count * 2) return false;
    for (std::size_t i = 0; i < byte_count; ++i) {
        const int hi = hex_value(hex[i * 2]);
        const int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return true;
}

std::string encode_hex(const unsigned char* bytes, std::size_t count) {
    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        hex.push_back(digits[bytes[i] >> 4]);
        hex.push_back(digits[bytes[i] & 0x0F]);
    }
    return hex;
}

// Constant-time byte compare: no early exit on the first differing byte, so
// verification timing does not leak where the HMAC diverged.
bool constant_time_equal(const unsigned char* a, const unsigned char* b, std::size_t n) {
    unsigned char diff = 0;
    for (std::size_t i = 0; i < n; ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

} // namespace

std::array<unsigned char, kHmacBytes> hmac_sha256(const std::string& token,
                                                  const unsigned char* raw, std::size_t len) {
    const auto* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == nullptr) {
        throw ProtocolError(ProtocolError::Code::CryptoFailure,
                            "mbedtls cannot provide the SHA-256 md info");
    }
    std::array<unsigned char, kHmacBytes> out{};
    const int rc = mbedtls_md_hmac(md_info, reinterpret_cast<const unsigned char*>(token.data()),
                                   token.size(), raw, len, out.data());
    if (rc != 0) {
        throw ProtocolError(ProtocolError::Code::CryptoFailure,
                            "mbedtls_md_hmac failed with " + std::to_string(rc));
    }
    return out;
}

std::string hmac_sha256_hex(const std::string& token, const std::string& raw) {
    const auto mac = hmac_sha256(token, reinterpret_cast<const unsigned char*>(raw.data()),
                                 raw.size());
    return encode_hex(mac.data(), mac.size());
}

std::string generate_nonce_hex() {
    // Entropy source fixed by the phase direction: std::random_device.
    std::random_device device;
    unsigned char raw[16];
    for (unsigned char& byte : raw) {
        byte = static_cast<unsigned char>(device() & 0xFFu);
    }
    return encode_hex(raw, sizeof(raw));
}

bool is_valid_nonce_hex(const std::string& nonce_hex) {
    unsigned char raw[16];
    return decode_hex(nonce_hex, raw, sizeof(raw));
}

Challenger::Challenger(std::string token) : token_(std::move(token)) {}

Challenge Challenger::begin() {
    if (begun_) {
        throw ProtocolError(ProtocolError::Code::BadArgument,
                            "handshake challenge already issued");
    }
    begun_ = true;
    pending_nonce_ = generate_nonce_hex();
    return Challenge{pending_nonce_, ""};
}

std::optional<HandshakeFailure> Challenger::verify(const Challenge& answer, AuthOk& out_ok) {
    if (!begun_) {
        return HandshakeFailure::NotBegun;
    }
    // The nonce is consumed by the first attempt whatever its outcome: a failed
    // answer is refused by the session (no second chance), so a later answer
    // re-presenting the nonce can only be a replay - rejected per the spec's
    // replay rule before anything else is evaluated.
    if (consumed_) {
        return HandshakeFailure::NonceReplay;
    }
    consumed_ = true;
    if (answer.nonce != pending_nonce_ ||
        !is_valid_nonce_hex(answer.nonce)) {
        return HandshakeFailure::UnknownNonce;
    }
    if (answer.hmac_response.size() != kHmacHexChars) {
        return HandshakeFailure::BadHmac;
    }

    unsigned char nonce_raw[16];
    unsigned char answer_raw[kHmacBytes];
    if (!decode_hex(answer.nonce, nonce_raw, sizeof(nonce_raw)) ||
        !decode_hex(answer.hmac_response, answer_raw, kHmacBytes)) {
        return HandshakeFailure::BadHmac;
    }

    const auto expected = hmac_sha256(token_, nonce_raw, sizeof(nonce_raw));
    if (!constant_time_equal(expected.data(), answer_raw, kHmacBytes)) {
        return HandshakeFailure::WrongToken;
    }
    out_ok = AuthOk{};
    return std::nullopt;
}

Responder::Responder(std::string token) : token_(std::move(token)) {}

Challenge Responder::respond(const Challenge& challenge) {
    unsigned char nonce_raw[16];
    if (!decode_hex(challenge.nonce, nonce_raw, sizeof(nonce_raw))) {
        throw ProtocolError(ProtocolError::Code::BadArgument,
                            "challenge nonce is not a valid " + std::to_string(kNonceHexChars) +
                                "-character hex string");
    }
    // D4: HMAC over the RAW nonce bytes (the 16 decoded bytes), not their hex
    // representation; keyed with the provisioned token.
    const auto mac = hmac_sha256(token_, nonce_raw, sizeof(nonce_raw));
    return Challenge{challenge.nonce, encode_hex(mac.data(), mac.size())};
}

} // namespace tether::protocol
