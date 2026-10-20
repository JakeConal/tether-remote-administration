// tests/matrix_test.cpp - task 6.1: the capability matrix dispatch layer
// (design D6) over recording fakes only - no sockets, no real children, no
// real files. Scenarios map to the remote-commands delta verbatim:
//  - closed set: an unknown verb is rejected with a structured error naming
//    the verb and ZERO family calls ("unknown command is refused harmlessly"
//    -> no system effect)
//  - matrix: a known verb outside the platform matrix resolves to one
//    structured unsupported refusal naming platform and family, and the
//    dispatcher keeps serving ("out-of-matrix capture command returns a
//    structured refusal" -> same session continues later commands)
//  - completeness: every representative of the defined set resolves to a
//    definitive outcome - success with the family implementation, or the
//    structured unsupported result where the family has not landed yet
//    ("every defined command resolves to a definitive outcome")
//  - certifications: destructive verbs refuse at this layer without the
//    confirmation token, nothing executed; parameters flow to the family as
//    literal data
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "matrix.hpp"

#include <memory>
#include <string>
#include <vector>

namespace tsys = tether::syscalls;

namespace {

// Recording fakes: call counts ARE the "no system effect" instrument.
struct FakeProcessControl final : tsys::IProcessControl {
    int list_calls = 0;
    int start_calls = 0;
    int stop_calls = 0;
    std::vector<std::vector<std::string>> start_log;
    std::vector<std::string> stop_log;

    std::vector<tsys::ProcessInfo> list() override {
        ++list_calls;
        return {tsys::ProcessInfo{111, "name-one"}, tsys::ProcessInfo{222, "name-two"}};
    }
    std::uint32_t start(const std::vector<std::string>& argv) override {
        ++start_calls;
        start_log.push_back(argv);
        return 4242;
    }
    void stop(std::uint32_t pid) override {
        ++stop_calls;
        stop_log.push_back(std::to_string(pid));
    }
};

struct FakeFileSystem final : tsys::IFileSystem {
    int list_calls = 0;
    int remove_calls = 0;
    int move_calls = 0;
    int copy_calls = 0;
    std::string last_removed;

    std::vector<tsys::FileEntry> list(const std::string&) override {
        ++list_calls;
        return {tsys::FileEntry{"document.txt", 12, false},
                tsys::FileEntry{"folder", 0, true}};
    }
    void remove(const std::string& path) override {
        ++remove_calls;
        last_removed = path;
    }
    void move(const std::string&, const std::string&) override { ++move_calls; }
    void copy(const std::string&, const std::string&) override { ++copy_calls; }
};

struct Fakes {
    std::shared_ptr<FakeProcessControl> process = std::make_shared<FakeProcessControl>();
    std::shared_ptr<FakeFileSystem> file = std::make_shared<FakeFileSystem>();

    void wire_into(tsys::CommandDispatcher& dispatcher) const {
        // The tests NEED the recording fakes, so the wiring casts the typed
        // pointers to the concrete recorders (same objects, same calls).
        dispatcher.set_process_control(process);
        dispatcher.set_file_system(file);
    }

    int total_family_calls() const {
        return process->list_calls + process->start_calls + process->stop_calls +
               file->list_calls + file->remove_calls + file->move_calls + file->copy_calls;
    }
};

// The argument shape each verb of the implemented families dispatches with
// (destructive rows carry the confirmation marker as args[0]).
std::vector<std::string> args_for(const std::string& verb) {
    if (verb == "delete") {
        return {tsys::kConfirmMarker, "a"};
    }
    if (verb == "move" || verb == "copy") {
        return {tsys::kConfirmMarker, "a", "b"};
    }
    if (verb == "start") {
        return {"prog"};
    }
    if (verb == "stop") {
        return {"4242"};
    }
    return {};
}

// Verbs whose families are implemented in this build (all of them on both
// platforms; the matrix itself is also exercised with a crafted row below).
bool is_implemented_family(tsys::Family family) {
    return family == tsys::Family::Process || family == tsys::Family::File;
}

} // namespace

