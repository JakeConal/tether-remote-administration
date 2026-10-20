// Phase 2 task 2.2: configuration loading tests (design D8).
// Covers valid load, missing fields, malformed JSON, wrong field types, and
// missing files - every failure path is an explicit ConfigError naming the
// file (and the field where known).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "config.hpp"

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>

namespace {

std::filesystem::path config_dir() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "tether-config-tests";
    std::filesystem::create_directories(dir);
    return dir;
}

std::string write_config(std::string_view name, std::string_view contents) {
    const std::filesystem::path path = config_dir() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    return path.string();
}

constexpr std::string_view kValidConsole = R"json({
  "listen_address": "127.0.0.1",
  "port": 47474,
  "token_store_path": "tokens.json"
})json";

constexpr std::string_view kValidAgent = R"json({
  "console_address": "127.0.0.1",
  "agent_name": "agent-lab-1",
  "token": "example-token"
})json";

std::string load_error_message_console(std::string_view path) {
    try {
        tether::common::load_console_config(std::string(path));
    } catch (const tether::common::ConfigError& error) {
        return error.what();
    }
    return std::string();
}

std::string load_error_message_agent(std::string_view path) {
    try {
        tether::common::load_agent_config(std::string(path));
    } catch (const tether::common::ConfigError& error) {
        return error.what();
    }
    return std::string();
}

} // namespace

TEST_CASE("valid console config loads all fields") {
    const std::string path = write_config("valid-console.json", kValidConsole);
    const tether::common::ConsoleConfig config = tether::common::load_console_config(path);
    CHECK(config.listen_address == "127.0.0.1");
    CHECK(config.port == 47474);
    CHECK(config.token_store_path == "tokens.json");
}

TEST_CASE("valid agent config loads all fields") {
    const std::string path = write_config("valid-agent.json", kValidAgent);
    const tether::common::AgentConfig config = tether::common::load_agent_config(path);
    CHECK(config.console_address == "127.0.0.1");
    CHECK(config.agent_name == "agent-lab-1");
    CHECK(config.token == "example-token");
}

TEST_CASE("missing config file fails explicitly naming the file") {
    const std::filesystem::path missing = config_dir() / "does-not-exist.json";
    const std::string message = load_error_message_console(missing.string());
    REQUIRE_FALSE(message.empty());
    CHECK(message.find("does-not-exist.json") != std::string::npos);
    CHECK(message.find("cannot open") != std::string::npos);
}

TEST_CASE("console config rejects missing field, naming file and field") {
    const std::string path = write_config("console-missing-field.json", R"json({
      "listen_address": "127.0.0.1",
      "port": 47474
    })json");
    const std::string message = load_error_message_console(path);
    REQUIRE_FALSE(message.empty());
    CHECK(message.find("console-missing-field.json") != std::string::npos);
    CHECK(message.find("token_store_path") != std::string::npos);
}

TEST_CASE("agent config rejects missing token field, naming file and field") {
    const std::string path = write_config("agent-missing-token.json", R"json({
      "console_address": "127.0.0.1",
      "agent_name": "agent-lab-1"
    })json");
    const std::string message = load_error_message_agent(path);
    REQUIRE_FALSE(message.empty());
    CHECK(message.find("agent-missing-token.json") != std::string::npos);
    CHECK(message.find("'token'") != std::string::npos);
}

TEST_CASE("malformed JSON fails explicitly") {
    const std::string path =
        write_config("malformed-console.json", "{ not json at all ");
    const std::string message = load_error_message_console(path);
    REQUIRE_FALSE(message.empty());
    CHECK(message.find("malformed-console.json") != std::string::npos);
    CHECK(message.find("malformed JSON") != std::string::npos);
}

TEST_CASE("wrong field type for port fails explicitly") {
    const std::string path = write_config("console-port-string.json", R"json({
      "listen_address": "127.0.0.1",
      "port": "not-a-number",
      "token_store_path": "tokens.json"
    })json");
    const std::string message = load_error_message_console(path);
    REQUIRE_FALSE(message.empty());
    CHECK(message.find("console-port-string.json") != std::string::npos);
    CHECK(message.find("'port'") != std::string::npos);
}

