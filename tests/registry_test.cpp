// tests/registry_test.cpp - task 5.1: the agent registry backed by session
// state (live / stale / down), listing through real handshakes on in-memory
// pipes (no sleeps; injected clock). Scenarios map to the operator-console
// registry requirement and the secure-session identity registration delta:
//  - mixed states listed correctly (live / live+stale / down)
//  - re-registration flips a down identity back to live with a NEW session
//  - a superseded session goes stale and drops from the listing when it dies
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "session.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tp = tether::protocol;
using RegistryState = tp::RegistryState;

namespace {

tp::SessionConfig test_config() { return tp::SessionConfig{100, 200, 100, 300}; }

tp::TokenResolver resolver_for(const std::map<std::string, std::string>& tokens) {
    return [tokens](const std::string& identity) {
        const auto it = tokens.find(identity);
        return it == tokens.end() ? std::string{} : it->second;
    };
}

// A handshaken console+agent pair on in-memory wires (the session_test
// harness pattern): the console session is the one that gets registered.
struct Harness {
    tp::InMemoryConnection wires;
    std::shared_ptr<tp::ConsoleSession> console;
    std::unique_ptr<tp::AgentSession> agent;

    Harness() = default;
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    static std::unique_ptr<Harness> connect(const std::string& agent_name,
                                            const std::string& token,
                                            const std::map<std::string, std::string>& tokens) {
        auto harness = std::make_unique<Harness>();
        harness->console = std::make_shared<tp::ConsoleSession>(
            harness->wires.to_console, harness->wires.to_agent, test_config(),
            resolver_for(tokens));
        harness->agent = std::make_unique<tp::AgentSession>(
            harness->wires.to_agent, harness->wires.to_console,
            tp::AgentIdentity{agent_name, "host-" + agent_name}, token, test_config());
        harness->agent->begin(0);
        harness->console->pump(1);
        harness->agent->pump(2);
        harness->console->pump(3);
        harness->agent->pump(4);
        REQUIRE(harness->console != nullptr);
        REQUIRE(harness->console->state() == tp::SessionState::Established);
        return harness;
    }

    // Drive the console side Down by ending the wire (EOF), like a vanished
    // peer would.
    void die() {
        wires.to_console.close_writer();
        console->pump(10);
        REQUIRE(console->state() == tp::SessionState::Down);
    }
};

std::string render(const std::vector<tp::RegistryEntry>& rows) {
    std::string text;
    for (const tp::RegistryEntry& row : rows) {
        text += row.identity;
        text += " ";
        text += tp::registry_state_name(row.state);
        text += "\n";
    }
    return text;
}

} // namespace

TEST_CASE("mixed states are listed correctly: live, live, down") {
    const std::map<std::string, std::string> tokens{{"up-1", "tok-1"},
                                                    {"up-2", "tok-2"},
                                                    {"lost-3", "tok-3"}};
    tp::AgentRegistry registry;
    auto up1 = Harness::connect("up-1", "tok-1", tokens);
    auto up2 = Harness::connect("up-2", "tok-2", tokens);
    auto lost3 = Harness::connect("lost-3", "tok-3", tokens);
    lost3->die(); // down after losing its session (still listed as down)

    CHECK(registry.register_session("up-1", up1->console) == false);
    CHECK(registry.register_session("up-2", up2->console) == false);
    CHECK(registry.register_session("lost-3", lost3->console) == false);

    const std::vector<tp::RegistryEntry> rows = registry.list();
    REQUIRE(rows.size() == 3);
    CHECK(render(rows) == "lost-3 down\nup-1 live\nup-2 live\n"); // sorted by identity

    CHECK(registry.has_agent("lost-3"));
    CHECK(registry.state_of("lost-3") == RegistryState::Down);
    CHECK(registry.state_of("up-1") == RegistryState::Live);
    CHECK(registry.state_of("ghost") == RegistryState::Down); // unknown reads down
    CHECK_FALSE(registry.has_agent("ghost"));
}

TEST_CASE("a down agent flips back to live on re-registration with a NEW session") {
    const std::map<std::string, std::string> tokens{{"agent-1", "tok-1"}};
    tp::AgentRegistry registry;
    auto first = Harness::connect("agent-1", "tok-1", tokens);
    CHECK(registry.register_session("agent-1", first->console) == false);
    first->die();
    CHECK(registry.state_of("agent-1") == RegistryState::Down);
    const std::shared_ptr<tp::ConsoleSession> old_session = registry.lookup("agent-1");
    REQUIRE(old_session != nullptr);

    // Reconnects and re-authenticates: the registry marks the identity live
    // again without any operator intervention, holding the NEW session.
    auto second = Harness::connect("agent-1", "tok-1", tokens);
    CHECK(registry.register_session("agent-1", second->console) == false); // old was down, no supersession
    CHECK(registry.state_of("agent-1") == RegistryState::Live);
    CHECK(registry.lookup("agent-1") == second->console);
    CHECK(registry.lookup("agent-1") != old_session);
}

TEST_CASE("a superseded session goes stale until it disconnects") {
    const std::map<std::string, std::string> tokens{{"agent-1", "tok-1"}};
    tp::AgentRegistry registry;
    auto older = Harness::connect("agent-1", "tok-1", tokens);
    CHECK(registry.register_session("agent-1", older->console) == false);

    // A second connection authenticates with the same identity: the older
    // session is marked stale (the session layer's own state), the newer one
    // serves the identity, and the listing shows both connected sessions.
    auto newer = Harness::connect("agent-1", "tok-1", tokens);
    CHECK(registry.register_session("agent-1", newer->console) == true);
    CHECK(older->console->state() == tp::SessionState::Stale);
    CHECK(registry.lookup("agent-1") == newer->console);
    REQUIRE(registry.state_of("agent-1") == RegistryState::Live);

    const std::vector<tp::RegistryEntry> rows = registry.list();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].identity == "agent-1"); // current session first
    CHECK(rows[0].state == RegistryState::Live);
    CHECK(rows[1].identity == "agent-1"); // then its superseded-but-connected peer
    CHECK(rows[1].state == RegistryState::Stale);

    // Once the superseded session's connection dies, the stale row is gone.
    older->die();
    CHECK(registry.list().size() == 1);
    CHECK(registry.list()[0].state == RegistryState::Live);
}
