// lib/protocol/payloads.cpp - JSON payload mapping (design D3, task 3.2).
// See payloads.hpp for the shape rules and the strict-validation contract.
#include "payloads.hpp"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>

namespace tether::protocol {

namespace {

using json = nlohmann::json;

[[noreturn]] void fail(MsgType type, const std::string& problem) {
    throw ProtocolError(ProtocolError::Code::MalformedPayload,
                        std::string(msg_type_name(type)) + " payload " + problem);
}

// Every control payload is a JSON object; anything else is a shape error that
// names the type, not a confusing generic parse error.
json parse_json_object(MsgType type, const std::vector<unsigned char>& payload) {
    const std::string raw(payload.begin(), payload.end());
    json parsed = json::parse(raw, nullptr, false);
    if (parsed.is_discarded()) {
        fail(type, "is not valid JSON: '" + raw.substr(0, 32) + "'");
    }
    if (!parsed.is_object()) {
        fail(type, "is not a JSON object");
    }
    return parsed;
}

void require_field(MsgType type, const json& object, const char* field, json::value_t wanted,
                   const char* wanted_desc) {
    if (!object.contains(field)) {
        fail(type, std::string("is missing the required field '") + field + "'");
    }
    if (object.at(field).type() != wanted) {
        fail(type, std::string("field '") + field + "' must be " + wanted_desc +
                       ", got " + std::string(object.at(field).type_name()));
    }
}

void require_string(MsgType type, const json& object, const char* field, bool non_empty) {
    require_field(type, object, field, json::value_t::string, "a string");
    if (non_empty && object.at(field).get<std::string>().empty()) {
        fail(type, std::string("field '") + field + "' must be a non-empty string");
    }
}

void reject_unknown_fields(MsgType type, const json& object,
                           std::initializer_list<const char*> known) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::find_if(known.begin(), known.end(), [&](const char* name) {
                return it.key() == name;
            }) == known.end()) {
            fail(type, std::string("has unknown field '") + it.key() + "'");
        }
    }
}

void expect_empty_object(MsgType type, const json& object) {
    reject_unknown_fields(type, object, {});
}

// The variant arm order is the single mapping source between Message types
// and wire discriminators; kept in lockstep with the enum in envelope.hpp.
using MessageVariantTypes =
    std::array<MsgType, std::variant_size_v<Message>>;

constexpr MessageVariantTypes make_variant_type_table() {
    return MessageVariantTypes{MsgType::Hello, MsgType::Challenge, MsgType::AuthOk,
                               MsgType::Cmd,   MsgType::ResultText, MsgType::FileMeta,
                               MsgType::FileChunk, MsgType::FileEnd, MsgType::Cancel,
                               MsgType::Ping,  MsgType::Pong,       MsgType::Err};
}

const MessageVariantTypes kVariantTypeTable = make_variant_type_table();

static_assert(std::variant_size_v<Message> == 12, "payload variant must cover all 12 MsgTypes");

Hello decode_hello(MsgType type, const json& object) {
    reject_unknown_fields(type, object, {"protocol_version", "agent_name", "agent_id"});
    require_field(type, object, "protocol_version", json::value_t::number_unsigned,
                  "an unsigned integer");
    const auto version = object.at("protocol_version").get<unsigned long long>();
    if (version != kEnvelopeVersion) {
        fail(type, "carries protocol_version " + std::to_string(version) +
                       " which does not match envelope version " +
                       std::to_string(kEnvelopeVersion));
    }
    require_string(type, object, "agent_name", true);
    require_string(type, object, "agent_id", true);
    return Hello{static_cast<unsigned>(version), object.at("agent_name").get<std::string>(),
                 object.at("agent_id").get<std::string>()};
}

Challenge decode_challenge(MsgType type, const json& object) {
    reject_unknown_fields(type, object, {"nonce", "hmac_response"});
    require_string(type, object, "nonce", true);
    Challenge challenge;
    challenge.nonce = object.at("nonce").get<std::string>();
    if (challenge.nonce.size() != kNonceHexChars) {
        fail(type, "field 'nonce' must be " + std::to_string(kNonceHexChars) +
                       " hex characters, got " + std::to_string(challenge.nonce.size()));
    }
    if (object.contains("hmac_response")) {
        require_string(type, object, "hmac_response", true);
        challenge.hmac_response = object.at("hmac_response").get<std::string>();
        if (challenge.hmac_response.size() != kHmacHexChars) {
            fail(type, "field 'hmac_response' must be " + std::to_string(kHmacHexChars) +
                           " hex characters, got " + std::to_string(challenge.hmac_response.size()));
        }
    }
    return challenge;
}

Cmd decode_cmd(MsgType type, const json& object) {
    reject_unknown_fields(type, object, {"command", "args"});
    require_string(type, object, "command", true);
    require_field(type, object, "args", json::value_t::array, "an array");
    Cmd command;
    command.command = object.at("command").get<std::string>();
    for (const auto& arg : object.at("args")) {
        if (!arg.is_string()) {
            fail(type, std::string("field 'args' must contain only strings, got ") +
                           std::string(arg.type_name()));
        }
        command.args.push_back(arg.get<std::string>());
    }
    return command;
}

