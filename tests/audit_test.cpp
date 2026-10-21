// tests/audit_test.cpp - task 6.9: console-side audit records. The store's
// JSONL codec, the query API, and the CLI hooks' records are exercised end
// to end with a fake IOperConsole (no sockets): every executed / refused /
// failed command attempt lands a command record, and background-job
// lifecycle events land their own job_event records referencing the
// initiating / finalizing command. Persistence is the operator's configured
// file: appending there, re-opened history staying queryable, and
// malformed-file strictness are all pinned here.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "audit.hpp"
#include "cli.hpp"
#include "config.hpp"
#include "session.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace tcli = tether::cli;
namespace net = tether::protocol;
namespace fs = std::filesystem;

namespace {

// One fake console driver shared by every run_cli call in a sub-test: the
// factory hands each invocation a fresh console over THIS script, so the
// behaviors (pieces fed to the hook, the returned view, unknown-agent
// refusals, thrown dispatch errors) and the observability (which commands
// were really dispatched) live in one place.
struct FakeScript {
    bool has_agent = true;
    bool dispatch_throws = false;
    std::string dispatch_throw_text = "dial died";
    std::vector<net::Message> pieces;
    std::optional<net::CommandView> view;
    std::vector<net::Cmd> dispatched;

    void set_completed(std::uint32_t seq, std::vector<net::Message> fed_pieces) {
        pieces = std::move(fed_pieces);
        net::CommandView completed;
        completed.seq = seq;
        completed.status = net::CommandStatus::Completed;
        completed.pieces = pieces;
        view = completed;
    }

    void set_failed(std::uint32_t seq, const std::string& code) {
        pieces = {net::Err{code, "the failure detail"}};
        net::CommandView failed;
        failed.seq = seq;
        failed.status = net::CommandStatus::Failed;
        failed.error_code = code;
        failed.pieces = pieces;
        view = failed;
    }

    void set_cancelled(std::uint32_t seq) {
        net::CommandView cancelled;
        cancelled.seq = seq;
        cancelled.status = net::CommandStatus::Cancelled;
        cancelled.error_code = "cancelled";
        view = cancelled;
    }
};

class FakeOperConsole final : public tcli::IOperConsole {
public:
    explicit FakeOperConsole(FakeScript& script) : script_(script) {}

    std::vector<net::RegistryEntry> list_agents() override {
        return {net::RegistryEntry{"a-1", net::RegistryState::Live}};
    }

    bool has_agent(const std::string&) override { return script_.has_agent; }

    std::optional<net::CommandView> dispatch(const std::string&, const net::Cmd& command,
                                             const net::ResultHook& hook) override {
        script_.dispatched.push_back(command);
        if (script_.dispatch_throws) {
            throw std::runtime_error(script_.dispatch_throw_text);
        }
        constexpr std::uint32_t kSeq = 7;
        for (const net::Message& piece : script_.pieces) {
            const bool terminal = std::holds_alternative<net::ResultText>(piece) ||
                                  std::holds_alternative<net::FileEnd>(piece) ||
                                  std::holds_alternative<net::Err>(piece);
            hook(kSeq, piece, terminal);
        }
        return script_.view;
    }

    tcli::CancelOutcome cancel(const std::string&, std::uint32_t) override {
        return tcli::CancelOutcome::NotPending;
    }

private:
    FakeScript& script_;
};

// The console factory one sub-test reuses.
tcli::ConsoleFactory factory_over(FakeScript& script) {
    FakeScript* script_addr = &script;
    return [script_addr](const std::string&) -> std::unique_ptr<tcli::IOperConsole> {
        return std::make_unique<FakeOperConsole>(*script_addr);
    };
}

// A memory-only audit store behind the factory shape (the enabled audit).
struct AuditRig {
    std::shared_ptr<tcli::AuditStore> store = std::make_shared<tcli::AuditStore>(std::string{});
    tcli::AuditStoreFactory factory = [this](const std::string&) { return store; };
};

tcli::CliStreams make_streams(std::ostringstream& out, std::ostringstream& err,
                              std::istringstream& in) {
    return tcli::CliStreams{out, err, in, true, false};
}

// One exec verb against the standard fake target.
int run_exec_verb(FakeScript& script, const std::string& command,
                  const std::vector<std::string>& args, std::ostringstream& out,
                  std::ostringstream& err, const tcli::AuditStoreFactory& audit_factory) {
    std::vector<std::string> arguments{"tether", "exec", "a-1", command};
    for (const std::string& argument : args) {
        arguments.push_back(argument);
    }
    std::istringstream in;
    return run_cli(arguments, make_streams(out, err, in), factory_over(script), audit_factory);
}

tcli::AuditRecord find_record(const tcli::AuditStore& store,
                              const std::optional<std::string>& record_type) {
    tcli::AuditQuery query;
    query.record_type = record_type;
    const std::vector<tcli::AuditRecord> records = store.query(query);
    REQUIRE(records.size() == 1);
    return records.front();
}

fs::path fresh_scratch_dir() {
    static int counter = 0;
    const fs::path dir =
        fs::temp_directory_path() / ("tether-audit-tests-" + std::to_string(counter++));
    fs::create_directories(dir);
    return dir;
}

// A fully-formed fixture line (self-consistent; shape is what the loader
// verifies, plus every field round-trips verbatim).
constexpr const char* kJsonlRecord =
    R"({"time":"2026-10-21T10:15:30.123Z","time_epoch_ms":1770000000123,"agent":"a-1",)"
    R"("command":"start_keylogger","parameters":["keys.log"],"seq":9,"outcome":"executed",)"
    R"("record_type":"job_event","event":"started","job":"input","delivered":"",)"
    R"("detail":""})";

} // namespace

