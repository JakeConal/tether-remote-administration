// lib/common/config.cpp - JSON config file loading (design D8, task 2.2).
//
// All failure paths are explicit: the loader throws ConfigError with a
// message naming the config file and (where known) the offending field.
// Structural validation lives here; semantic validation follows features.
#include "config.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <ios>
#include <vector>

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

// Optional integer field with a validated range; returns the default when the
// key is absent (additive config: old agent configs keep loading).
int optional_port(const json& doc, const std::string& path, const char* field, int fallback) {
    const auto it = doc.find(field);
    if (it == doc.end() || it->is_null()) {
        return fallback;
    }
    if (!it->is_number_integer()) {
        throw ConfigError(path + ": field '" + std::string(field) + "' must be an integer");
    }
    const int port = it->get<int>();
    if (port < 1 || port > 65535) {
        throw ConfigError(path + ": field '" + std::string(field) +
                          "' must be an integer in [1, 65535]");
    }
    return port;
}

GmailSectionConfig parse_gmail_section(const json& doc, const std::string& path) {
    const json& section = required_field(doc, path, "gmail");
    if (section.is_null()) {
        return GmailSectionConfig{}; // explicit null -> disabled (still structured)
    }
    if (!section.is_object()) {
        throw ConfigError(path + ": field 'gmail' must be an object");
    }
    // Compose once: passing `path + ":gmail"` inline would bind the error
    // prefix to the string temporary's lifetime (gcc -Wdangling-reference).
    const std::string gmail_path = path + ":gmail";
    const json& enabled = required_field(section, gmail_path, "enabled");
    if (!enabled.is_boolean()) {
        throw ConfigError(path + ": field 'gmail.enabled' must be a boolean");
    }
    GmailSectionConfig gmail;
    gmail.enabled = enabled.get<bool>();
    if (!gmail.enabled) {
        // Additive friendliness: a disabled section records no further fields.
        return gmail;
    }
    gmail.credentials_path = required_string(section, gmail_path, "credentials_path");
    if (gmail.credentials_path.empty()) {
        throw ConfigError(path + ": field 'gmail.credentials_path' must be a non-empty path");
    }
    const json& interval = required_field(section, gmail_path, "poll_interval_seconds");
    if (!interval.is_number_integer()) {
        throw ConfigError(path + ": field 'gmail.poll_interval_seconds' must be an integer");
    }
    gmail.poll_interval_seconds = interval.get<int>();
    if (gmail.poll_interval_seconds < 1) {
        throw ConfigError(
            path + ": field 'gmail.poll_interval_seconds' must be at least a whole second");
    }
    const json& admins = required_field(section, gmail_path, "admin_list");
    if (!admins.is_array() || admins.empty()) {
        throw ConfigError(path + ": field 'gmail.admin_list' must be a non-empty array of addresses");
    }
    for (const auto& admin : admins) {
        if (!admin.is_string() || admin.get<std::string>().empty()) {
            throw ConfigError(path + ": every gmail.admin_list entry must be a non-empty string");
        }
        gmail.admin_list.push_back(admin.get<std::string>());
    }
    gmail.default_agent = required_string(section, gmail_path, "default_agent");
    if (gmail.default_agent.empty()) {
        throw ConfigError(path + ": field 'gmail.default_agent' must be a non-empty identity");
    }
    return gmail;
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
    if (config.token_store_path.empty()) {
        throw ConfigError(path + ": field 'token_store_path' must be a non-empty path");
    }
    if (doc.find("gmail") != doc.end()) {
        config.gmail = parse_gmail_section(doc, path);
    }
    // Additive (task 6.9): the optional console audit trail path. Absent or
    // explicit null keeps auditing off; a non-string is a config error.
    if (const auto audit = doc.find("audit_path"); audit != doc.end() && !audit->is_null()) {
        if (!audit->is_string()) {
            throw ConfigError(path + ": field 'audit_path' must be a string");
        }
        config.audit_path = audit->get<std::string>();
    }
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
    config.console_port = optional_port(doc, path, "console_port", 47474);
    config.agent_name = required_string(doc, path, "agent_name");
    // Empty token passes structural validation (example templates ship it);
    // the handshake layer rejects it when authentication lands (task 3.3).
    config.token = required_string(doc, path, "token");
    if (doc.find("instance_id") != doc.end() && doc.at("instance_id").is_string()) {
        config.instance_id = doc.at("instance_id").get<std::string>();
    }
    return config;
}

} // namespace tether::common