TEST_CASE("out-of-range port fails explicitly") {
    const std::string path = write_config("console-port-range.json", R"json({
      "listen_address": "127.0.0.1",
      "port": 70000,
      "token_store_path": "tokens.json"
    })json");
    const std::string message = load_error_message_console(path);
    REQUIRE_FALSE(message.empty());
    CHECK(message.find("'port'") != std::string::npos);
}

TEST_CASE("wrong field type for string field fails explicitly") {
    const std::string path = write_config("agent-name-number.json", R"json({
      "console_address": "127.0.0.1",
      "agent_name": 7,
      "token": "example-token"
    })json");
    const std::string message = load_error_message_agent(path);
    REQUIRE_FALSE(message.empty());
    CHECK(message.find("agent-name-number.json") != std::string::npos);
    CHECK(message.find("'agent_name'") != std::string::npos);
}

TEST_CASE("non-object JSON root fails explicitly") {
    const std::string path = write_config("array-root.json", "[1, 2, 3]");
    const std::string message = load_error_message_console(path);
    REQUIRE_FALSE(message.empty());
    CHECK(message.find("array-root.json") != std::string::npos);
    CHECK(message.find("object") != std::string::npos);
}

TEST_CASE("agent config example shape with empty token loads") {
    // Example templates ship an empty token; structural validation accepts it
    // (the handshake layer rejects it when authentication lands in Phase 3).
    const std::string path = write_config("agent-empty-token.json", R"json({
      "console_address": "127.0.0.1",
      "agent_name": "agent-lab-1",
      "token": ""
    })json");
    const tether::common::AgentConfig config = tether::common::load_agent_config(path);
    CHECK(config.token.empty());
}

// ---- Phase 4 additions (additive): console gmail section + agent dial port ----

TEST_CASE("console config without a gmail section stays TCP-only") {
    const std::string path = write_config("console-no-gmail.json", kValidConsole);
    const tether::common::ConsoleConfig config = tether::common::load_console_config(path);
    CHECK_FALSE(config.gmail.has_value()); // disabled by absence, not by default
}

TEST_CASE("console config with a disabled gmail section loads structured-but-off") {
    const std::string path = write_config("console-gmail-disabled.json", R"json({
      "listen_address": "127.0.0.1",
      "port": 47474,
      "token_store_path": "tokens.json",
      "gmail": {
        "enabled": false
      }
    })json");
    const tether::common::ConsoleConfig config = tether::common::load_console_config(path);
    REQUIRE(config.gmail.has_value());
    CHECK(config.gmail->enabled == false);
}

TEST_CASE("console config with an enabled gmail section loads every field") {
    const std::string path = write_config("console-gmail-enabled.json", R"json({
      "listen_address": "127.0.0.1",
      "port": 47474,
      "token_store_path": "tokens.json",
      "gmail": {
        "enabled": true,
        "credentials_path": "C:/absolute/outside/repo/gmail-credentials.json",
        "poll_interval_seconds": 30,
        "admin_list": ["admin@example.com", "boss@example.com"],
        "default_agent": "agent-lab-1"
      }
    })json");
    const tether::common::ConsoleConfig config = tether::common::load_console_config(path);
    REQUIRE(config.gmail.has_value());
    REQUIRE(config.gmail->enabled);
    CHECK(config.gmail->credentials_path == "C:/absolute/outside/repo/gmail-credentials.json");
    CHECK(config.gmail->poll_interval_seconds == 30);
    CHECK(config.gmail->admin_list.size() == 2);
    CHECK(config.gmail->admin_list[0] == "admin@example.com");
    CHECK(config.gmail->default_agent == "agent-lab-1");
}

