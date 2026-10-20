// tests/envelope_test.cpp - task 3.1 verification: the D3 envelope codec.
// Covers: wire byte layout (ver|type|seq|len, little-endian), round-trip,
// truncated frames, oversize len, unknown version, unknown type, strict
// payload bounds, and the poison-without-misreading-the-stream semantics.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "envelope.hpp"

namespace tp = tether::protocol;
using tp::Frame;
using tp::MsgType;
using tp::ProtocolError;

namespace {

// Clear-text payload so violations are easy to spot in hex dumps.
std::vector<unsigned char> payload_from(const std::string& text) {
    return std::vector<unsigned char>(text.begin(), text.end());
}

std::vector<unsigned char> encode_of(MsgType type, std::uint32_t seq,
                                     const std::vector<unsigned char>& payload) {
    return tp::encode_frame(Frame{type, seq, payload});
}

std::vector<Frame> decode_all(const std::vector<unsigned char>& wire) {
    tp::FrameDecoder decoder;
    std::vector<Frame> frames;
    decoder.feed(wire.data(), wire.size(), frames);
    decoder.finish();
    return frames;
}

} // namespace

TEST_CASE("wire byte layout: ver|type|seq|len little-endian") {
    const auto payload = payload_from("hello world");
    const auto wire = encode_of(MsgType::Cmd, 0x10203040u, payload);
    REQUIRE(wire.size() == tp::kEnvelopeHeaderBytes + payload.size());
    CHECK(wire[0] == tp::kEnvelopeVersion);
    CHECK(wire[1] == static_cast<unsigned char>(MsgType::Cmd));
    CHECK(wire[2] == 0x40u); // seq 0x10203040, least significant byte first
    CHECK(wire[3] == 0x30u);
    CHECK(wire[4] == 0x20u);
    CHECK(wire[5] == 0x10u);
    CHECK(wire[6] == static_cast<unsigned char>(payload.size())); // len, little-endian
    CHECK(wire[7] == 0x00u);
    CHECK(wire[8] == 0x00u);
    CHECK(wire[9] == 0x00u);
    CHECK(std::vector<unsigned char>(wire.begin() + tp::kEnvelopeHeaderBytes, wire.end()) ==
          payload);
}

TEST_CASE("round-trip preserves type, seq, and payload exactly") {
    const std::string text = "ALL the payloads: {\"json\":true}  \x01\x7f";
    const Frame sent{MsgType::ResultText, 424242u, payload_from(text)};
    const auto wire = tp::encode_frame(sent);
    const auto frames = decode_all(wire);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].type == MsgType::ResultText);
    CHECK(frames[0].seq == 424242u);
    CHECK(frames[0].payload == sent.payload);
}

TEST_CASE("decoder assembles frames across arbitrary feed slices") {
    const auto a = encode_of(MsgType::Ping, 0, {});
    const auto b = encode_of(MsgType::Pong, 0, payload_from("pong"));
    std::vector<unsigned char> wire;
    wire.insert(wire.end(), a.begin(), a.end());
    wire.insert(wire.end(), b.begin(), b.end());

    tp::FrameDecoder decoder;
    std::vector<Frame> frames;
    // Byte-by-byte feeding must behave exactly like a single-shot feed.
    for (const unsigned char byte : wire) {
        decoder.feed(&byte, 1, frames);
    }
    decoder.finish();
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].type == MsgType::Ping);
    CHECK(frames[0].seq == 0);
    CHECK(frames[1].type == MsgType::Pong);
    CHECK(frames[1].payload == payload_from("pong"));
    CHECK(decoder.buffered() == 0);
}

TEST_CASE("truncated frame: rejected at end-of-stream, never misread") {
    const std::string text = "payload cut off before its last 3 bytes";
    auto wire = encode_of(MsgType::Cmd, 7u, payload_from(text));
    wire.resize(wire.size() - 3);

    tp::FrameDecoder decoder;
    std::vector<Frame> frames;
    decoder.feed(wire.data(), wire.size(), frames);
    CHECK(frames.empty());
    CHECK(decoder.buffered() == wire.size());

    bool threw = false;
    try {
        decoder.finish();
    } catch (const ProtocolError& e) {
        threw = true;
        CHECK(e.code() == ProtocolError::Code::Truncated);
    }
    CHECK(threw);
    // The rejected partial frame is never re-interpreted as another message
    // even if later bytes arrive: the decoder is poisoned past the error.
    CHECK(decoder.buffered() != 0);
}

