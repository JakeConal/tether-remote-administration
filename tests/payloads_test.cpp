// tests/payloads_test.cpp - task 3.2 verification: JSON payload handling for
// every protocol v2 type with strict per-type field validation. Every malformed
// shape below must fail with ProtocolError(MalformedPayload) and the message
// must name the offending field.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "payloads.hpp"

#include <nlohmann/json.hpp>

namespace tp = tether::protocol;
using tp::MsgType;
using tp::ProtocolError;

namespace {

// JSON text -> raw payload bytes (test-side convenience).
std::vector<unsigned char> raw_json(const std::string& text) {
    return std::vector<unsigned char>(text.begin(), text.end());
}

bool fails_with(const std::string& expected_fragment, tp::MsgType type,
                const std::vector<unsigned char>& payload) {
    bool named_the_field = false;
    try {
        static_cast<void>(tp::decode_payload(type, payload));
    } catch (const ProtocolError& e) {
        CHECK(e.code() == ProtocolError::Code::MalformedPayload);
        named_the_field = std::string{e.what()}.find(expected_fragment) != std::string::npos;
    }
    return named_the_field;
}

} // namespace

TEST_CASE("HELLO round-trip and field shape") {
    const tp::Hello hello{2u, "agent-1", "host-pc-1"};
    const auto wire = tp::encode_message(hello, 0);
    const auto frames = tp::encode_frame(tp::Frame{MsgType::Hello, 0, tp::encode_payload(hello)});
    CHECK(frames == wire);
    const auto decoded = tp::decode_payload(MsgType::Hello, tp::encode_payload(hello));
    REQUIRE(std::holds_alternative<tp::Hello>(decoded));
    const auto& back = std::get<tp::Hello>(decoded);
    CHECK(back.protocol_version == 2u);
    CHECK(back.agent_name == "agent-1");
    CHECK(back.agent_id == "host-pc-1");
    CHECK(tp::message_type_of(hello) == MsgType::Hello);

    // JSON sanity: the wire payload is exactly the three documented fields.
    const auto payload = tp::encode_payload(hello);
    const auto parsed = nlohmann::json::parse(std::string(payload.begin(), payload.end()));
    CHECK(parsed.size() == 3);
}

TEST_CASE("HELLO rejects wrong protocol_version values") {
    const std::string bad = R"({"protocol_version":1,"agent_name":"a","agent_id":"b"})";
    CHECK(fails_with("protocol_version", MsgType::Hello, raw_json(bad)));
    const std::string too_new = R"({"protocol_version":3,"agent_name":"a","agent_id":"b"})";
    CHECK(fails_with("protocol_version", MsgType::Hello, raw_json(too_new)));
    const std::string as_string = R"({"protocol_version":"2","agent_name":"a","agent_id":"b"})";
    CHECK(fails_with("protocol_version", MsgType::Hello, raw_json(as_string)));
    const std::string as_float = R"({"protocol_version":2.0,"agent_name":"a","agent_id":"b"})";
    CHECK(fails_with("protocol_version", MsgType::Hello, raw_json(as_float)));
    const std::string missing = R"({"agent_name":"a","agent_id":"b"})";
    CHECK(fails_with("protocol_version", MsgType::Hello, raw_json(missing)));
}

TEST_CASE("HELLO rejects malformed identity fields") {
    CHECK(fails_with("agent_name", MsgType::Hello, raw_json(R"({"protocol_version":2})")));
    CHECK(fails_with("agent_id", MsgType::Hello,
                      raw_json(R"({"protocol_version":2,"agent_name":"a"})")));
    CHECK(fails_with("agent_name", MsgType::Hello,
                      raw_json(R"({"protocol_version":2,"agent_name":"","agent_id":"b"})")));
    CHECK(fails_with("agent_id", MsgType::Hello,
                      raw_json(R"({"protocol_version":2,"agent_name":"a","agent_id":""})")));
    CHECK(fails_with("agent_name", MsgType::Hello,
                      raw_json(R"({"protocol_version":2,"agent_name":7,"agent_id":"b"})")));
    CHECK(fails_with("extra", MsgType::Hello,
                      raw_json(
                          R"({"protocol_version":2,"agent_name":"a","agent_id":"b","extra":1})")));
}

TEST_CASE("CHALLENGE round-trips both direction forms") {
    // Console -> agent: nonce only.
    const tp::Challenge plain{"a1b2c3d4e5f60718293a4b5c6d7e8f90", ""};
    const auto plain_back =
        tp::decode_payload(MsgType::Challenge, tp::encode_payload(plain));
    REQUIRE(std::holds_alternative<tp::Challenge>(plain_back));
    CHECK(std::get<tp::Challenge>(plain_back).nonce == plain.nonce);
    CHECK(std::get<tp::Challenge>(plain_back).hmac_response.empty());

    // Agent -> console: the direction overload with an hmac_response.
    const std::string hmac(64, 'a');
    const tp::Challenge answer{"a1b2c3d4e5f60718293a4b5c6d7e8f90", hmac};
    const auto answer_back =
        tp::decode_payload(MsgType::Challenge, tp::encode_payload(answer));
    REQUIRE(std::holds_alternative<tp::Challenge>(answer_back));
    CHECK(std::get<tp::Challenge>(answer_back).hmac_response == hmac);
}

