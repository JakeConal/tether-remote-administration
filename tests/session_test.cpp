// tests/session_test.cpp - task 3.4 + secure-session delta verification.
// Both peers run over in-memory pipes; time is injected in milliseconds (no
// sleeps - the whole suite runs in well under a second). Scenarios map to the
// delta's WHEN/THENs:
//  - authenticated dial-out with a valid provisioned token
//  - wrong token / unknown identity / empty token / delayed handshake refusals
//  - concurrent commands keep results attributed to their own seq
//  - a result arriving after later traffic still matches by seq
//  - timeout marks one command failed without poisoning the session
//  - cancel marks one command cancelled, session stays usable
//  - keepalive marks a peer dead after the configured window (both sides)
//  - identity registration: distinct identities coexist, and a duplicate
//    identity makes the older session stale and ineligible
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "session.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tp = tether::protocol;
using SessionState = tp::SessionState;
using CommandStatus = tp::CommandStatus;

namespace {

// Test-only deadlines (milliseconds): commands 100, auth 200, ping 100,
// dead-peer window 300 - all injected, so no wall waiting anywhere.
tp::SessionConfig test_config() { return tp::SessionConfig{100, 200, 100, 300}; }

tp::TokenResolver resolver_for(const std::map<std::string, std::string>& tokens) {
    return [tokens](const std::string& identity) {
        const auto it = tokens.find(identity);
        return it == tokens.end() ? std::string{} : it->second;
    };
}

// A fully handshaken console+agent harness. The sessions bind references to
// their own wires, so a harness must never move: it lives behind a unique_ptr.
struct Harness {
    tp::InMemoryConnection wires; // to_console: agent writes; to_agent: console writes
    std::shared_ptr<tp::ConsoleSession> console;
    std::unique_ptr<tp::AgentSession> agent;

    Harness() = default;
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    static std::unique_ptr<Harness> connect(const std::string& agent_name, const std::string& token,
                                            const std::map<std::string, std::string>& tokens) {
        // Sanity: the handshake helper itself uses doctest assertions here,
        // so failures report inside the calling test.
        auto harness = std::make_unique<Harness>();
        const auto established_after_handshake = [&harness] {
            REQUIRE(harness->console != nullptr);
            REQUIRE(harness->agent != nullptr);
            REQUIRE(harness->console->state() == SessionState::Established);
            REQUIRE(harness->agent->state() == SessionState::Established);
        };
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
        established_after_handshake();
        return harness;
    }
};

} // namespace

TEST_CASE("dial-out handshake authenticates a valid agent") {
    tp::InMemoryConnection wires;
    tp::ConsoleSession console{wires.to_console, wires.to_agent, test_config(),
                               resolver_for({{"agent-1", "tok-1"}})};
    tp::AgentSession agent{wires.to_agent, wires.to_console, {"agent-1", "host-1"}, "tok-1",
                           test_config()};
    REQUIRE(console.state() == SessionState::Connecting);
    REQUIRE(agent.state() == SessionState::Connecting);

    agent.begin(0);    // step 1: HELLO
    console.pump(1);   // step 2: CHALLENGE issued
    CHECK(console.identity() == "agent-1");
    agent.pump(2);     // step 3: HMAC answer
    console.pump(3);   // step 4: verified -> AUTH_OK, console Established
    CHECK(console.state() == SessionState::Established);
    agent.pump(4);     // AUTH_OK consumed -> agent Established
    CHECK(agent.state() == SessionState::Established);
}

TEST_CASE("a wrong token is refused and commands are impossible") {
    tp::InMemoryConnection wires;
    tp::ConsoleSession console{wires.to_console, wires.to_agent, test_config(),
                               resolver_for({{"agent-1", "tok-1"}})};
    tp::AgentSession agent{wires.to_agent, wires.to_console, {"agent-1", "host-1"}, "bad-token",
                           test_config()};

    agent.begin(0);
    console.pump(1);
    agent.pump(2);
    console.pump(3); // verification fails -> ERR + Down on the console side
    CHECK(console.state() == SessionState::Down);
    agent.pump(4); // the refusal ERR reaches the agent
    CHECK(agent.state() == SessionState::Down);

    // Spec: no command is ever processed over a refused session.
    CHECK_THROWS_AS(console.start(tp::Cmd{"exec", {"ls"}}, 5), tp::SessionError);
}

