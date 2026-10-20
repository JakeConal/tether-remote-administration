// tether-agent entry point (design D1).
// Phase 4 scope: initialize the structured logger, load the config file, and
// run the dial-out transport loop - HTTP-style connect retries with bounded
// exponential backoff, then the P3 agent session state machine (handshake,
// keepalive, seq correlation) riding the TCP transport. Command EXECUTION is
// Phase 5: no handler is installed here, so inbound commands receive the
// protocol layer's structured ERR("no_handler") reply and the session stays
// usable.
#include "config.hpp"
#include "logging.hpp"
#include "session.hpp"
#include "tcp.hpp"
#include "transport.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {
constexpr const char* kDefaultConfigPath = "agent.json";

tether::transport::TransportMillis steady_now_ms() {
    return std::int64_t{std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count()};
}
} // namespace

int main(int argc, char* argv[]) {
    using tether::common::AgentConfig;
    using tether::common::ConfigError;
    using tether::common::Logger;
    namespace transport = tether::transport;

    // Discover the config file path.
    std::string config_path = kDefaultConfigPath;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::string(argv[i]) == "--config") {
            config_path = argv[i + 1];
        }
    }

    Logger& log = Logger::instance();
    log.info("agent starting", {{"config", config_path}});

    namespace protocol = tether::protocol;

    AgentConfig config;
    try {
        if (!std::filesystem::exists(config_path)) {
            log.error("config file is required to dial out", {{"config", config_path}});
            return 1;
        }
        config = tether::common::load_agent_config(config_path);
        // The token itself is never logged (D8; re-verified at task 7.4).
        log.info("config loaded", {{"console_address", config.console_address},
                                   {"console_port", std::to_string(config.console_port)},
                                   {"agent_name", config.agent_name}});
    } catch (const ConfigError& error) {
        log.error("config load failed", {{"error", error.what()}});
        return 1;
    }

    const protocol::AgentIdentity identity{
        config.agent_name, config.instance_id.empty() ? config.agent_name : config.instance_id};
    const transport::AgentEndpoint endpoint{
        config.console_address, static_cast<std::uint16_t>(config.console_port)};
    // The dial-out loop runs forever with the bounded-backoff policy (spec:
    // "retries connection attempts at a bounded interval instead of exiting").
    const transport::DialPolicy policy{};

    for (;;) {
        std::unique_ptr<transport::TcpAgentLink> link;
        try {
            link = transport::establish_with_retries(endpoint, identity, config.token,
                                                     protocol::SessionConfig{}, policy);
        } catch (const transport::TransportError& error) {
            // Unreachable with an unbounded budget, but failures must surface.
            log.error("dial-out gave up unexpectedly", {{"error", error.what()}});
            return 1;
        }
        transport::TransportMillis now = steady_now_ms();
        log.info("dial-out connected; starting session",
                 {{"agent", identity.name}, {"state", protocol::session_state_name(link->state())}});
        try {
            link->begin_handshake(now);
            while (!link->down() && !link->established()) {
                now = steady_now_ms();
                link->pump(now);
                if (!link->down() && !link->established()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            while (link->established()) {
                now = steady_now_ms();
                link->pump(now);
                if (!link->established()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } catch (const transport::TransportError& error) {
            log.error("session pump failed on dead wire", {{"error", error.what()}});
        }
        log.warn("session down; redialing",
                 {{"agent", identity.name},
                  {"final_state", protocol::session_state_name(link->down()
                                                                 ? protocol::SessionState::Down
                                                                 : link->state())}});
    }
}