TEST_CASE("outcome classification: refusals vs failures") {
    CHECK(tcli::classify_command_outcome("unknown_command") == "refused");
    CHECK(tcli::classify_command_outcome("unsupported_platform") == "refused");
    CHECK(tcli::classify_command_outcome("missing_confirmation") == "refused");
    CHECK(tcli::classify_command_outcome("invalid_args") == "refused");
    CHECK(tcli::classify_command_outcome("already_running") == "refused");
    CHECK(tcli::classify_command_outcome("not_running") == "refused");

    CHECK(tcli::classify_command_outcome("execute_failed") == "failed");
    CHECK(tcli::classify_command_outcome("not_found") == "failed");
    CHECK(tcli::classify_command_outcome("access_denied") == "failed");
    CHECK(tcli::classify_command_outcome("timeout") == "failed");
    CHECK(tcli::classify_command_outcome("cancelled") == "failed");
}

TEST_CASE("wall time renders ISO-8601 UTC with milliseconds") {
    CHECK(tcli::format_audit_time_iso8601(0) == "1970-01-01T00:00:00.000Z");
    CHECK(tcli::format_audit_time_iso8601(1234) == "1970-01-01T00:00:01.234Z");
    CHECK(tcli::format_audit_time_iso8601(59000) == "1970-01-01T00:00:59.000Z");
}

TEST_CASE("the store appends and queries in memory") {
    tcli::AuditStore store; // memory-only

    tcli::AuditRecord executed;
    executed.time = "1970-01-01T00:00:01.000Z";
    executed.time_epoch_ms = 1000;
    executed.agent = "a-1";
    executed.command = "ls";
    executed.parameters = {"/tmp"};
    executed.seq = 3;
    executed.outcome = "executed";
    store.append(executed);

    tcli::AuditRecord started;
    started.time = "1970-01-01T00:00:02.000Z";
    started.time_epoch_ms = 2000;
    started.agent = "a-1";
    started.command = "start_keylogger";
    started.seq = 4;
    started.outcome = "executed";
    started.record_type = "job_event";
    started.event = "started";
    started.job = "input";
    store.append(started);

    tcli::AuditRecord refused;
    refused.time = "1970-01-01T00:00:03.000Z";
    refused.time_epoch_ms = 3000;
    refused.agent = "b-2";
    refused.command = "shutdown";
    refused.seq = 0;
    refused.outcome = "refused";
    refused.detail = "not confirmed";
    store.append(refused);

    CHECK(store.size() == 3);
    CHECK(store.query({}).size() == 3);

    tcli::AuditQuery by_agent;
    by_agent.agent = "a-1";
    CHECK(store.query(by_agent).size() == 2);

    tcli::AuditQuery by_outcome;
    by_outcome.outcome = "refused";
    const std::vector<tcli::AuditRecord> refusals = store.query(by_outcome);
    REQUIRE(refusals.size() == 1);
    CHECK(refusals.front().agent == "b-2");
    CHECK(refusals.front().detail == "not confirmed");

    tcli::AuditQuery by_seq;
    by_seq.seq = 3;
    const std::vector<tcli::AuditRecord> seq_hits = store.query(by_seq);
    REQUIRE(seq_hits.size() == 1);
    CHECK(seq_hits.front().command == "ls");
    CHECK(seq_hits.front().parameters == std::vector<std::string>{"/tmp"});

    tcli::AuditQuery by_type;
    by_type.record_type = std::string("job_event");
    const std::vector<tcli::AuditRecord> events = store.query(by_type);
    REQUIRE(events.size() == 1);
    CHECK(events.front().event == "started");
    CHECK(events.front().job == "input");

    tcli::AuditQuery by_verb;
    by_verb.command = "start_keylogger";
    CHECK(store.query(by_verb).size() == 1);

    tcli::AuditQuery window;
    window.since_ms = 1500;
    window.until_ms = 2999;
    CHECK(store.query(window).size() == 1); // the started event only

    tcli::AuditQuery nada;
    nada.agent = "nobody";
    CHECK(store.query(nada).empty());
}