TEST_CASE("an unknown verb is rejected with no system effect at all") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher;
    fakes.wire_into(dispatcher);

    const tsys::Outcome outcome = dispatcher.dispatch("shellexec", {"powershell", "-e", "dir"});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.code == "unknown_command");
    CHECK(outcome.message.find("shellexec") != std::string::npos);

    // The closed-set gate sits before every capability call: the accounts
    // below are the proof that nothing ran (spec: no system effect).
    CHECK(fakes.process->list_calls == 0);
    CHECK(fakes.process->start_calls == 0);
    CHECK(fakes.process->stop_calls == 0);
    CHECK(fakes.file->list_calls == 0);
    CHECK(fakes.file->remove_calls == 0);
    CHECK(fakes.file->move_calls == 0);
    CHECK(fakes.file->copy_calls == 0);

    // The rejection is also the END of the story for that verb: the same
    // dispatcher object serves a real command immediately after.
    const tsys::Outcome after = dispatcher.dispatch("ls", {});
    CHECK(after.ok);
    CHECK(fakes.file->list_calls == 1);
}

TEST_CASE("the empty command word is outside the closed set") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher;
    fakes.wire_into(dispatcher);

    const tsys::Outcome outcome = dispatcher.dispatch("", {});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.code == "unknown_command");
    CHECK(fakes.total_family_calls() == 0);
}

TEST_CASE("every defined command resolves to a definitive outcome") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher;
    fakes.wire_into(dispatcher);

    const std::vector<tsys::CommandSpec>& set = tsys::command_set();
    REQUIRE(set.size() == 19); // the full v2 set, no more, no less
    for (std::size_t i = 0; i < set.size(); ++i) {
        for (std::size_t j = i + 1; j < set.size(); ++j) {
            REQUIRE(set[j].verb != set[i].verb); // uniqueness keeps the set closed
        }
    }

    for (const tsys::CommandSpec& spec : set) {
        CAPTURE(spec.verb);
        const tsys::Outcome outcome = dispatcher.dispatch(spec.verb, args_for(spec.verb));
        // "never an unclassified failure": every one of the 19 verbs ends in
        // a recognized class, success included.
        const bool definitive = outcome.ok || !outcome.code.empty();
        REQUIRE(definitive);
        if (is_implemented_family(spec.family)) {
            // With the family wired, supported verbs execute.
            CHECK(outcome.ok);
            CHECK(outcome.code == "ok");
        } else {
            // Families landing in 6.4-6.8 hold the structured unsupported
            // resolution in the meantime (naming platform + family).
            CHECK_FALSE(outcome.ok);
            CHECK(outcome.code == "unsupported_platform");
            CHECK(outcome.message.find(spec.verb) != std::string::npos);
        }
    }
    // The 8 implemented-family verbs all executed through their interfaces:
    // list, start, stop (by pid - no enumeration needed), ls, delete, move,
    // copy in that loop.
    CHECK(fakes.process->list_calls == 1);
    CHECK(fakes.process->start_calls == 1);
    CHECK(fakes.process->stop_calls == 1);
    CHECK(fakes.file->list_calls == 1);
    CHECK(fakes.file->remove_calls == 1);
    CHECK(fakes.file->move_calls == 1);
    CHECK(fakes.file->copy_calls == 1);
}

