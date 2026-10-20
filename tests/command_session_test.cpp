// tests/command_session_test.cpp - the agent-side wiring test for tasks
// 6.1-6.3: the capability-matrix dispatcher rides a REAL session pair over
// in-memory pipes (no sockets, injected clocks), with the file family on
// REAL temporary disk and the process family recording-faked. Scenarios map
// to the remote-commands delta:
//  - "unknown command is refused harmlessly": an unknown verb over the wire
//    returns the structured error naming the verb, no family call happens,
//    and the same session serves a real command right after;
//  - "out-of-matrix command returns a structured refusal and the session
//    continues": an unimplemented family reports the structured unsupported
//    result and the session keeps serving;
//  - "missing confirmation blocks a destructive command" at the agent
//    LAYER: an unconfirmed delete over the wire refuses with the disk
//    meanwhile untouched, and the confirmed twin proceeds;
//  - results stream as their structured shapes: listings become result text;
//    argv travels byte-for-byte to the capability layer.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "command_handler.hpp"
#include "matrix.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace tp = tether::protocol;
namespace tsys = tether::syscalls;
namespace fs = std::filesystem;

namespace {

tp::SessionConfig test_config() { return tp::SessionConfig{100, 200, 100, 300}; }

tp::TokenResolver resolver_for(const std::string& token) {
    return [token](const std::string&) { return token; };
}

// A fully handshaken console+agent pair with the dispatcher installed on the
// agent side (the same install path the service main uses).
struct Harness {
    tp::InMemoryConnection wires;
    std::shared_ptr<tp::ConsoleSession> console;
    std::unique_ptr<tp::AgentSession> agent;

    static std::unique_ptr<Harness> connect() {
        auto harness = std::make_unique<Harness>();
        harness->console = std::make_shared<tp::ConsoleSession>(
            harness->wires.to_console, harness->wires.to_agent, test_config(),
            resolver_for("tok"));
        harness->agent = std::make_unique<tp::AgentSession>(
            harness->wires.to_agent, harness->wires.to_console, tp::AgentIdentity{"agent-1", "h1"},
            "tok", test_config());
        harness->agent->begin(0);
        harness->console->pump(1);
        harness->agent->pump(2);
        harness->console->pump(3);
        REQUIRE(harness->console->state() == tp::SessionState::Established);
        return harness;
    }

    // Console issues the command; the agent's dispatch + reply happen inside
    // one pump; the console picks the correlated result in the next pump.
    tp::CommandView issue(const tp::Cmd& command) {
        const std::uint32_t seq = console->start(command, 10);
        agent->pump(11);
        console->pump(12);
        const auto view = console->command(seq);
        REQUIRE(view.has_value());
        REQUIRE(view->status != tp::CommandStatus::Pending); // sync commands end within one pump
        return *view;
    }

    std::string text_of(const tp::CommandView& view) const {
        std::string text;
        for (const tp::Message& piece : view.pieces) {
            if (const auto* result_text = std::get_if<tp::ResultText>(&piece)) {
                text += result_text->text;
            }
        }
        return text;
    }
};

// Recording fake process layer (faked at the session level: real-family
// behavior is proven in process_family_test).
struct FakeProcessControl final : tsys::IProcessControl {
    int list_calls = 0;
    int start_calls = 0;
    int stop_calls = 0;
    std::vector<std::vector<std::string>> start_log;

    std::vector<tsys::ProcessInfo> list() override {
        ++list_calls;
        return {tsys::ProcessInfo{111, "name-one"}, tsys::ProcessInfo{222, "name-two"}};
    }
    std::uint32_t start(const std::vector<std::string>& argv) override {
        ++start_calls;
        start_log.push_back(argv);
        return 4242;
    }
    void stop(std::uint32_t) override { ++stop_calls; }
};

std::atomic<int> g_dir_counter{0};

struct DiskFixture {
    DiskFixture() : root(fs::temp_directory_path() /
                         ("tether-command-tests-" + std::to_string(g_dir_counter++))) {
        std::error_code ec;
        fs::create_directories(root, ec);
        REQUIRE_MESSAGE(!ec, "could not create the test directory");
    }
    ~DiskFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path root;

    void write(const fs::path& path, const std::string& content) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        out << content;
    }
    bool exists(const fs::path& path) const {
        std::error_code ec;
        return fs::exists(path, ec) && !ec;
    }
};

} // namespace

TEST_CASE("an unknown command over the wire is refused harmlessly") {
    std::shared_ptr<FakeProcessControl> process = std::make_shared<FakeProcessControl>();
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(process);
    dispatcher.set_file_system(tsys::make_file_system());

    const auto harness = Harness::connect();
    tether::agent::install_commands(dispatcher, *harness->agent);

    const tp::CommandView refused =
        harness->issue(tp::Cmd{"shellexec", {"powershell", "-enc", "abc"}});
    REQUIRE(refused.status == tp::CommandStatus::Failed);
    CHECK(refused.error_code == "unknown_command");

    // The console tracker ends ERR commands with its own synthesized marker
    // piece ("command ended: <code>", frozen session-layer behavior pinned by
    // session_test); the agent's verb-naming refusal rides the wire ERR
    // payload, asserted at the executor level in the next case.
    REQUIRE(refused.pieces.size() == 1);
    const auto* err = std::get_if<tp::Err>(&refused.pieces.back());
    REQUIRE(err != nullptr);
    CHECK(err->code == "unknown_command");
    CHECK(err->message == "command ended: unknown_command");

    // No system effect: the dispatcher's instrumented counter stays at zero.
    CHECK(process->list_calls == 0);
    CHECK(process->start_calls == 0);

    // The session remains usable, and the agent did not wedge or close.
    CHECK(harness->agent->state() == tp::SessionState::Established);
    const tp::CommandView after = harness->issue(tp::Cmd{"list", {}});
    CHECK(after.status == tp::CommandStatus::Completed);
    CHECK(process->list_calls == 1);
}