TEST_CASE("an unknown identity (no provisioned token) is refused") {
    tp::InMemoryConnection wires;
    tp::ConsoleSession console{wires.to_console, wires.to_agent, test_config(),
                               resolver_for({{"someone-else", "tok-1"}})};
    tp::AgentSession agent{wires.to_agent, wires.to_console, {"agent-1", "host-1"}, "tok-1",
                           test_config()};

    agent.begin(0);
    console.pump(1);
    agent.pump(2);
    CHECK(console.state() == SessionState::Down);
    CHECK(agent.state() == SessionState::Down);
}

TEST_CASE("an empty provisioned token refuses to dial (config note honored)") {
    tp::InMemoryConnection wires;
    tp::AgentSession agent{wires.to_agent, wires.to_console, {"agent-1", "host-1"}, "",
                           test_config()};
    CHECK_THROWS_AS(agent.begin(0), tp::SessionError);
    CHECK(agent.state() == SessionState::Connecting); // refusal at begin(), before any HELLO
}

TEST_CASE("a delayed handshake is refused after auth_timeout") {
    tp::InMemoryConnection wires;
    tp::ConsoleSession console{wires.to_console, wires.to_agent, test_config(),
                               resolver_for({{"agent-1", "tok-1"}})};
    console.pump(0);
    CHECK(console.state() == SessionState::Connecting);
    console.pump(199);
    CHECK(console.state() == SessionState::Connecting); // nothing answered yet
    console.pump(200); // auth_timeout elapsed since open -> refused
    CHECK(console.state() == SessionState::Down);
}

TEST_CASE("wrong-protocol HELLO is refused") {
    tp::InMemoryConnection wires;
    tp::ConsoleSession console{wires.to_console, wires.to_agent, test_config(),
                               resolver_for({{"agent-1", "tok-1"}})}; 
    // A craftful peer sending HELLO with a foreign version byte.
    const auto bad_hello = tp::encode_message(tp::Hello{1u, "agent-1", "host-1"}, 0);
    wires.to_console.write(bad_hello.data(), bad_hello.size());
    console.pump(0);
    CHECK(console.state() == SessionState::Down);
}

TEST_CASE("concurrent commands keep results attributed to their own seq") {
    auto pair = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});

    // The agent answers command 2 instantly and defers command 1, so the
    // arrival order at the console is the reverse of the issue order.
    pair->agent->set_command_handler(
        [&](std::uint32_t seq, const tp::Cmd&, const std::function<bool()>&) {
            if (seq == 2) {
                pair->agent->send_result(seq, tp::ResultText{"B-result"});
            }
        });

    const std::uint32_t seq_a = pair->console->start(tp::Cmd{"cmd-a", {}}, 10);
    const std::uint32_t seq_b = pair->console->start(tp::Cmd{"cmd-b", {}}, 11);
    CHECK(seq_a != seq_b);

    pair->agent->pump(12); // runs the deferred + instant handlers in seq order
    pair->console->pump(13);
    auto view_b = pair->console->command(seq_b);
    REQUIRE(view_b.has_value());
    CHECK(view_b->status == CommandStatus::Completed);
    REQUIRE(view_b->pieces.size() == 1);
    CHECK(std::get<tp::ResultText>(view_b->pieces[0]).text == "B-result");

    auto view_a = pair->console->command(seq_a);
    REQUIRE(view_a.has_value());
    CHECK(view_a->status == CommandStatus::Pending);
    CHECK(pair->console->outstanding_seqs().size() == 1);

    // "Result arrives after later traffic": command 1's result shows up while
    // command 3 is already in flight - matched by seq, not by arrival order.
    const std::uint32_t seq_c = pair->console->start(tp::Cmd{"cmd-c", {}}, 20);
    pair->agent->send_result(seq_a, tp::ResultText{"A-result"}); // delivered out of band
    pair->agent->pump(21);
    pair->console->pump(22);

    view_a = pair->console->command(seq_a);
    REQUIRE(view_a.has_value());
    CHECK(view_a->status == CommandStatus::Completed);
    REQUIRE(view_a->pieces.size() == 1);
    CHECK(std::get<tp::ResultText>(view_a->pieces[0]).text == "A-result");

    auto view_c = pair->console->command(seq_c);
    REQUIRE(view_c.has_value());
    CHECK(view_c->status == CommandStatus::Pending);
}