ResultText decode_result_text(MsgType type, const json& object) {
    reject_unknown_fields(type, object, {"text"});
    require_string(type, object, "text", true);
    return ResultText{object.at("text").get<std::string>()};
}

FileMeta decode_file_meta(MsgType type, const json& object) {
    reject_unknown_fields(type, object, {"name", "size"});
    require_string(type, object, "name", true);
    require_field(type, object, "size", json::value_t::number_unsigned, "an unsigned integer");
    return FileMeta{object.at("name").get<std::string>(),
                    object.at("size").get<std::uint64_t>()};
}

Err decode_err(MsgType type, const json& object) {
    reject_unknown_fields(type, object, {"code", "message"});
    require_string(type, object, "code", true);
    require_string(type, object, "message", true);
    return Err{object.at("code").get<std::string>(), object.at("message").get<std::string>()};
}

std::string to_json_string(const json& object) { return object.dump(); }

std::vector<unsigned char> bytes_from_string(const std::string& text) {
    return std::vector<unsigned char>(text.begin(), text.end());
}

} // namespace

MsgType message_type_of(const Message& message) {
    return kVariantTypeTable[message.index()];
}

std::vector<unsigned char> encode_payload(const Message& message) {
    if (const auto* chunk = std::get_if<FileChunk>(&message)) {
        return chunk->bytes; // FILE_CHUNK is the one raw-byte branch (D3)
    }
    json object = json::object();
    if (std::holds_alternative<Hello>(message)) {
        const auto& hello = std::get<Hello>(message);
        object["protocol_version"] = hello.protocol_version;
        object["agent_name"] = hello.agent_name;
        object["agent_id"] = hello.agent_id;
    } else if (std::holds_alternative<Challenge>(message)) {
        const auto& challenge = std::get<Challenge>(message);
        object["nonce"] = challenge.nonce;
        if (!challenge.hmac_response.empty()) {
            object["hmac_response"] = challenge.hmac_response;
        }
    } else if (std::holds_alternative<Cmd>(message)) {
        const auto& command = std::get<Cmd>(message);
        object["command"] = command.command;
        object["args"] = command.args;
    } else if (std::holds_alternative<ResultText>(message)) {
        object["text"] = std::get<ResultText>(message).text;
    } else if (std::holds_alternative<FileMeta>(message)) {
        const auto& meta = std::get<FileMeta>(message);
        object["name"] = meta.name;
        object["size"] = meta.size;
    } else if (std::holds_alternative<Err>(message)) {
        const auto& error = std::get<Err>(message);
        object["code"] = error.code;
        object["message"] = error.message;
    } else if (std::holds_alternative<AuthOk>(message) || std::holds_alternative<FileEnd>(message) ||
               std::holds_alternative<Cancel>(message) || std::holds_alternative<Ping>(message) ||
               std::holds_alternative<Pong>(message)) {
        // deliberately empty payloads per the v2 type list
    } else {
        throw ProtocolError(ProtocolError::Code::BadArgument,
                            "message variant has no payload mapping (bad index " +
                                std::to_string(message.index()) + ")");
    }
    return bytes_from_string(to_json_string(object));
}

std::vector<unsigned char> encode_message(const Message& message, std::uint32_t seq) {
    return encode_frame(Frame{message_type_of(message), seq, encode_payload(message)});
}

Message decode_payload(MsgType type, const std::vector<unsigned char>& payload) {
    if (type == MsgType::FileChunk) {
        return FileChunk{payload}; // raw bytes; JSON parsing would corrupt them
    }
    const json object = parse_json_object(type, payload);
    switch (type) {
        case MsgType::Hello: return Message{decode_hello(type, object)};
        case MsgType::Challenge: return Message{decode_challenge(type, object)};
        case MsgType::AuthOk:
            expect_empty_object(type, object);
            return Message{AuthOk{}};
        case MsgType::Cmd: return Message{decode_cmd(type, object)};
        case MsgType::ResultText: return Message{decode_result_text(type, object)};
        case MsgType::FileMeta: return Message{decode_file_meta(type, object)};
        case MsgType::FileChunk: break; // handled above; unreachable
        case MsgType::FileEnd:
            expect_empty_object(type, object);
            return Message{FileEnd{}};
        case MsgType::Cancel:
            expect_empty_object(type, object);
            return Message{Cancel{}};
        case MsgType::Ping:
            expect_empty_object(type, object);
            return Message{Ping{}};
        case MsgType::Pong:
            expect_empty_object(type, object);
            return Message{Pong{}};
        case MsgType::Err: return Message{decode_err(type, object)};
    }
    throw ProtocolError(ProtocolError::Code::UnknownType,
                        std::string("no payload decoder for type ") + msg_type_name(type));
}

} // namespace tether::protocol
