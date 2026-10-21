// console/audit.hpp - console-side audit records (task 6.9; remote-commands
// delta: "For every command the agent executes or refuses, the console SHALL
// retain an audit record containing time, agent identity, command,
// parameters, sequence number, and outcome; the record set SHALL be
// queryable during the console's operation and persists within the
// operator's configured storage.").
//
// Shape:
//  - AuditRecord is the flat record: the six required fields (time, agent,
//    command, parameters, seq, outcome) plus the background-job event
//    fields (`record_type`/`event`/`job`/`delivered`) and a free-text
//    `detail`. Persisted as one JSON object per line (JSONL) in the
//    operator-configured file (console config `audit_path`; the Task 6.9
//    additive config field).
//  - AuditStore keeps the queryable record set for the console process and
//    appends every record to the JSONL path; the store re-opens the file at
//    construction, so previously persisted history is queryable again
//    (fixture access rides the same code path through load_audit_jsonl).
//  - The CLI hooks (console/cli.cpp) build the records: every dispatched
//    command lands one command record (executed / refused / failed), and
//    background-job lifecycle events land their own job_event records
//    referencing the command that initiated (start) / finalized (stop) the
//    job.
//
// Storage-authority rules:
//  - a record append that cannot be written fails loudly (AuditStoreError);
//    the CLI reports and exits non-zero rather than continuing unaudited;
//  - sequence number 0 means the console dispatched nothing (local refusal,
//    lost session, transport failure) - no command was ever assigned a seq.
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace tether::cli {

// One audit record. Command records (record_type "command") use agent,
// command, parameters, seq, outcome and detail; job_event records carry the
// same identity fields plus event/job/delivered.
struct AuditRecord {
    std::string time;          // ISO-8601 UTC with milliseconds (YYYY-MM-DDTHH:MM:SS.mmmZ)
    std::int64_t time_epoch_ms = 0;
    std::string agent;         // the targeted registry identity
    std::string command;       // the command word (e.g. "ls", "start_keylogger")
    std::vector<std::string> parameters; // the command parameters, order preserved
    std::uint32_t seq = 0;     // the dispatch seq; 0 = nothing was dispatched
    std::string outcome;       // "executed" | "refused" | "failed"
    std::string record_type = "command"; // "command" | "job_event"
    // background-job event fields (job_event records):
    std::string event;         // "started" | "stopped"
    std::string job;           // the registry slot the job owns ("camera" | "input")
    std::string delivered;     // stop events: the name of the delivered capture file
    std::string detail;        // human-readable refusal reason or failure detail
};

// The query filter. Every field is optional; set fields must match exactly,
// time bounds are inclusive epoch-millisecond comparisons.
struct AuditQuery {
    std::optional<std::string> agent;
    std::optional<std::string> command;
    std::optional<std::string> outcome;
    std::optional<std::string> record_type;
    std::optional<std::uint32_t> seq;
    std::optional<std::string> event; // job_event records
    std::optional<std::int64_t> since_ms;
    std::optional<std::int64_t> until_ms;
};

// The agent ERR classifications the console marks as refusals (structured
// no-effect outcomes); every other error code is a command failure:
//   unknown_command      outside the closed set
//   unsupported_platform the capability matrix named the gap
//   missing_confirmation destructive command without the token
//   invalid_args         malformed parameter shape
//   already_running / not_running  background-job state refusals
std::string classify_command_outcome(const std::string& error_code);

// ISO-8601 UTC rendering of an epoch-milliseconds instant (with the same
// millisecond precision the record keeps numerically).
std::string format_audit_time_iso8601(std::int64_t epoch_ms);

// The machine-readable job event line the input family embeds in a start
// result's text payload (the frozen RESULT-JSON convention: job events ride
// JSON inside RESULT_TEXT; the wire's message-type set stays untouched).
// Nullopt for any other line shape or malformed JSON.
struct JobEventHint {
    std::string job;   // e.g. "input"
    std::string event; // e.g. "started"
};
std::optional<JobEventHint> parse_job_event_line(const std::string& line);

// Storage problems fail loudly: never silently skip a record.
class AuditStoreError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The process-wide queryable record set + the JSONL persistence file.
class AuditStore {
public:
    // `path` is the operator-configured JSONL file. An empty path disables
    // persistence (records stay queryable in memory only - the test mode).
    // Construction re-opens existing history: records already in the file
    // are loaded into the queryable set, and appends continue after them.
    // Failures (unreadable history, malformed line, uncreatable file) throw
    // AuditStoreError naming the path and, for lines, the line index.
    explicit AuditStore(const std::string& path = {});

    // Adds the record to the queryable set and (when persistence is on)
    // appends one JSON line. Write failures throw AuditStoreError.
    void append(const AuditRecord& record);

    // The queryable records that match `query`, in insertion (time) order.
    std::vector<AuditRecord> query(const AuditQuery& query = {}) const;

    // Total queryable record count.
    std::size_t size() const noexcept { return records_.size(); }

private:
    std::string path_;
    mutable std::mutex mutex_;
    std::vector<AuditRecord> records_;
};

// Fixture access: strictly loads one JSONL audit file (tests and other
// readers; the store's history re-open shares this parser). Throws
// AuditStoreError on any malformed line.
std::vector<AuditRecord> load_audit_jsonl(const std::string& path);

} // namespace tether::cli