TEST_CASE("hot-pipe truncation: bytes already consumed by an earlier frame stay valid") {
    const auto good = encode_of(MsgType::Ping, 0, {});
    auto bad = encode_of(MsgType::Cmd, 1u, payload_from("short"));
    bad.resize(bad.size() - 2);

    tp::FrameDecoder decoder;
    std::vector<Frame> frames;
    decoder.feed(good.data(), good.size(), frames);
    decoder.feed(bad.data(), bad.size(), frames);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].type == MsgType::Ping);
}

TEST_CASE("oversize len on a control frame: rejected from the header alone") {
    std::vector<unsigned char> header(tp::kEnvelopeHeaderBytes);
    header[0] = tp::kEnvelopeVersion;
    header[1] = static_cast<unsigned char>(MsgType::Cmd);
    const std::uint32_t bogus_len = static_cast<std::uint32_t>(tp::kMaxControlPayloadBytes + 1);
    header[6] = static_cast<unsigned char>(bogus_len & 0xFFu);
    header[7] = static_cast<unsigned char>((bogus_len >> 8) & 0xFFu);
    header[8] = static_cast<unsigned char>((bogus_len >> 16) & 0xFFu);
    header[9] = static_cast<unsigned char>((bogus_len >> 24) & 0xFFu);
    // Follow the toxic header with an otherwise perfectly valid frame: the
    // stream cannot be re-aligned, so nothing may be decoded from it.
    const auto tail = encode_of(MsgType::Ping, 9u, {});

    tp::FrameDecoder decoder;
    std::vector<Frame> frames;
    std::vector<unsigned char> stream = header;
    stream.insert(stream.end(), tail.begin(), tail.end());

    bool threw = false;
    try {
        decoder.feed(stream.data(), stream.size(), frames);
    } catch (const ProtocolError& e) {
        threw = true;
        CHECK(e.code() == ProtocolError::Code::OversizePayload);
    }
    CHECK(threw);
    CHECK(frames.empty()); // the trailing valid frame was NOT misread as data

    // Poisoning is idempotent: re-feeding anything re-throws the same error.
    bool rethrew = false;
    try {
        decoder.feed(tail.data(), tail.size(), frames);
    } catch (const ProtocolError& e) {
        rethrew = true;
        CHECK(e.code() == ProtocolError::Code::OversizePayload);
    }
    CHECK(rethrew);
    CHECK(frames.empty());
}

TEST_CASE("oversize len on FILE_CHUNK respects the separate chunk bound") {
    std::vector<unsigned char> header(tp::kEnvelopeHeaderBytes);
    header[0] = tp::kEnvelopeVersion;
    header[1] = static_cast<unsigned char>(MsgType::FileChunk);
    const std::uint32_t bogus_len = static_cast<std::uint32_t>(tp::kMaxFileChunkBytes + 1);
    header[6] = static_cast<unsigned char>(bogus_len & 0xFFu);
    header[7] = static_cast<unsigned char>((bogus_len >> 8) & 0xFFu);
    header[8] = static_cast<unsigned char>((bogus_len >> 16) & 0xFFu);
    header[9] = static_cast<unsigned char>((bogus_len >> 24) & 0xFFu);

    tp::FrameDecoder decoder;
    std::vector<Frame> frames;
    bool threw = false;
    try {
        decoder.feed(header.data(), header.size(), frames);
    } catch (const ProtocolError& e) {
        threw = true;
        CHECK(e.code() == ProtocolError::Code::OversizePayload);
    }
    CHECK(threw);
    CHECK(frames.empty());
}

TEST_CASE("payloads exactly at the declared bounds are accepted") {
    // Control frame at exactly 1 MiB.
    Frame control{MsgType::Cmd, 1u, std::vector<unsigned char>(tp::kMaxControlPayloadBytes, 0x61)};
    auto wire = tp::encode_frame(control);
    auto frames = decode_all(wire);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].type == MsgType::Cmd);
    CHECK(frames[0].payload.size() == tp::kMaxControlPayloadBytes);

    // File chunk at exactly 256 KiB.
    Frame chunk{MsgType::FileChunk, 2u, std::vector<unsigned char>(tp::kMaxFileChunkBytes, 0x62)};
    wire = tp::encode_frame(chunk);
    frames = decode_all(wire);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].type == MsgType::FileChunk);
    CHECK(frames[0].payload.size() == tp::kMaxFileChunkBytes);
}

