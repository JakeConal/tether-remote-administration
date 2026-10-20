// lib/protocol/payloads.hpp - JSON payload mapping for protocol v2 (D3, task
// 3.2). A Message variant keyed by the envelope's MsgType, with strict
// per-type field validation: malformed shapes raise ProtocolError with the
// offending field named in the message.
//
// FILE_CHUNK is the one non-JSON branch: it carries raw file bytes directly
// (design D3), modeled as its own variant arm (`FileChunk`).
//
// Direction conventions live with the session/handshake layer; the payload
// layer only knows field shapes. Note the CHALLENGE type is overloaded on
// direction (design D4, flagged deviation): console -> agent carries only
// `nonce`, while the agent's answer re-uses CHALLENGE as `nonce +
// hmac_response`. Both are valid decodings of the one type byte.
#pragma once

#include "envelope.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace tether::protocol {

// Nonce and HMAC answers travel as lowercase hex strings inside JSON.
inline constexpr std::size_t kNonceHexChars = 32;      // 16 random bytes
inline constexpr std::size_t kHmacHexChars = 64;       // HMAC-SHA256 = 32 bytes

struct Hello {
    unsigned protocol_version = kEnvelopeVersion;
    std::string agent_name; // unique registry identity (see AgentConfig)
    std::string agent_id;   // install-specific instance identifier
};

struct Challenge {
    std::string nonce;          // console -> agent: 32-hex-char random nonce
    std::string hmac_response;  // agent -> console only: HMAC over the raw nonce bytes
};

struct AuthOk {}; // acceptance stamp; the console sends it empty

struct Cmd {
    std::string command;             // command verb, non-empty
    std::vector<std::string> args;   // argv-style array (may be empty)
};

struct ResultText {
    std::string text;
};

struct FileMeta {
    std::string name;      // file name for streaming results
    std::uint64_t size = 0; // total byte count of the following FILE_CHUNKs
};

struct FileChunk {
    std::vector<unsigned char> bytes; // raw, not JSON
};

struct FileEnd {}; // end of the file stream for this command's seq

struct Cancel {}; // target seq rides in the envelope

struct Ping {};
struct Pong {};

struct Err {
    std::string code;    // stable short code, e.g. "auth_failed", "cancelled"
    std::string message; // human-readable detail
};

using Message = std::variant<Hello, Challenge, AuthOk, Cmd, ResultText, FileMeta, FileChunk,
                             FileEnd, Cancel, Ping, Pong, Err>;

// Ther MsgType for a message (matches the variant arm; ranges over the enum).
MsgType message_type_of(const Message& message);

// Serialize the message's payload per its type: JSON for control frames, raw
// bytes for FileChunk. Bounds are enforced again by encode_frame.
std::vector<unsigned char> encode_payload(const Message& message);

// Full-frame helper: payload + type + seq in one call.
std::vector<unsigned char> encode_message(const Message& message, std::uint32_t seq);

// Decode a payload into a Message per the frame's type. Throws
// ProtocolError(Code::MalformedPayload) with a message naming the offending
// field for: non-JSON syntax, non-object control payloads, missing fields,
// fields of the wrong type, empty required strings, out-of-range numbers, and
// unknown extra fields (strict shapes protect against silent version drift).
Message decode_payload(MsgType type, const std::vector<unsigned char>& payload);

} // namespace tether::protocol
