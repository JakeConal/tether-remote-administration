// lib/protocol/envelope.hpp - protocol v2 envelope codec (design D3, task 3.1).
//
// Wire shape (fixed little-endian order, both multi-byte fields):
//   [0]      1B ver  = kEnvelopeVersion (2)
//   [1]      1B type = MsgType discriminator (range-validated)
//   [2..5]   4B seq  = sequence number, little-endian uint32
//   [6..9]   4B len  = payload byte count, little-endian uint32
//   [10..]   payload = JSON (control frames) or raw bytes (FILE_CHUNK)
//
// Strict bounds (kMaxControlPayloadBytes / kMaxFileChunkBytes): a frame that
// declares more payload bytes than its type allows is rejected the moment its
// header is complete - without consuming or re-interpreting any of the
// remaining stream as a different message. Unknown `ver` and out-of-range
// `type` poison the stream the same way: framing alignment is unrecoverable
// past such a violation, so every later feed re-reports the same error.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace tether::protocol {

// Envelope version carried in every message's 1-byte ver field (design D3).
inline constexpr unsigned char kEnvelopeVersion = 2;

// [ver][type][seq][len] - 1 + 1 + 4 + 4 bytes ahead of every payload.
inline constexpr std::size_t kEnvelopeHeaderBytes = 10;

// Strict payload caps per design D3 and the phase direction: control frames
// are JSON debuggable at human scale, file chunks stream in bounded slices.
inline constexpr std::size_t kMaxControlPayloadBytes = 1024 * 1024;  // 1 MiB, JSON control frames
inline constexpr std::size_t kMaxFileChunkBytes = 256 * 1024;        // 256 KiB, FILE_CHUNK raw payload

// Message type discriminator carried in the envelope's 1-byte type field
// (design D3 type list). Values are fixed wire constants for protocol v2.
enum class MsgType : unsigned char {
    Hello = 1,
    Challenge = 2,
    AuthOk = 3,
    Cmd = 4,
    ResultText = 5,
    FileMeta = 6,
    FileChunk = 7,
    FileEnd = 8,
    Cancel = 9,
    Ping = 10,
    Pong = 11,
    Err = 12,
};

// Human-readable name for log/error reporting; "invalid" for raw values that
// are no MsgType (never throws).
const char* msg_type_name(MsgType type);

// True exactly for the 12 defined MsgType values (1..12): the decoder uses
// this to range-validate the type byte before trusting anything else.
bool is_valid_msg_type(unsigned char raw);

// Payload cap for a frame of this type: FILE_CHUNK streams raw bytes under the
// chunk bound; every other type carries JSON under the control bound.
std::size_t max_payload_bytes_for(MsgType type);

// Error taxonomy for everything between the wire and the session layer.
class ProtocolError : public std::runtime_error {
public:
    enum class Code {
        UnknownVersion,   // envelope `ver` byte is not kEnvelopeVersion
        UnknownType,      // envelope `type` byte outside the MsgType enum
        OversizePayload,  // declared/composed payload exceeds the type bound
        Truncated,        // stream ended with an incomplete frame pending
        MalformedPayload, // payload failed its per-type shape validation
        BadArgument,      // API misuse (bad input values handed to the codec)
        CryptoFailure,    // HMAC-SHA256 primitive returned an error
    };

    ProtocolError(Code code, const std::string& message)
        : std::runtime_error(message), code_(code) {}

    Code code() const noexcept { return code_; }

private:
    Code code_;
};

// One decoded protocol message: discriminator + seq + payload bytes.
struct Frame {
    MsgType type = MsgType::Hello;
    std::uint32_t seq = 0;
    std::vector<unsigned char> payload;
};

// Serialize a frame to the wire. Throws ProtocolError for an out-of-range
// type or a payload exceeding the type's declared bound; the seq field is
// range-unconstrained (every uint32 value is a legal sequence number).
std::vector<unsigned char> encode_frame(const Frame& frame);

// Incremental decoder: consuming arbitrary stream slices, emitting complete
// frames and rejecting protocol violations without re-interpreting the stream.
//
// Decoding rules:
// - Bound/identity violations (unknown ver, unknown type, oversize len) are
//   detectable from the header alone and poison the decoder: all later feeds
//   re-throw the same error, and no further frame is ever emitted.
// - finish() marks the stream ended; any frame left incomplete at that point
//   reports Truncated and poisons the decoder as well.
class FrameDecoder {
public:
    // Feed raw stream bytes; complete frames are appended to `frames`. Throws
    // ProtocolError on a wire violation (idempotently once poisoned).
    void feed(const unsigned char* bytes, std::size_t count, std::vector<Frame>& frames);

    // Bytes held waiting to complete the current frame (header or payload).
    std::size_t buffered() const noexcept;

    // Signal end-of-stream. Throws ProtocolError(Truncated) if a frame is
    // still pending; harmless idempotent no-op when the buffer is empty.
    void finish();

private:
    enum class Phase { Header, Payload };

    [[noreturn]] void poison(const ProtocolError& error);

    Phase phase_ = Phase::Header;
    unsigned char header_[kEnvelopeHeaderBytes] = {};
    std::size_t header_fill_ = 0;
    MsgType pending_type_ = MsgType::Hello;
    std::uint32_t pending_seq_ = 0;
    std::size_t expected_payload_ = 0;
    std::vector<unsigned char> payload_;
    std::optional<ProtocolError> poison_;
};

} // namespace tether::protocol