TEST_CASE("gmail section field and range problems fail explicitly") {
    {
        const std::string path = write_config("gmail-interval-zero.json", R"json({
          "listen_address": "127.0.0.1",
          "port": 47474,
          "token_store_path": "tokens.json",
          "gmail": {
            "enabled": true,
            "credentials_path": "/tmp/creds.json",
            "poll_interval_seconds": 0,
            "admin_list": ["admin@example.com"],
            "default_agent": "agent-lab-1"
          }
        })json");
        const std::string message = load_error_message_console(path);
        REQUIRE_FALSE(message.empty());
        CHECK(message.find("'gmail.poll_interval_seconds'") != std::string::npos);
    }
    {
        const std::string path = write_config("gmail-admin-empty.json", R"json({
          "listen_address": "127.0.0.1",
          "port": 47474,
          "token_store_path": "tokens.json",
          "gmail": {
            "enabled": true,
            "credentials_path": "/tmp/creds.json",
            "poll_interval_seconds": 30,
            "admin_list": [],
            "default_agent": "agent-lab-1"
          }
        })json");
        const std::string message = load_error_message_console(path);
        REQUIRE_FALSE(message.empty());
        CHECK(message.find("'gmail.admin_list'") != std::string::npos);
    }
    {
        const std::string path = write_config("gmail-missing-creds.json", R"json({
          "listen_address": "127.0.0.1",
          "port": 47474,
          "token_store_path": "tokens.json",
          "gmail": {
            "enabled": true,
            "poll_interval_seconds": 30,
            "admin_list": ["admin@example.com"],
            "default_agent": "agent-lab-1"
          }
        })json");
        const std::string message = load_error_message_console(path);
        REQUIRE_FALSE(message.empty());
        CHECK(message.find("credentials_path") != std::string::npos);
    }
    {
        const std::string path = write_config("gmail-object-broken.json", R"json({
          "listen_address": "127.0.0.1",
          "port": 47474,
          "token_store_path": "tokens.json",
          "gmail": "yes"
        })json");
        const std::string message = load_error_message_console(path);
        REQUIRE_FALSE(message.empty());
        CHECK(message.find("'gmail'") != std::string::npos);
        CHECK(message.find("object") != std::string::npos);
    }
}

TEST_CASE("agent console_port defaults to the documented port when absent") {
    const std::string path = write_config("agent-no-port.json", kValidAgent);
    const tether::common::AgentConfig config = tether::common::load_agent_config(path);
    CHECK(config.console_port == 47474);
}

TEST_CASE("agent console_port loads when present and validates its type and range") {
    {
        const std::string path = write_config("agent-port.json", R"json({
          "console_address": "127.0.0.1",
          "console_port": 50999,
          "agent_name": "agent-lab-1",
          "token": "example-token"
        })json");
        const tether::common::AgentConfig config = tether::common::load_agent_config(path);
        CHECK(config.console_port == 50999);
    }
    {
        const std::string path = write_config("agent-port-string.json", R"json({
          "console_address": "127.0.0.1",
          "console_port": "50999",
          "agent_name": "agent-lab-1",
          "token": "example-token"
        })json");
        const std::string message = load_error_message_agent(path);
        REQUIRE_FALSE(message.empty());
        CHECK(message.find("'console_port'") != std::string::npos);
    }
    {
        const std::string path = write_config("agent-port-range.json", R"json({
          "console_address": "127.0.0.1",
          "console_port": 70000,
          "agent_name": "agent-lab-1",
          "token": "example-token"
        })json");
        const std::string message = load_error_message_agent(path);
        REQUIRE_FALSE(message.empty());
        CHECK(message.find("'console_port'") != std::string::npos);
    }
}

TEST_CASE("agent instance_id is optional and loads when present") {
    {
        const std::string path = write_config("agent-no-instance.json", kValidAgent);
        const tether::common::AgentConfig config = tether::common::load_agent_config(path);
        CHECK(config.instance_id.empty()); // defaults to agent_name at the consumer
    }
    {
        const std::string path = write_config("agent-instance.json", R"json({
          "console_address": "127.0.0.1",
          "agent_name": "agent-lab-1",
          "instance_id": "host-lab-01",
          "token": "example-token"
        })json");
        const tether::common::AgentConfig config = tether::common::load_agent_config(path);
        CHECK(config.instance_id == "host-lab-01");
    }
}