TEST_CASE("CHALLENGE rejects malformed nonce/hmac shapes") {
    const std::string nonce = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
    CHECK(fails_with("nonce", MsgType::Challenge, raw_json("{}")));
    CHECK(fails_with("nonce", MsgType::Challenge, raw_json(R"({"nonce":""})")));
    CHECK(fails_with("nonce", MsgType::Challenge, raw_json(R"({"nonce":123})")));
    CHECK(fails_with("nonce", MsgType::Challenge, raw_json(R"({"nonce":"tooshort"})")));
    CHECK(fails_with("hmac_response", MsgType::Challenge,
                      raw_json(R"({"nonce":")" + nonce + R"(","hmac_response":""})")));
    CHECK(fails_with("hmac_response", MsgType::Challenge,
                      raw_json(R"({"nonce":")" + nonce + R"(","hmac_response":"zzzz"})")));
    CHECK(fails_with("extra", MsgType::Challenge,
                      raw_json(R"({"nonce":")" + nonce + R"(","hmac_response":" )" + nonce +
                               R"(","extra":1})")));

    // malformed JSON syntax itself
    CHECK(fails_with("JSON", MsgType::Challenge, raw_json("{not json")));
    CHECK(fails_with("object", MsgType::Challenge, raw_json("[1,2,3]")));
}

TEST_CASE("AUTH_OK accepts exactly the empty object") {
    const auto decoded = tp::decode_payload(MsgType::AuthOk, raw_json("{}"));
    REQUIRE(std::holds_alternative<tp::AuthOk>(decoded));
    CHECK(fails_with("status", MsgType::AuthOk, raw_json(R"({"status":"ok"})")));
    CHECK(fails_with("'nonce'", MsgType::AuthOk, raw_json(R"({"nonce":"x"})")));
}

TEST_CASE("CMD round-trip and rejecting malformed shapes") {
    const tp::Cmd command{"exec", {"ps", "-aux"}};
    const auto decoded = tp::decode_payload(MsgType::Cmd, tp::encode_payload(command));
    REQUIRE(std::holds_alternative<tp::Cmd>(decoded));
    const auto& back = std::get<tp::Cmd>(decoded);
    CHECK(back.command == "exec");
    CHECK(back.args == std::vector<std::string>{"ps", "-aux"});

    // Empty args array is a legal shape.
    const auto no_args = tp::decode_payload(MsgType::Cmd, tp::encode_payload(tp::Cmd{"list", {}}));
    REQUIRE(std::holds_alternative<tp::Cmd>(no_args));
    CHECK(std::get<tp::Cmd>(no_args).args.empty());

    CHECK(fails_with("command", MsgType::Cmd, raw_json(R"({"args":[]})")));
    CHECK(fails_with("command", MsgType::Cmd, raw_json(R"({"command":"","args":[]})")));
    CHECK(fails_with("args", MsgType::Cmd, raw_json(R"({"command":"exec"})")));
    CHECK(fails_with("args", MsgType::Cmd, raw_json(R"({"command":"exec","args":"ps"})")));
    CHECK(fails_with("args", MsgType::Cmd, raw_json(R"({"command":"exec","args":[1,"x"]})")));
    CHECK(fails_with("verb", MsgType::Cmd, raw_json(R"({"command":"exec","args":[],"verb":1})")));
}

TEST_CASE("RESULT_TEXT round-trip and rejecting malformed shapes") {
    const tp::ResultText result{"hello\nmultiline"};
    const auto decoded = tp::decode_payload(MsgType::ResultText, tp::encode_payload(result));
    REQUIRE(std::holds_alternative<tp::ResultText>(decoded));
    CHECK(std::get<tp::ResultText>(decoded).text == "hello\nmultiline");

    CHECK(fails_with("text", MsgType::ResultText, raw_json("{}")));
    CHECK(fails_with("text", MsgType::ResultText, raw_json(R"({"text":5})")));
    CHECK(fails_with("size", MsgType::ResultText, raw_json(R"({"text":"x","size":2})")));
}

TEST_CASE("FILE_META round-trip with exact 64-bit sizes") {
    const std::uint64_t big = 18446744073709551615ull; // uint64 max
    const tp::FileMeta meta{"shot.png", big};
    const auto decoded = tp::decode_payload(MsgType::FileMeta, tp::encode_payload(meta));
    REQUIRE(std::holds_alternative<tp::FileMeta>(decoded));
    const auto& back = std::get<tp::FileMeta>(decoded);
    CHECK(back.name == "shot.png");
    CHECK(back.size == big);

    CHECK(fails_with("name", MsgType::FileMeta, raw_json(R"({"size":10})")));
    CHECK(fails_with("name", MsgType::FileMeta, raw_json(R"({"name":"","size":10})")));
    CHECK(fails_with("size", MsgType::FileMeta, raw_json(R"({"name":"a"})")));
    CHECK(fails_with("size", MsgType::FileMeta, raw_json(R"({"name":"a","size":-5})")));
    CHECK(fails_with("size", MsgType::FileMeta, raw_json(R"({"name":"a","size":1.5})")));
    CHECK(fails_with("size", MsgType::FileMeta, raw_json(R"({"name":"a","size":"10"})")));
}