TEST_CASE("out-of-matrix verbs return the structured refusal naming platform and family") {
    Fakes fakes;

    // The matrix mechanism test: one row pinned to a platform that does not
    // resolve it, exactly the shape a platform gap takes in real data (the
    // production rows keep all 19 verbs on both OSes; Wayland/partial-OS
    // gaps land as data edits in the family tasks).
    std::vector<tsys::CommandSpec> crafted = tsys::command_set();
    for (tsys::CommandSpec& spec : crafted) {
        if (std::string(spec.verb) == "screenshot") {
            spec.on_linux = false; // e.g. the platform where capture does not exist
        }
    }
    tsys::CommandDispatcher dispatcher(tsys::Platform::Linux, crafted);

    const tsys::Outcome outcome = dispatcher.dispatch("screenshot", {});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.code == "unsupported_platform");
    CHECK(outcome.message.find("linux") != std::string::npos);
    CHECK(outcome.message.find("capture") != std::string::npos);
    CHECK(outcome.message.find("screenshot") != std::string::npos);
    CHECK(fakes.total_family_calls() == 0);

    // Spec: "the same session continues to serve later commands" - the
    // refusal cannot wedge the dispatch layer either.
    fakes.wire_into(dispatcher);
    const tsys::Outcome after = dispatcher.dispatch("ls", {});
    CHECK(after.ok);
    CHECK(after.lines.size() == 2);
}

TEST_CASE("supported-but-unimplemented families hold the structured unsupported result") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher; // real platform, real matrix, no impls wired

    const tsys::Outcome outcome = dispatcher.dispatch("shutdown", {"confirm"});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.code == "unsupported_platform");
    CHECK(outcome.message.find("shutdown") != std::string::npos);
    CHECK(outcome.message.find("power") != std::string::npos);
    CHECK(outcome.message.find(tsys::platform_name(dispatcher.platform())) !=
          std::string::npos);
    // The gate refused before any capability call (no family is wired for
    // power, and the process/file fakes stay entirely untouched).
    CHECK(fakes.total_family_calls() == 0);
}

TEST_CASE("destructive verbs refuse at the agent layer without the confirmation token") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher;
    fakes.wire_into(dispatcher);

    for (const std::vector<std::string>& unconfirmed :
         {std::vector<std::string>{}, std::vector<std::string>{"guarded.txt"},
          std::vector<std::string>{"yes"}}) {
        CAPTURE(unconfirmed.size());
        const tsys::Outcome outcome = dispatcher.dispatch("delete", unconfirmed);
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.code == "missing_confirmation");
        CHECK(outcome.message.find("confirm") != std::string::npos);
        CHECK(fakes.file->remove_calls == 0); // nothing past the gate
    }

    // move and copy carry the same agent-layer gate (task 6.3 binding:
    // destructive = replace destinations).
    CHECK(dispatcher.dispatch("move", {"a", "b"}).code == "missing_confirmation");
    CHECK(dispatcher.dispatch("copy", {"a", "b"}).code == "missing_confirmation");
    CHECK(fakes.file->move_calls == 0);
    CHECK(fakes.file->copy_calls == 0);
    CHECK(dispatcher.dispatch("shutdown", {}).code == "unsupported_platform");
    CHECK(dispatcher.dispatch("restart", {}).code == "unsupported_platform");
}

TEST_CASE("a confirmed destructive verb executes at the family interface") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher;
    fakes.wire_into(dispatcher);

    const tsys::Outcome outcome =
        dispatcher.dispatch("delete", {tsys::kConfirmMarker, "guarded.txt"});
    CHECK(outcome.ok);
    CHECK(fakes.file->remove_calls == 1);
    CHECK(fakes.file->last_removed == "guarded.txt"); // the marker left the argv
}

TEST_CASE("parameters reach the family as literal typed data") {
    Fakes fakes;
    tsys::CommandDispatcher dispatcher;
    fakes.wire_into(dispatcher);

    // The metacharacter set of the parameters requirement, riding argv data:
    const std::vector<std::string> literal_args = {"a;b", "c|d", "e&f", "g>h"};
    const tsys::Outcome outcome = dispatcher.dispatch("start", literal_args);
    CHECK(outcome.ok);
    REQUIRE(fakes.process->start_log.size() == 1);
    CHECK(fakes.process->start_log[0] == literal_args); // argv unchanged, byte for byte

    // File paths are the same contract: the literal name is what the family
    // sees (and looks up - see file_family_test for the real-disk variant).
    dispatcher.dispatch("delete", {tsys::kConfirmMarker, "a;b|c&d>e"});
    CHECK(fakes.file->last_removed == "a;b|c&d>e");
}
