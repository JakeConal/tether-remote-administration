// tether-console entry point (Phase 2 scaffolding, design D5).
// Phase 2 scope: initialize the structured logger, load config when a config
// file is found, print one readiness line, exit 0. The operator CLI verbs,
// agent registry, and streaming results arrive in Phase 5; the TCP listener
// and protocol hooks arrive in Phases 3-4.
#include "config.hpp"
#include "logging.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace {
constexpr const char* kDefaultConfigPath = "console.json";
} // namespace

int main(int argc, char* argv[]) {
    using tether::common::ConsoleConfig;
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
    log.info("console starting", {{"config", config_path}});

    if (std::filesystem::exists(config_path)) {
        try {
            const ConsoleConfig config = tether::common::load_console_config(config_path);
            log.info("config loaded", {{"listen_address", config.listen_address},
                                       {"port", std::to_string(config.port)},
                                       {"token_store_path", config.token_store_path}});
        } catch (const ConfigError& error) {
            log.error("config load failed", {{"error", error.what()}});
            return 1;
        }
    } else {
        log.warn("no config file found; proceeding without it this phase",
                 {{"config", config_path}});
    }

    log.info("tether-console ready", {{"version", TETHER_VERSION}});
    return 0;
}