TEST_CASE("FILE_CHUNK is the raw-byte branch: byte-exact round-trip") {
    std::vector<unsigned char> bytes;
    for (int i = 0; i < 64; ++i) {
        bytes.push_back(static_cast<unsigned char>((i % 2 == 0) ? 0x00 : 0xFF));
    }
    const tp::FileChunk chunk{bytes};
    CHECK(tp::message_type_of(chunk) == MsgType::FileChunk);
    const auto payload = tp::encode_payload(chunk);
    CHECK(payload == bytes); // no JSON quoting - raw bytes on the wire
    const auto decoded = tp::decode_payload(MsgType::FileChunk, payload);
    REQUIRE(std::holds_alternative<tp::FileChunk>(decoded));
    CHECK(std::get<tp::FileChunk>(decoded).bytes == bytes);
}

TEST_CASE("FILE_END strictness") {
    CHECK(std::holds_alternative<tp::FileEnd>(tp::decode_payload(MsgType::FileEnd, raw_json("{}"))));
    CHECK(fails_with("checksum", MsgType::FileEnd, raw_json(R"({"checksum":1})")));
}

TEST_CASE("CANCEL strictness") {
    CHECK(std::holds_alternative<tp::Cancel>(tp::decode_payload(MsgType::Cancel, raw_json("{}"))));
    CHECK(fails_with("target", MsgType::Cancel, raw_json(R"({"target":1})")));
}

TEST_CASE("PING / PONG strictness") {
    CHECK(std::holds_alternative<tp::Ping>(tp::decode_payload(MsgType::Ping, raw_json("{}"))));
    CHECK(std::holds_alternative<tp::Pong>(tp::decode_payload(MsgType::Pong, raw_json("{}"))));
    CHECK(fails_with("epoch", MsgType::Ping, raw_json(R"({"epoch":1})")));
    CHECK(fails_with("epoch", MsgType::Pong, raw_json(R"({"epoch":1})")));
}

TEST_CASE("ERR round-trip and rejecting malformed shapes") {
    const tp::Err error{"boom", "disk on fire"};
    const auto decoded = tp::decode_payload(MsgType::Err, tp::encode_payload(error));
    REQUIRE(std::holds_alternative<tp::Err>(decoded));
    const auto& back = std::get<tp::Err>(decoded);
    CHECK(back.code == "boom");
    CHECK(back.message == "disk on fire");

    CHECK(fails_with("code", MsgType::Err, raw_json(R"({"message":"m"})")));
    CHECK(fails_with("message", MsgType::Err, raw_json(R"({"code":"boom"})")));
    CHECK(fails_with("code", MsgType::Err, raw_json(R"({"code":1,"message":"m"})")));
    CHECK(fails_with("message", MsgType::Err, raw_json(R"({"code":"boom","message":""})")));
}

TEST_CASE("encode_message derives the envelope from message + seq") {
    const auto wire = tp::encode_message(tp::Cmd{"snap", {}}, 77u);
    tp::FrameDecoder decoder;
    std::vector<tp::Frame> frames;
    decoder.feed(wire.data(), wire.size(), frames);
    decoder.finish();
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].type == MsgType::Cmd);
    CHECK(frames[0].seq == 77u);
    const auto decoded = tp::decode_payload(frames[0].type, frames[0].payload);
    REQUIRE(std::holds_alternative<tp::Cmd>(decoded));
    CHECK(std::get<tp::Cmd>(decoded).command == "snap");
}

TEST_CASE("message_type_of covers all 12 variant arms") {
    CHECK(tp::message_type_of(tp::Hello{2, "a", "b"}) == MsgType::Hello);
    CHECK(tp::message_type_of(tp::Challenge{"n", "h"}) == MsgType::Challenge);
    CHECK(tp::message_type_of(tp::AuthOk{}) == MsgType::AuthOk);
    CHECK(tp::message_type_of(tp::Cmd{"c", {}}) == MsgType::Cmd);
    CHECK(tp::message_type_of(tp::ResultText{""}) == MsgType::ResultText);
    CHECK(tp::message_type_of(tp::FileMeta{"", 0}) == MsgType::FileMeta);
    CHECK(tp::message_type_of(tp::FileChunk{{}}) == MsgType::FileChunk);
    CHECK(tp::message_type_of(tp::FileEnd{}) == MsgType::FileEnd);
    CHECK(tp::message_type_of(tp::Cancel{}) == MsgType::Cancel);
    CHECK(tp::message_type_of(tp::Ping{}) == MsgType::Ping);
    CHECK(tp::message_type_of(tp::Pong{}) == MsgType::Pong);
    CHECK(tp::message_type_of(tp::Err{"", ""}) == MsgType::Err);
}
