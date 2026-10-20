// tether-agent entry point (Phase 2 scaffolding, design D1).
// Phase 2 scope: initialize the structured logger, load config when a config
// file is found, print one readiness line, exit 0. Dial-out sessions, the
// HMAC handshake, and the command engine arrive in Phases 3-5.
#include "config.hpp"
#include "logging.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace {
constexpr const char* kDefaultConfigPath = "agent.json";
} // namespace

int main(int argc, char* argv[]) {
    using tether::common::AgentConfig;
    using tether::common::ConfigError;
    using tether::common::Logger;

    // Discover the config file path; none required to reach readiness here.
    std::string config_path = kDefaultConfigPath;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::string(argv[i]) == "--config") {
            config_path = argv[i + 1];
        }
    }

    Logger& log = Logger::instance();
    log.info("agent starting", {{"config", config_path}});

    if (std::filesystem::exists(config_path)) {
        try {
            const AgentConfig config = tether::common::load_agent_config(config_path);
            // The token itself is never logged (D8; re-verified at task 7.4).
            log.info("config loaded", {{"console_address", config.console_address},
                                       {"agent_name", config.agent_name}});
        } catch (const ConfigError& error) {
            log.error("config load failed", {{"error", error.what()}});
            return 1;
        }
    } else {
        log.warn("no config file found; proceeding without it this phase",
                 {{"config", config_path}});
    }

    log.info("tether-agent ready", {{"version", TETHER_VERSION}});
    return 0;
}