TEST_CASE("a timeout marks one command failed without poisoning the session") {
    auto pair = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});

    pair->agent->set_command_handler(
        [](std::uint32_t, const tp::Cmd&, const std::function<bool()>&) {
            // hung-style: never replies
        });
    const std::uint32_t seq = pair->console->start(tp::Cmd{"hung", {}}, 10);
    REQUIRE(pair->console->command(seq).has_value());

    pair->console->pump(109); // 10 + 100 deadline: not yet
    auto view = pair->console->command(seq);
    REQUIRE(view.has_value());
    CHECK(view->status == CommandStatus::Pending);

    pair->console->pump(110); // deadline reached -> exactly this command fails
    view = pair->console->command(seq);
    REQUIRE(view.has_value());
    CHECK(view->status == CommandStatus::Failed);
    CHECK(view->error_code == "timeout");
    REQUIRE(view->pieces.size() == 1);
    CHECK(std::get<tp::Err>(view->pieces[0]).code == "timeout");
    CHECK(pair->console->state() == SessionState::Established); // session unharmed
    CHECK(pair->console->outstanding_seqs().empty());

    // The same session executes the next command normally.
    pair->agent->set_command_handler(
        [&](std::uint32_t s, const tp::Cmd&, const std::function<bool()>&) {
            pair->agent->send_result(s, tp::ResultText{"fine"});
        });
    const std::uint32_t next = pair->console->start(tp::Cmd{"next", {}}, 115);
    pair->agent->pump(116);
    pair->console->pump(117);
    view = pair->console->command(next);
    REQUIRE(view.has_value());
    CHECK(view->status == CommandStatus::Completed);
    CHECK(pair->console->state() == SessionState::Established);
}

TEST_CASE("a cancelled command reports Cancelled and the session stays usable") {
    auto pair = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});

    pair->agent->set_command_handler(
        [](std::uint32_t, const tp::Cmd&, const std::function<bool()>&) {
            // long-running handler; is_cancelled polls come in from CANCEL
        });
    const std::uint32_t seq = pair->console->start(tp::Cmd{"long", {}}, 10);
    REQUIRE(pair->console->cancel(seq));
    pair->console->pump(11); // the CANCEL frame goes out
    pair->agent->pump(12);   // agent marks the attempt cancelled, acks
    pair->console->pump(13); // ack routed by seq -> Cancelled
    auto view = pair->console->command(seq);
    REQUIRE(view.has_value());
    CHECK(view->status == CommandStatus::Cancelled);
    CHECK(view->error_code == "cancelled");

    // The cancellation did not wedge the session; the next command succeeds.
    pair->agent->set_command_handler(
        [&](std::uint32_t s, const tp::Cmd&, const std::function<bool()>&) {
            pair->agent->send_result(s, tp::ResultText{"still-alive"});
        });
    const std::uint32_t next = pair->console->start(tp::Cmd{"next", {}}, 20);
    pair->agent->pump(21);
    pair->console->pump(22);
    view = pair->console->command(next);
    REQUIRE(view.has_value());
    CHECK(view->status == CommandStatus::Completed);
    CHECK(pair->console->state() == SessionState::Established);

    // Cancelling a nonexistent/finished command does nothing harmful.
    CHECK_FALSE(pair->console->cancel(next));
}

TEST_CASE("the agent's handler observes cancellation through is_cancelled") {
    auto pair = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});

    // The handler keeps its is_cancelled closure (P5's real handlers poll it
    // from their work loop; here the test polls it like dispatch would).
    std::function<bool()> captured;
    pair->agent->set_command_handler(
        [&](std::uint32_t, const tp::Cmd&, const std::function<bool()>& is_cancelled) {
            captured = is_cancelled;
        });
    const std::uint32_t seq = pair->console->start(tp::Cmd{"watched", {}}, 10);
    pair->agent->pump(11);
    REQUIRE(captured != nullptr);
    CHECK_FALSE(captured()); // still busy when the handler started

    REQUIRE(pair->console->cancel(seq));
    pair->console->pump(12);
    pair->agent->pump(13); // the CANCEL lands (agent acks; flag object shared)
    pair->console->pump(14); // the ack routes by seq -> Cancelled
    CHECK(captured());    // the very closure the handler is holding flips true

    auto view = pair->console->command(seq);
    REQUIRE(view.has_value());
    CHECK(view->status == CommandStatus::Cancelled);
}