TEST_CASE("a persisted store re-opens its history and keeps appending") {
    const fs::path dir = fresh_scratch_dir();
    const std::string path = (dir / "audit.jsonl").string();

    tcli::AuditStore first(path);
    tcli::AuditRecord one;
    one.time = "1970-01-01T00:00:01.000Z";
    one.time_epoch_ms = 1000;
    one.agent = "a-1";
    one.command = "ls";
    one.parameters = {"\" quoted path\""}; // data stays data in JSON encoding
    one.seq = 1;
    one.outcome = "executed";
    one.record_type = "command";
    first.append(one);

    tcli::AuditRecord two;
    two.time = "1970-01-01T00:00:02.000Z";
    two.time_epoch_ms = 2000;
    two.agent = "a-1";
    two.command = "stop_keylogger";
    two.seq = 2;
    two.outcome = "executed";
    two.record_type = "job_event";
    two.event = "stopped";
    two.job = "input";
    two.delivered = "keylog.txt";
    first.append(two);

    // Re-open: the operator's history is queryable again, exactly as stored.
    tcli::AuditStore reopened(path);
    CHECK(reopened.size() == 2);
    {
        const std::vector<tcli::AuditRecord> history = reopened.query({});
        REQUIRE(history.size() == 2);
        CHECK(history[0].command == "ls");
        CHECK(history[0].parameters == std::vector<std::string>{"\" quoted path\""});
        CHECK(history[0].time_epoch_ms == 1000);
        CHECK(history[1].event == "stopped");
        CHECK(history[1].delivered == "keylog.txt");
        CHECK(history[1].time == "1970-01-01T00:00:02.000Z");
    }

    // And appends continue after the loaded history.
    tcli::AuditRecord three;
    three.time = "1970-01-01T00:00:03.000Z";
    three.time_epoch_ms = 3000;
    three.agent = "a-1";
    three.command = "list";
    three.seq = 3;
    three.outcome = "executed";
    reopened.append(three);

    std::ifstream raw(path, std::ios::binary);
    REQUIRE(raw);
    unsigned line_count = 0;
    std::string line;
    while (std::getline(raw, line)) {
        if (!line.empty()) {
            ++line_count;
        }
    }
    CHECK(line_count == 3);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("malformed history fails the store loudly") {
    const fs::path dir = fresh_scratch_dir();
    const std::string path = (dir / "broken.jsonl").string();

    {
        std::ofstream out(path, std::ios::binary);
        out << "garbage not json\n";
    }
    CHECK_THROWS_AS([&] { tcli::AuditStore probe(path); }(), tcli::AuditStoreError);

    // A well-shaped JSON line with a wrong field type also refuses: the
    // loader is strict about the record shape.
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"({"time":7,"time_epoch_ms":0,"agent":"a","command":"ls",)"
               R"("parameters":[],"seq":0,"outcome":"executed","record_type":"command",)"
               R"("event":"","job":"","delivered":"","detail":""})" << '\n';
    }
    CHECK_THROWS_AS([&] { tcli::AuditStore probe(path); }(), tcli::AuditStoreError);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("records load strictly and round-trip every field") {
    const fs::path dir = fresh_scratch_dir();
    const std::string path = (dir / "fixture.jsonl").string();
    {
        std::ofstream out(path, std::ios::binary);
        out << kJsonlRecord << '\n';
        out << '\n'; // a blank line between records is legal
    }
    const std::vector<tcli::AuditRecord> records = tcli::load_audit_jsonl(path);
    REQUIRE(records.size() == 1);
    const tcli::AuditRecord& record = records.front();
    CHECK(record.time == "2026-10-21T10:15:30.123Z");
    CHECK(record.time_epoch_ms == 1770000000123);
    CHECK(record.agent == "a-1");
    CHECK(record.command == "start_keylogger");
    CHECK(record.parameters == std::vector<std::string>{"keys.log"});
    CHECK(record.seq == 9);
    CHECK(record.outcome == "executed");
    CHECK(record.record_type == "job_event");
    CHECK(record.event == "started");
    CHECK(record.job == "input");
    CHECK(record.delivered.empty());
    CHECK(record.detail.empty());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("an executed command lands exactly one audit record") {
    FakeScript script;
    script.set_completed(7, {net::ResultText{"alpha\n"}});
    AuditRig rig;
    std::ostringstream out;
    std::ostringstream err;
    const int status = run_exec_verb(script, "ls", {"/tmp"}, out, err, rig.factory);
    CHECK(status == tcli::kExitOk);
    REQUIRE(script.dispatched.size() == 1);
    CHECK(script.dispatched.front().command == "ls");
    CHECK(script.dispatched.front().args == std::vector<std::string>{"/tmp"});

    const tcli::AuditRecord record = find_record(*rig.store, std::string("command"));
    CHECK(record.agent == "a-1");
    CHECK(record.command == "ls");
    CHECK(record.parameters == std::vector<std::string>{"/tmp"});
    CHECK(record.seq == 7);
    CHECK(record.outcome == "executed");
    CHECK(record.detail.empty());
    CHECK(!record.time.empty());
    CHECK(record.time_epoch_ms > 0);
    CHECK(record.record_type == "command");
    // stdout discipline unchanged: the result text, byte-exact.
    CHECK(out.str() == "alpha\n");
}

TEST_CASE("an agent refusal lands a refused record with its detail") {
    FakeScript script;
    script.set_failed(7, "already_running");
    AuditRig rig;
    std::ostringstream out;
    std::ostringstream err;
    const int status =
        run_exec_verb(script, "start_keylogger", {"keys.log"}, out, err, rig.factory);
    CHECK(status == tcli::kExitFailure);
    REQUIRE(script.dispatched.size() == 1); // it really reached the agent

    const tcli::AuditRecord record = find_record(*rig.store, std::string("command"));
    CHECK(record.command == "start_keylogger");
    CHECK(record.parameters == std::vector<std::string>{"keys.log"});
    CHECK(record.seq == 7);
    CHECK(record.outcome == "refused");
    CHECK(record.detail == "already_running: the failure detail");
}

TEST_CASE("a command failure lands a failed record with its detail") {
    FakeScript script;
    script.set_failed(7, "execute_failed");
    AuditRig rig;
    std::ostringstream out;
    std::ostringstream err;
    const int status = run_exec_verb(script, "ls", {}, out, err, rig.factory);
    CHECK(status == tcli::kExitFailure);

    const tcli::AuditRecord record = find_record(*rig.store, std::string("command"));
    CHECK(record.outcome == "failed");
    CHECK(record.detail == "execute_failed: the failure detail");
    CHECK(record.seq == 7);
    CHECK(err.str().find("error: command failed: execute_failed") == 0);
}

TEST_CASE("an unconfirmed destructive command is refused before dispatch") {
    FakeScript script; // no pieces at all - nothing may be sent
    AuditRig rig;
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    const tcli::ConsoleFactory make_console = factory_over(script);
    const int status = run_cli(std::vector<std::string>{"tether", "exec", "a-1", "shutdown"},
                               make_streams(out, err, in), make_console, rig.factory);
    CHECK(status == tcli::kExitRefused);
    CHECK(script.dispatched.empty()); // the nothing-sent guarantee

    const tcli::AuditRecord record = find_record(*rig.store, std::string("command"));
    CHECK(record.command == "shutdown");
    CHECK(record.agent == "a-1");
    CHECK(record.parameters.empty());
    CHECK(record.seq == 0);
    CHECK(record.outcome == "refused");
    CHECK(record.detail.find("not confirmed") != std::string::npos);
}

TEST_CASE("an unknown agent is refused before dispatch") {
    FakeScript script;
    script.has_agent = false;
    AuditRig rig;
    std::ostringstream out;
    std::ostringstream err;
    const int status = run_exec_verb(script, "ls", {}, out, err, rig.factory);
    CHECK(status == tcli::kExitUnknownAgent);
    CHECK(script.dispatched.empty());

    const tcli::AuditRecord record = find_record(*rig.store, std::string("command"));
    CHECK(record.outcome == "refused");
    CHECK(record.seq == 0);
    CHECK(record.detail.find("unknown agent") != std::string::npos);
}

TEST_CASE("a dispatch transport failure lands a failed record with seq 0") {
    FakeScript script;
    script.dispatch_throws = true;
    AuditRig rig;
    std::ostringstream out;
    std::ostringstream err;
    const int status = run_exec_verb(script, "ls", {}, out, err, rig.factory);
    CHECK(status == tcli::kExitFailure);

    const tcli::AuditRecord record = find_record(*rig.store, std::string("command"));
    CHECK(record.outcome == "failed");
    CHECK(record.seq == 0);
    CHECK(record.detail.find("dispatch failed") == 0);
    CHECK(record.detail.find("dial died") != std::string::npos);
}

TEST_CASE("a cancelled command lands a failed record naming the cancel") {
    FakeScript script;
    script.set_cancelled(7);
    AuditRig rig;
    std::ostringstream out;
    std::ostringstream err;
    const int status = run_exec_verb(script, "ls", {}, out, err, rig.factory);
    CHECK(status == tcli::kExitFailure);

    const tcli::AuditRecord record = find_record(*rig.store, std::string("command"));
    CHECK(record.outcome == "failed");
    CHECK(record.detail == "command cancelled");
    CHECK(record.seq == 7);
}

TEST_CASE("a keylogger start records its job event from the RESULT-JSON convention") {
    FakeScript script;
    script.set_completed(
        7, {net::ResultText{
               "keylogger job started\n"
               "{\"tether_job_event\":{\"job\":\"input\",\"event\":\"started\"}}\n"}});
    AuditRig rig;
    std::ostringstream out;
    std::ostringstream err;
    const int status =
        run_exec_verb(script, "start_keylogger", {"keys.log"}, out, err, rig.factory);
    CHECK(status == tcli::kExitOk);

    // TWO records: the command outcome and the job lifecycle event.
    tcli::AuditQuery commands;
    commands.record_type = std::string("command");
    const std::vector<tcli::AuditRecord> command_records = rig.store->query(commands);
    REQUIRE(command_records.size() == 1);
    CHECK(command_records.front().outcome == "executed");

    tcli::AuditQuery events;
    events.record_type = std::string("job_event");
    const std::vector<tcli::AuditRecord> event_records = rig.store->query(events);
    REQUIRE(event_records.size() == 1);
    const tcli::AuditRecord& event = event_records.front();
    CHECK(event.event == "started");
    CHECK(event.job == "input");
    CHECK(event.delivered.empty());
    CHECK(event.seq == 7); // referencing the initiating command's dispatch
    CHECK(event.agent == "a-1");
    CHECK(event.command == "start_keylogger");
    CHECK(event.parameters == std::vector<std::string>{"keys.log"});
}

TEST_CASE("a camera start without RESULT-JSON records its started event by the verb table") {
    FakeScript script;
    script.set_completed(7, {net::ResultText{"camera job started\nfull speed ahead\n"}});
    AuditRig rig;
    std::ostringstream out;
    std::ostringstream err;
    const int status = run_exec_verb(script, "start_webcam", {}, out, err, rig.factory);
    CHECK(status == tcli::kExitOk);

    tcli::AuditQuery events;
    events.record_type = std::string("job_event");
    const std::vector<tcli::AuditRecord> event_records = rig.store->query(events);
    REQUIRE(event_records.size() == 1);
    CHECK(event_records.front().event == "started");
    CHECK(event_records.front().job == "camera");
}

TEST_CASE("a stop command's job event derives from the delivered file stream") {
    FakeScript script;
    script.set_completed(7,
                         {net::FileMeta{"camera_012.png", 2}, net::FileChunk{{0x42, 0x43}},
                          net::FileEnd{}});
    AuditRig rig;
    std::ostringstream out;
    std::ostringstream err;
    const int status = run_exec_verb(script, "stop_webcam", {}, out, err, rig.factory);
    CHECK(status == tcli::kExitOk);

    tcli::AuditQuery events;
    events.record_type = std::string("job_event");
    const std::vector<tcli::AuditRecord> event_records = rig.store->query(events);
    REQUIRE(event_records.size() == 1);
    CHECK(event_records.front().event == "stopped");
    CHECK(event_records.front().job == "camera");
    CHECK(event_records.front().delivered == "camera_012.png");
    CHECK(event_records.front().command == "stop_webcam");
    // No "started" event accompanies a stop: exactly two records exist.
    CHECK(rig.store->size() == 2);
}

TEST_CASE("non-job commands never fabricate job events") {
    FakeScript script;
    script.set_completed(7, {net::ResultText{"plain text\n"}});
    AuditRig rig;
    std::ostringstream out;
    std::ostringstream err;
    const int status = run_exec_verb(script, "ls", {}, out, err, rig.factory);
    CHECK(status == tcli::kExitOk);

    tcli::AuditQuery events;
    events.record_type = std::string("job_event");
    CHECK(rig.store->query(events).empty());
    CHECK(rig.store->size() == 1);
}

TEST_CASE("auditing stays identical to a store-less run when disabled") {
    FakeScript script;
    script.set_completed(7, {net::ResultText{"alpha\n"}});
    std::ostringstream out;
    std::ostringstream err;
    std::istringstream in;

    // The DEFAULT run_cli overload (no audit factory): no records anywhere,
    // zero behavior change - the phase-5 call sites keep working untouched.
    const int status = run_cli(std::vector<std::string>{"tether", "exec", "a-1", "ls"},
                               make_streams(out, err, in), factory_over(script));
    CHECK(status == tcli::kExitOk);
    CHECK(out.str() == "alpha\n");
}

TEST_CASE("an audit factory failure exits before dispatch") {
    FakeScript script;
    script.set_completed(7, {});
    std::ostringstream out;
    std::ostringstream err;
    std::istringstream in;
    tcli::AuditStoreFactory broken_audit = [](const std::string&)
        -> std::shared_ptr<tcli::AuditStore> {
        throw tcli::AuditStoreError("audit file 'bad-path' cannot be created");
    };
    const int status = run_cli(std::vector<std::string>{"tether", "exec", "a-1", "ls"},
                               make_streams(out, err, in), factory_over(script), broken_audit);
    CHECK(status == tcli::kExitFailure);
    CHECK(script.dispatched.empty());
    CHECK(err.str().find("audit setup failed: audit file 'bad-path'") != std::string::npos);
}

TEST_CASE("the CLI tails the operator's persisted audit file") {
    const fs::path dir = fresh_scratch_dir();
    const std::string path = (dir / "cli-audit.jsonl").string();

    FakeScript script;
    script.set_completed(7, {net::ResultText{"alpha\n"}});
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    tcli::AuditStoreFactory persisted = [path](const std::string&) {
        return std::make_shared<tcli::AuditStore>(path);
    };
    const int status = run_cli(std::vector<std::string>{"tether", "exec", "a-1", "ls"},
                               make_streams(out, err, in), factory_over(script), persisted);
    CHECK(status == tcli::kExitOk);

    // The persisted file already carries the record through the CLI factory.
    tcli::AuditStore reopened(path);
    CHECK(reopened.size() == 1);
    CHECK(find_record(reopened, std::string("command")).outcome == "executed");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("config: audit_path is additive and optional") {
    const fs::path dir = fresh_scratch_dir();
    const char* base = R"({"listen_address":"127.0.0.1","port":47474,"token_store_path":"tokens.json")";

    {
        std::ofstream out(dir / "absent.json", std::ios::binary);
        out << base << '}';
    }
    {
        std::ofstream out(dir / "set.json", std::ios::binary);
        out << base << R"(,"audit_path":"audit/foo.jsonl"})";
    }
    {
        std::ofstream out(dir / "null.json", std::ios::binary);
        out << base << R"(,"audit_path":null})";
    }
    {
        std::ofstream out(dir / "empty.json", std::ios::binary);
        out << base << R"(,"audit_path":""})";
    }
    {
        std::ofstream out(dir / "wrong.json", std::ios::binary);
        out << base << R"(,"audit_path":42})";
    }

    using tether::common::load_console_config;
    CHECK_FALSE(load_console_config((dir / "absent.json").string()).audit_path.has_value());
    CHECK(load_console_config((dir / "set.json").string()).audit_path ==
          std::optional<std::string>{"audit/foo.jsonl"});
    CHECK_FALSE(load_console_config((dir / "null.json").string()).audit_path.has_value());
    CHECK(load_console_config((dir / "empty.json").string()).audit_path ==
          std::optional<std::string>{""}); // empty stays explicit: the off value
    CHECK_THROWS_AS(load_console_config((dir / "wrong.json").string()),
                    tether::common::ConfigError);

    std::error_code ec;
    fs::remove_all(dir, ec);
}
