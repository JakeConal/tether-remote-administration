// tether-console entry point (design D5, Phase 5): a thin shell around the
// run_cli() verb surface. This main owns everything config-shaped - --config
// discovery, the structured logger, Windows stdout binary discipline for
// byte-exact redirects, the provisioned-token store, and the TCP listener
// (started lazily when a verb constructs the console) - and hands argv to the
// CLI layer (console/cli.cpp). All verb/arg parsing lives in tether-cli.
//
// Runtime shape (documented): one invocation = one verb. `tether-console
// exec a-1 ls > plan.txt` streams command results byte-exactly through the
// shell redirect; the session registry any invocation sees is the registry of
// the transport stack it starts for that invocation. The agent side is an
// always-running dial-out loop (bounded backoff), so each invocation pumps a
// bounded dial-in wait (`agent_dial_wait_ms` config field, TcpOperConsole
// setter) for the agent's next dial to land: that is what makes dial-out and
// one-shot verbs meet without a long-lived console daemon.
#include "audit.hpp"
#include "cli.hpp"
#include "config.hpp"
#include "logging.hpp"
#include "tcp.hpp"
#include "tcp_console.hpp"
#include "transport.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr const char* kDefaultConfigPath = "console.json";

bool stream_is_tty(FILE* stream) {
#if defined(_WIN32)
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

// Provisioned tokens (design D8: secrets stay outside the repo). A missing
// store leaves the console unable to authenticate anyone - the session layer
// refuses those dials - instead of silently inventing a token.
std::map<std::string, std::string> load_token_store(const std::string& path) {
    namespace fs = std::filesystem;
    if (!fs::exists(path)) {
        return {};
    }
    std::ifstream in(path);
    if (!in) {
        throw tether::common::ConfigError("token store '" + path + "' cannot be opened");
    }
    const nlohmann::json doc = nlohmann::json::parse(in);
    std::map<std::string, std::string> tokens;
    if (!doc.is_object()) {
        throw tether::common::ConfigError(
            "token store '" + path + "' must be a JSON object of agent names to tokens");
    }
    for (auto entry = doc.begin(); entry != doc.end(); ++entry) {
        if (!entry.value().is_string()) {
            throw tether::common::ConfigError("token store '" + path +
                                              "' entry '" + entry.key() + "' is not a string");
        }
        tokens[entry.key()] = entry.value().get<std::string>();
    }
    return tokens;
}

// Console config with the no-file fallback: a missing file behaves as the
// default-constructed config (ephemeral port, documented loader defaults).
tether::common::ConsoleConfig loaded_console_config(const std::string& config_path) {
    if (!std::filesystem::exists(config_path)) {
        return tether::common::ConsoleConfig{};
    }
    return tether::common::load_console_config(config_path);
}

tether::transport::ConsoleTransportOptions console_options(
    const tether::common::ConsoleConfig& config) {
    tether::transport::ConsoleTransportOptions options;
    options.listen_address = config.listen_address.empty() ? std::string("127.0.0.1")
                                                           : config.listen_address;
    options.port = config.port > 0 ? static_cast<std::uint16_t>(config.port) : 0;
    options.session = tether::protocol::SessionConfig{}; // protocol defaults
    options.resolve_token = [tokens = load_token_store(config.token_store_path)](
                                const std::string& identity) {
        const auto it = tokens.find(identity);
        return it == tokens.end() ? std::string{} : it->second;
    };
    return options;
}

} // namespace

int main(int argc, char* argv[]) {
    using tether::common::Logger;
    namespace cli = tether::cli;
    namespace transport = tether::transport;

    Logger& log = Logger::instance();
    log.info("console starting", {{"version", TETHER_VERSION}});

#if defined(_WIN32)
    // Byte-exact redirects: file chunks (raw bytes) must not pass through
    // text-mode CR/LF translation on the way to a redirected file or pipe.
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    cli::CliStreams streams{
        std::cout,
        std::cerr,
        std::cin,
        stream_is_tty(stdout),
        stream_is_tty(stdin),
    };

    const cli::ConsoleFactory make_console = [&log](const std::string& config_path)
        -> std::unique_ptr<cli::IOperConsole> {
        const tether::common::ConsoleConfig config = loaded_console_config(config_path);
        transport::ConsoleTransportOptions options = console_options(config);
        auto owned = std::make_unique<transport::TcpConsoleTransport>(options);
        owned->start(); // bind + listen + begin accepting (design D1: agents dial in)
        auto console = std::make_unique<cli::TcpOperConsole>(std::move(owned));
        // Bounded dial-in wait (see the runtime-shape note at the top): the
        // agent's bounded dial-out loop must be able to land inside this
        // invocation. The config field `agent_dial_wait_ms` tunes it.
        console->set_agent_dial_budget_ms(config.agent_dial_wait_ms);
        return console;
    };

    // Task 6.9 (additive console config `audit_path`): the operator turns the
    // audit trail on with a storage path; without one auditing stays off
    // (a null store is the CLI's disabled mode). The store re-opens existing
    // history itself, so accumulated records stay queryable per invocation.
    const cli::AuditStoreFactory make_audit = [&log](const std::string& config_path)
        -> std::shared_ptr<cli::AuditStore> {
        if (!std::filesystem::exists(config_path)) {
            return nullptr; // no config file: the disabled default
        }
        const auto config = tether::common::load_console_config(config_path);
        if (!config.audit_path.has_value() || config.audit_path->empty()) {
            return nullptr; // absent/empty audit_path is the off switch
        }
        log.info("audit trail enabled", {{"path", *config.audit_path}});
        return std::make_shared<cli::AuditStore>(*config.audit_path);
    };

    return cli::run_cli(argc, argv, streams, make_console, make_audit);
}