TEST_CASE("encoder rejects payloads past the bound per type") {
    Frame fat_control{MsgType::Cmd, 1u,
                      std::vector<unsigned char>(tp::kMaxControlPayloadBytes + 1, 0x61)};
    CHECK_THROWS_AS(tp::encode_frame(fat_control), ProtocolError);

    Frame fat_chunk{MsgType::FileChunk, 1u,
                    std::vector<unsigned char>(tp::kMaxFileChunkBytes + 1, 0x62)};
    CHECK_THROWS_AS(tp::encode_frame(fat_chunk), ProtocolError);
}

TEST_CASE("unknown version is rejected and poisons the stream") {
    for (const unsigned char bad_version : {0, 1, 3}) {
        auto wire = encode_of(MsgType::Hello, 5u, payload_from("{}"));
        wire[0] = bad_version;

        tp::FrameDecoder decoder;
        std::vector<Frame> frames;
        bool threw = false;
        try {
            decoder.feed(wire.data(), wire.size(), frames);
        } catch (const ProtocolError& e) {
            threw = true;
            CHECK(e.code() == ProtocolError::Code::UnknownVersion);
        }
        CHECK(threw);
        CHECK(frames.empty());
    }
}

TEST_CASE("unknown type is rejected both below and above the enum range") {
    for (const unsigned char bad_type : {0, 13, 200}) {
        auto wire = encode_of(MsgType::Ping, 0, {});
        wire[1] = bad_type;

        tp::FrameDecoder decoder;
        std::vector<Frame> frames;
        bool threw = false;
        try {
            decoder.feed(wire.data(), wire.size(), frames);
        } catch (const ProtocolError& e) {
            threw = true;
            CHECK(e.code() == ProtocolError::Code::UnknownType);
        }
        CHECK(threw);
        CHECK(frames.empty());
    }
}

TEST_CASE("seq accepts the full uint32 range") {
    const std::uint32_t seq = 0xFFFFFFFFu;
    const auto wire = encode_of(MsgType::Cancel, seq, {});
    const auto frames = decode_all(wire);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].seq == seq);
}

TEST_CASE("zero-length payload frames decode as empty payloads") {
    const auto frames = decode_all(encode_of(MsgType::Ping, 0, {}));
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].type == MsgType::Ping);
    CHECK(frames[0].payload.empty());
    CHECK(frames[0].seq == 0);
}

TEST_CASE("type names and bounds metadata") {
    CHECK(std::string(tp::msg_type_name(MsgType::Hello)) == "HELLO");
    CHECK(std::string(tp::msg_type_name(MsgType::Challenge)) == "CHALLENGE");
    CHECK(std::string(tp::msg_type_name(MsgType::AuthOk)) == "AUTH_OK");
    CHECK(std::string(tp::msg_type_name(MsgType::Cmd)) == "CMD");
    CHECK(std::string(tp::msg_type_name(MsgType::ResultText)) == "RESULT_TEXT");
    CHECK(std::string(tp::msg_type_name(MsgType::FileMeta)) == "FILE_META");
    CHECK(std::string(tp::msg_type_name(MsgType::FileChunk)) == "FILE_CHUNK");
    CHECK(std::string(tp::msg_type_name(MsgType::FileEnd)) == "FILE_END");
    CHECK(std::string(tp::msg_type_name(MsgType::Cancel)) == "CANCEL");
    CHECK(std::string(tp::msg_type_name(MsgType::Ping)) == "PING");
    CHECK(std::string(tp::msg_type_name(MsgType::Pong)) == "PONG");
    CHECK(std::string(tp::msg_type_name(MsgType::Err)) == "ERR");
    CHECK(tp::is_valid_msg_type(1));
    CHECK(tp::is_valid_msg_type(12));
    CHECK_FALSE(tp::is_valid_msg_type(0));
    CHECK_FALSE(tp::is_valid_msg_type(13));
    CHECK(tp::max_payload_bytes_for(MsgType::FileChunk) == tp::kMaxFileChunkBytes);
    CHECK(tp::max_payload_bytes_for(MsgType::Cmd) == tp::kMaxControlPayloadBytes);
}
