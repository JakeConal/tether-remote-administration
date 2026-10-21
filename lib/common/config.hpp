// lib/common/config.hpp - configuration file loading (design D8, task 2.2;
// Phase 4 additions: the optional console `gmail` section and the agent
// console dial-out port, both additive).
//
// v2 replaces v1's hardcoded constants (port 12345, admin list, plaintext
// tokens) with per-runtime config files. Every problem is an explicit
// failure naming the file (and the field where known) - the loader never
// substitutes silent defaults.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace tether::common {

// The optional `gmail` console section (design D2: the Gmail adapter is
// independently enableable; a TCP-only config simply omits it):
// - enabled:              turns the adapter on for this console
// - credentials_path:     OAuth refresh-token credentials FILE OUTSIDE the
//                         repo ({account, client_id, client_secret,
//                         refresh_token}); secrets never enter the tree
// - poll_interval_seconds: inbox polling cadence (>= 1 when enabled)
// - admin_list:           sender allowlist; mail from other senders is
//                         ignored harmlessly (never executes)
// - default_agent:        the registry identity mail-issued commands route to
//                         when a mail body carries no explicit agent selector
struct GmailSectionConfig {
    bool enabled = false;
    std::string credentials_path;
    int poll_interval_seconds = 0;
    std::vector<std::string> admin_list;
    std::string default_agent;
};

// Fields for the console (operator side), see docs/examples: console.example.json.
// - listen_address:   address the console accepts agent sessions on
// - port:             port the console listens on (1..65535)
// - token_store_path: where the console reads its provisioned agent tokens
// - gmail:            optional Gmail adapter section (absent/null = disabled)
// - audit_path:       optional task-6.9 audit trail file (JSONL). Absent,
//                     null, or an empty string disables the audit trail;
//                     a set path turns on console-side audit records
//                     (executed/refused/failed commands and background-job
//                     lifecycle events persist there and re-load on open).
struct ConsoleConfig {
    std::string listen_address;
    int port = 0;
    std::string token_store_path;
    std::optional<GmailSectionConfig> gmail;
    std::optional<std::string> audit_path;
};

// Fields for the agent (target side), see docs: agent.example.json.
// - console_address: address the agent dials out to (design D1)
// - console_port:    optional dial-out port (absent -> the default 47474,
//                    matching console.example.json)
// - agent_name:      the agent's unique registry identity
// - instance_id:     optional install instance id (defaults to agent_name)
// - token:           pre-shared token for the HMAC handshake (design D4)
struct AgentConfig {
    std::string console_address;
    int console_port = 47474;
    std::string agent_name;
    std::string instance_id;
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
