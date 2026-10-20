// lib/common/config.cpp - JSON config file loading (design D8, task 2.2).
//
// All failure paths are explicit: the loader throws ConfigError with a
// message naming the config file and (where known) the offending field.
// Structural validation lives here; semantic validation follows features.
#include "config.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <ios>

namespace tether::common {
namespace {

using nlohmann::json;

// Reads and parses the file. Missing/unopenable file and malformed JSON both
// become ConfigError; nlohmann's own exception never escapes this module.
void parse_config_file(const std::string& path, json& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw ConfigError(path + ": cannot open config file (missing or unreadable)");
    }
    try {
        in >> out;
    } catch (const json::parse_error& e) {
        throw ConfigError(path + ": malformed JSON (" + e.what() + ")");
    }
}

const json& required_field(const json& doc, const std::string& path, const char* field) {
    const auto it = doc.find(field);
    if (it == doc.end()) {
        throw ConfigError(path + ": missing required field '" + field + "'");
    }
    return *it;
}

std::string required_string(const json& doc, const std::string& path, const char* field) {
    const json& value = required_field(doc, path, field);
    if (!value.is_string()) {
        throw ConfigError(path + ": field '" + field + "' must be a string");
    }
    return value.get<std::string>();
}

int required_port(const json& doc, const std::string& path) {
    const json& value = required_field(doc, path, "port");
    if (!value.is_number_integer()) {
        throw ConfigError(path + ": field 'port' must be an integer");
    }
    const int port = value.get<int>();
    if (port < 1 || port > 65535) {
        throw ConfigError(path + ": field 'port' must be an integer in [1, 65535]");
    }
    return port;
}

} // namespace

ConsoleConfig load_console_config(const std::string& path) {
    json doc;
    parse_config_file(path, doc);
    if (!doc.is_object()) {
        throw ConfigError(path + ": config root must be a JSON object");
    }
    ConsoleConfig config;
    config.listen_address = required_string(doc, path, "listen_address");
    config.port = required_port(doc, path);
    config.token_store_path = required_string(doc, path, "token_store_path");
    return config;
}

AgentConfig load_agent_config(const std::string& path) {
    json doc;
    parse_config_file(path, doc);
    if (!doc.is_object()) {
        throw ConfigError(path + ": config root must be a JSON object");
    }
    AgentConfig config;
    config.console_address = required_string(doc, path, "console_address");
    config.agent_name = required_string(doc, path, "agent_name");
    // Empty token passes structural validation (example templates ship it);
    // the handshake layer rejects it when authentication lands (task 3.3).
    config.token = required_string(doc, path, "token");
    return config;
}

} // namespace tether::common