TEST_CASE("an agent fading without a disconnect is detected after the keepalive window") {
    auto pair = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});

    // The agent process disappears: only the console pumps from here on.
    pair->console->pump(103); // interval (established at t=3) -> PING queued
    pair->console->pump(203); // second PING
    pair->console->pump(300);
    CHECK(pair->console->state() == SessionState::Established); // window not lapsed yet
    pair->console->pump(302);
    CHECK(pair->console->state() == SessionState::Established); // 299 < window
    pair->console->pump(303); // 300 elapsed with zero life signs -> Down
    CHECK(pair->console->state() == SessionState::Down);
    CHECK_THROWS_AS(pair->console->start(tp::Cmd{"x", {}}, 310), tp::SessionError);

    // Registry view of the same session would now see Down (no longer live);
    // the keepalive-based detection is exactly what makes "not eligible" hold.
}

TEST_CASE("a console fading is detected by the agent after the window too") {
    auto pair = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});

    pair->agent->pump(104);
    CHECK(pair->agent->state() == SessionState::Established);
    pair->agent->pump(204);
    pair->agent->pump(303);
    CHECK(pair->agent->state() == SessionState::Established);
    pair->agent->pump(304);
    CHECK(pair->agent->state() == SessionState::Down);
}

TEST_CASE("flowing keepalives keep both peers alive far beyond the window") {
    auto pair = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});

    for (tp::SessionMillis now = 100; now <= 3000; now += 100) {
        pair->console->pump(now);
        pair->agent->pump(now - 50); // slight phase offset, alternating replies
    }
    CHECK(pair->console->state() == SessionState::Established);
    CHECK(pair->agent->state() == SessionState::Established);
}

TEST_CASE("identity registration: distinct identities coexist independently") {
    tp::AgentRegistry registry;
    auto a = Harness::connect("agent-a", "tok-a", {{"agent-a", "tok-a"}, {"agent-b", "tok-b"}});
    auto b = Harness::connect("agent-b", "tok-b", {{"agent-a", "tok-a"}, {"agent-b", "tok-b"}});

    CHECK(registry.lookup("agent-a") == nullptr);
    CHECK_FALSE(registry.register_session("agent-a", a->console));
    CHECK_FALSE(registry.register_session("agent-b", b->console));
    CHECK(registry.size() == 2);

    // Each identity resolves to its own session, and both remain eligible.
    CHECK(registry.lookup("agent-a") == a->console);
    CHECK(registry.lookup("agent-b") == b->console);
    CHECK(a->console->state() == SessionState::Established);
    CHECK(b->console->state() == SessionState::Established);

    // Commands can be issued through both, with separate seq tracking.
    a->agent->set_command_handler(
        [&](std::uint32_t s, const tp::Cmd&, const std::function<bool()>&) {
            a->agent->send_result(s, tp::ResultText{"from-a"});
        });
    b->agent->set_command_handler(
        [&](std::uint32_t s, const tp::Cmd&, const std::function<bool()>&) {
            b->agent->send_result(s, tp::ResultText{"from-b"});
        });
    const std::uint32_t seq_a = a->console->start(tp::Cmd{"who", {}}, 10);
    const std::uint32_t seq_b = b->console->start(tp::Cmd{"who", {}}, 10);
    a->agent->pump(11);
    b->agent->pump(11);
    a->console->pump(12);
    b->console->pump(12);
    CHECK(std::get<tp::ResultText>(a->console->command(seq_a)->pieces[0]).text == "from-a");
    CHECK(std::get<tp::ResultText>(b->console->command(seq_b)->pieces[0]).text == "from-b");
    CHECK(seq_a == seq_b); // per-session allocators (same identity chain is fine)
}

TEST_CASE("duplicate identity flips the older session stale; the newest serves commands") {
    tp::AgentRegistry registry;
    auto old = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});
    auto fresh = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});

    CHECK_FALSE(registry.register_session("agent-1", old->console));
    CHECK(registry.register_session("agent-1", fresh->console)); // supersedes

    CHECK(old->console->state() == SessionState::Stale);
    CHECK(fresh->console->state() == SessionState::Established);
    CHECK(registry.lookup("agent-1") == fresh->console); // commands use the newest

    // The stale session lost command eligibility - until it reconnects (a
    // fresh registration always supersedes it).
    CHECK_THROWS_AS(old->console->start(tp::Cmd{"exec", {}}, 10), tp::SessionError);

    // The newest session still executes commands on the same identity.
    fresh->agent->set_command_handler(
        [&](std::uint32_t s, const tp::Cmd&, const std::function<bool()>&) {
            fresh->agent->send_result(s, tp::ResultText{"served"});
        });
    const std::uint32_t seq = fresh->console->start(tp::Cmd{"who", {}}, 10);
    fresh->agent->pump(11);
    fresh->console->pump(12);
    CHECK(fresh->console->command(seq)->status == CommandStatus::Completed);
}

