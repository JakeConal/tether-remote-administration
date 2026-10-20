// lib/protocol/envelope.cpp - protocol v2 envelope codec (design D3, task 3.1).
// See envelope.hpp for the wire shape and the rejection/poisoning rules.
#include "envelope.hpp"

#include <algorithm>

namespace tether::protocol {

namespace {

// Fixed little-endian field helpers: the wire order is pinned by design D3,
// not derived from host integer layout.
void put_u32_le(unsigned char* dst, std::uint32_t value) {
    dst[0] = static_cast<unsigned char>(value & 0xFFu);
    dst[1] = static_cast<unsigned char>((value >> 8) & 0xFFu);
    dst[2] = static_cast<unsigned char>((value >> 16) & 0xFFu);
    dst[3] = static_cast<unsigned char>((value >> 24) & 0xFFu);
}

std::uint32_t get_u32_le(const unsigned char* src) {
    return static_cast<std::uint32_t>(src[0]) | (static_cast<std::uint32_t>(src[1]) << 8) |
           (static_cast<std::uint32_t>(src[2]) << 16) | (static_cast<std::uint32_t>(src[3]) << 24);
}

} // namespace

const char* msg_type_name(MsgType type) {
    switch (type) {
        case MsgType::Hello: return "HELLO";
        case MsgType::Challenge: return "CHALLENGE";
        case MsgType::AuthOk: return "AUTH_OK";
        case MsgType::Cmd: return "CMD";
        case MsgType::ResultText: return "RESULT_TEXT";
        case MsgType::FileMeta: return "FILE_META";
        case MsgType::FileChunk: return "FILE_CHUNK";
        case MsgType::FileEnd: return "FILE_END";
        case MsgType::Cancel: return "CANCEL";
        case MsgType::Ping: return "PING";
        case MsgType::Pong: return "PONG";
        case MsgType::Err: return "ERR";
    }
    return "invalid";
}

bool is_valid_msg_type(unsigned char raw) {
    return raw >= static_cast<unsigned char>(MsgType::Hello) &&
           raw <= static_cast<unsigned char>(MsgType::Err);
}

std::size_t max_payload_bytes_for(MsgType type) {
    return type == MsgType::FileChunk ? kMaxFileChunkBytes : kMaxControlPayloadBytes;
}

std::vector<unsigned char> encode_frame(const Frame& frame) {
    if (!is_valid_msg_type(static_cast<unsigned char>(frame.type))) {
        throw ProtocolError(ProtocolError::Code::UnknownType,
                            std::string("frame type not in the v2 MsgType enum: ") +
                                msg_type_name(frame.type));
    }
    const std::size_t bound = max_payload_bytes_for(frame.type);
    if (frame.payload.size() > bound) {
        throw ProtocolError(
            ProtocolError::Code::OversizePayload,
            "frame payload of " + std::to_string(frame.payload.size()) + " bytes exceeds the " +
                msg_type_name(frame.type) + " bound of " + std::to_string(bound) + " bytes");
    }
    // DoS guard: payloads are JSON or file slices, never anywhere near the
    // uint32 length field's own range.
    if (frame.payload.size() > 0xFFFFFFFFu) {
        throw ProtocolError(ProtocolError::Code::OversizePayload,
                            "frame payload exceeds the 32-bit len field");
    }

    std::vector<unsigned char> bytes;
    bytes.reserve(kEnvelopeHeaderBytes + frame.payload.size());
    bytes.push_back(kEnvelopeVersion);
    bytes.push_back(static_cast<unsigned char>(frame.type));
    unsigned char seq_bytes[4];
    put_u32_le(seq_bytes, frame.seq);
    bytes.insert(bytes.end(), seq_bytes, seq_bytes + 4);
    unsigned char len_bytes[4];
    put_u32_le(len_bytes, static_cast<std::uint32_t>(frame.payload.size()));
    bytes.insert(bytes.end(), len_bytes, len_bytes + 4);
    bytes.insert(bytes.end(), frame.payload.begin(), frame.payload.end());
    return bytes;
}

void FrameDecoder::feed(const unsigned char* bytes, std::size_t count,
                        std::vector<Frame>& frames) {
    if (poison_) {
        poison(*poison_);
    }
    if (bytes == nullptr && count != 0) {
        throw ProtocolError(ProtocolError::Code::BadArgument, "decoder fed a null buffer");
    }

    std::size_t consumed = 0;
    while (consumed < count) {
        if (phase_ == Phase::Header) {
            const std::size_t to_copy =
                std::min(count - consumed, kEnvelopeHeaderBytes - header_fill_);
            std::copy(bytes + consumed, bytes + consumed + to_copy, header_ + header_fill_);
            header_fill_ += to_copy;
            consumed += to_copy;
            if (header_fill_ < kEnvelopeHeaderBytes) {
                break;
            }
            // Header complete: validate identity and bounds before any payload
            // byte is buffered, so violations never get second-guessed later.
            if (header_[0] != kEnvelopeVersion) {
                poison(ProtocolError(ProtocolError::Code::UnknownVersion,
                                     "unknown envelope version " + std::to_string(header_[0]) +
                                         " (expected " + std::to_string(kEnvelopeVersion) + ")"));
            }
            if (!is_valid_msg_type(header_[1])) {
                poison(ProtocolError(ProtocolError::Code::UnknownType,
                                     "unknown message type " + std::to_string(header_[1]) +
                                         " in the envelope header"));
            }
            pending_type_ = static_cast<MsgType>(header_[1]);
            pending_seq_ = get_u32_le(header_ + 2);
            const std::uint32_t declared_len = get_u32_le(header_ + 6);
            const std::size_t bound = max_payload_bytes_for(pending_type_);
            if (declared_len > bound) {
                poison(ProtocolError(
                    ProtocolError::Code::OversizePayload,
                    std::string(msg_type_name(pending_type_)) + " frame declares " +
                        std::to_string(static_cast<unsigned long long>(declared_len)) +
                        " payload bytes, exceeding the bound of " + std::to_string(bound)));
            }
            expected_payload_ = declared_len;
            payload_.clear();
            payload_.reserve(declared_len);
            if (declared_len == 0) {
                // Zero-payload frame already complete: emit immediately so a
                // clean end-of-stream right after it is not misread as a
                // truncated frame. The header is consumed with the frame, so
                // the parse state must reset here just like the payload path.
                frames.push_back(Frame{pending_type_, pending_seq_, {}});
                phase_ = Phase::Header;
                header_fill_ = 0;
                continue;
            }
            phase_ = Phase::Payload;
            continue;
        }

        const std::size_t to_copy =
            std::min(count - consumed, expected_payload_ - payload_.size());
        payload_.insert(payload_.end(), bytes + consumed, bytes + consumed + to_copy);
        consumed += to_copy;
        if (payload_.size() < expected_payload_) {
            break;
        }
        frames.push_back(Frame{pending_type_, pending_seq_, std::move(payload_)});
        payload_.clear();
        phase_ = Phase::Header;
        header_fill_ = 0;
    }
}

std::size_t FrameDecoder::buffered() const noexcept {
    return phase_ == Phase::Header ? header_fill_ : kEnvelopeHeaderBytes + payload_.size();
}

void FrameDecoder::finish() {
    if (poison_) {
        poison(*poison_);
    }
    if (buffered() != 0) {
        poison(ProtocolError(ProtocolError::Code::Truncated,
                             "stream ended with " + std::to_string(buffered()) +
                                 " bytes of an incomplete frame pending"));
    }
}

void FrameDecoder::poison(const ProtocolError& error) {
    poison_ = error; // remember: every later feed/finish re-throws this exact error
    throw error;
}

} // namespace tether::protocol
