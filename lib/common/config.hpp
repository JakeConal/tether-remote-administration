// lib/common/config.hpp - configuration file loading (design D8, task 2.2).
//
// v2 replaces v1's hardcoded constants (port 12345, admin list, plaintext
// tokens) with per-runtime config files. Every problem is an explicit
// failure naming the file (and the field where known) - the loader never
// substitutes silent defaults.
#pragma once

#include <stdexcept>
#include <string>

namespace tether::common {

// Fields for the console (operator side), see docs/examples: console.example.json.
// - listen_address:   address the console accepts agent sessions on
// - port:             port the console listens on (1..65535)
// - token_store_path: where the console reads its provisioned agent tokens
struct ConsoleConfig {
    std::string listen_address;
    int port = 0;
    std::string token_store_path;
};

// Fields for the agent (target side), see docs: agent.example.json.
// - console_address: address the agent dials out to (design D1)
// - agent_name:      the agent's unique registry identity
// - token:           pre-shared token for the HMAC handshake (design D4)
struct AgentConfig {
    std::string console_address;
    std::string agent_name;
    std::string token;
};

// Thrown for every configuration problem. The message always names the file,
// and names the offending field where the problem is field-level. Callers
// log/report it; text goes to the structured logger, never silently ignored.
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Load a console config file. Throws ConfigError on: unreadable/missing file,
// malformed JSON, non-object root, missing fields, wrong field types,
// out-of-range port. Only structural validation; semantic checks (e.g. a
// non-empty token) are enforced when the corresponding features land.
ConsoleConfig load_console_config(const std::string& path);

// Load an agent config file. Failure modes as above. An empty token string is
// structurally valid here (config example templates ship that way); session
// authentication will reject it when the handshake is enforced in Phase 3.
AgentConfig load_agent_config(const std::string& path);

} // namespace tether::common