TEST_CASE("a stale session never expires into Down via timers; keepalive stops") {
    tp::AgentRegistry registry;
    auto old_pair = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});
    auto fresh_pair = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});

    (void)registry.register_session("agent-1", old_pair->console);
    (void)registry.register_session("agent-1", fresh_pair->console);
    old_pair->console->pump(5000); // past any window: frozen in Stale
    CHECK(old_pair->console->state() == SessionState::Stale);
}

TEST_CASE("a late result after timeout cannot resurrect the command") {
    auto pair = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});

    pair->agent->set_command_handler(
        [](std::uint32_t, const tp::Cmd&, const std::function<bool()>&) {
            // silent until the test drives the late reply
        });
    const std::uint32_t seq = pair->console->start(tp::Cmd{"slow", {}}, 10);
    pair->agent->pump(11); // agent tracks the CMD as in-flight, stays silent
    pair->console->pump(110); // timeout
    CHECK(pair->console->command(seq)->status == CommandStatus::Failed);

    pair->agent->send_result(seq, tp::ResultText{"too late"});
    pair->console->pump(120); // late result ignored
    const auto view = pair->console->command(seq);
    REQUIRE(view.has_value());
    CHECK(view->status == CommandStatus::Failed); // not resurrected
    CHECK(view->pieces.size() == 1);              // only the timeout ERR piece
    CHECK(pair->console->state() == SessionState::Established);

    // Tracking is not damaged for later commands either.
    pair->agent->set_command_handler(
        [&](std::uint32_t s, const tp::Cmd&, const std::function<bool()>&) {
            pair->agent->send_result(s, tp::ResultText{"ok"});
        });
    const std::uint32_t next = pair->console->start(tp::Cmd{"next", {}}, 125);
    pair->agent->pump(126);
    pair->console->pump(127);
    CHECK(pair->console->command(next)->status == CommandStatus::Completed);
}

TEST_CASE("result hooks see every streamed piece and the terminal flag") {
    auto pair = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});

    pair->agent->set_command_handler(
        [&](std::uint32_t s, const tp::Cmd&, const std::function<bool()>&) {
            pair->agent->send_result(s, tp::FileMeta{"capture.png", 8});
            pair->agent->send_result(s,
                                    tp::FileChunk{{0, 1, 2, 3, 4, 5, 6, 7}});
            pair->agent->send_result(s, tp::FileEnd{});
        });
    std::vector<std::string> hook_log;
    tp::ResultHook hook = [&](std::uint32_t, const tp::Message& piece, bool terminal) {
        hook_log.push_back(std::string(tp::msg_type_name(tp::message_type_of(piece))) +
                           (terminal ? "(terminal)" : ""));
    };
    const std::uint32_t seq = pair->console->start(tp::Cmd{"shot", {}}, 10, hook);
    pair->agent->pump(11);
    pair->console->pump(12);

    REQUIRE(hook_log.size() == 3);
    CHECK(hook_log[0] == "FILE_META");
    CHECK(hook_log[1] == "FILE_CHUNK");
    CHECK(hook_log[2] == "FILE_END(terminal)");

    const auto view = pair->console->command(seq);
    REQUIRE(view.has_value());
    CHECK(view->status == CommandStatus::Completed);
    REQUIRE(view->pieces.size() == 3);
    CHECK(std::get<tp::FileMeta>(view->pieces[0]).size == 8);
    CHECK(std::get<tp::FileChunk>(view->pieces[1]).bytes.size() == 8);
}

TEST_CASE("an agent without a handler refuses commands with ERR, session stays up") {
    auto pair = Harness::connect("agent-1", "tok-1", {{"agent-1", "tok-1"}});

    const std::uint32_t seq = pair->console->start(tp::Cmd{"whatever", {}}, 10);
    pair->agent->pump(11); // no handler installed -> ERR "no_handler"
    pair->console->pump(12);
    const auto view = pair->console->command(seq);
    REQUIRE(view.has_value());
    CHECK(view->status == CommandStatus::Failed);
    CHECK(view->error_code == "no_handler");
    CHECK(pair->console->state() == SessionState::Established);
}

TEST_CASE("invalid session configurations fail fast") {
    tp::SessionConfig zero{0, 100, 100, 300};
    CHECK_THROWS_AS(zero.validate(), tp::SessionError);
    tp::SessionConfig tiny_window{100, 200, 300, 100}; // window < interval
    CHECK_THROWS_AS(tiny_window.validate(), tp::SessionError);
}