TEST_CASE("the agent composes the structured refusal naming the verb on the wire") {
    // Executor-level view of the same refusal (spec: "the initiator receives
    // a structured error identifying the invalid verb"): what dispatch()
    // produces for an unknown verb is exactly one ERR piece whose message
    // carries the verb - the agent puts that on the wire verbatim.
    tsys::CommandDispatcher dispatcher;
    const std::vector<tp::Message> refused =
        tether::agent::outcome_to_pieces(dispatcher.dispatch("shellexec", {}));
    REQUIRE(refused.size() == 1);
    const auto* err = std::get_if<tp::Err>(&refused.front());
    REQUIRE(err != nullptr);
    CHECK(err->code == "unknown_command");
    CHECK(err->message.find("shellexec") != std::string::npos);

    // The unsupported and missing-confirmation refusals shape the same way.
    const std::vector<tp::Message> unsupported =
        tether::agent::outcome_to_pieces(dispatcher.dispatch("screenshot", {}));
    REQUIRE(unsupported.size() == 1);
    const auto* gap = std::get_if<tp::Err>(&unsupported.front());
    REQUIRE(gap != nullptr);
    CHECK(gap->code == "unsupported_platform");
    CHECK(gap->message.find("screenshot") != std::string::npos);
    CHECK(gap->message.find("capture") != std::string::npos);
}

TEST_CASE("an unsupported command is a structured refusal and the session keeps serving") {
    auto process = std::make_shared<FakeProcessControl>();
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(process); // process live; capture/power are not
    dispatcher.set_file_system(tsys::make_file_system());

    const auto harness = Harness::connect();
    tether::agent::install_commands(dispatcher, *harness->agent);

    const tp::CommandView refused = harness->issue(tp::Cmd{"screenshot", {}});
    REQUIRE(refused.status == tp::CommandStatus::Failed);
    CHECK(refused.error_code == "unsupported_platform");

    const tp::CommandView minus_confirmation = harness->issue(tp::Cmd{"delete", {"x"}});
    CHECK(minus_confirmation.status == tp::CommandStatus::Failed);
    CHECK(minus_confirmation.error_code == "missing_confirmation");

    // A later, evaluated command rides the same session untouched.
    const tp::CommandView after = harness->issue(tp::Cmd{"list", {}});
    CHECK(after.status == tp::CommandStatus::Completed);
    CHECK(harness->text_of(after) == "111 name-one\n222 name-two\n");
}

TEST_CASE("destructive file commands confirm at the agent layer over the wire") {
    DiskFixture disk;
    const fs::path guarded = disk.root / "guarded.txt";
    disk.write(guarded, "payload");

    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(tsys::make_process_control());
    dispatcher.set_file_system(tsys::make_file_system());

    const auto harness = Harness::connect();
    tether::agent::install_commands(dispatcher, *harness->agent);

    // Unconfirmed: structured refusal at the agent; the file lives on.
    const tp::CommandView refused = harness->issue(tp::Cmd{"delete", {guarded.string()}});
    REQUIRE(refused.status == tp::CommandStatus::Failed);
    CHECK(refused.error_code == "missing_confirmation");
    CHECK(disk.exists(guarded));

    // Confirmed (token in args): the exact same session proceeds, the file
    // is gone; NOTHING was written or crashed in between.
    const tp::CommandView confirmed =
        harness->issue(tp::Cmd{"delete", {tsys::kConfirmMarker, guarded.string()}});
    CHECK(confirmed.status == tp::CommandStatus::Completed);
    CHECK(harness->text_of(confirmed).find("deleted") == 0);
    CHECK_FALSE(disk.exists(guarded));
}

TEST_CASE("start argv arrives at the capability layer byte-for-byte over the wire") {
    std::shared_ptr<FakeProcessControl> process = std::make_shared<FakeProcessControl>();
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(process);
    dispatcher.set_file_system(tsys::make_file_system());

    const auto harness = Harness::connect();
    tether::agent::install_commands(dispatcher, *harness->agent);

    const std::vector<std::string> argv{"prog;a|x&y>z", "with spaces", "q\"uote"};
    const tp::CommandView view = harness->issue(tp::Cmd{"start", argv});
    REQUIRE(view.status == tp::CommandStatus::Completed);
    CHECK(harness->text_of(view) == "started pid 4242\n");

    REQUIRE(process->start_log.size() == 1);
    CHECK(process->start_log[0] == argv); // the metacharacters are data end to end
}
