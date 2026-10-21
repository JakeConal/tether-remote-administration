// console/audit.cpp - the audit store's implementation (task 6.9): the
// JSONL codec, the queryable in-memory set, the persistence appends, and
// the helpers the CLI classifies outcomes and decodes job events with.
//
// The on-disk form is one JSON object per line with exactly the record's
// fields (the object keys are the struct's fields; job_event records keep
// the same key set, with empty strings for the fields a command record has
// no value for). Loading is strict: a single malformed line makes opening
// the store fail loudly - an audit trail that silently skips records is not
// an audit trail.
#include "audit.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace tether::cli {

namespace {

using nlohmann::json;

// A JSONL line for `record` (all twelve fields, always present).
std::string render_audit_record(const AuditRecord& record) {
    json document;
    document["time"] = record.time;
    document["time_epoch_ms"] = record.time_epoch_ms;
    document["agent"] = record.agent;
    document["command"] = record.command;
    document["parameters"] = record.parameters;
    document["seq"] = record.seq;
    document["outcome"] = record.outcome;
    document["record_type"] = record.record_type;
    document["event"] = record.event;
    document["job"] = record.job;
    document["delivered"] = record.delivered;
    document["detail"] = record.detail;
    return document.dump();
}

// Strict decode of one record object: every field must be present with the
// right type. Unknown keys are tolerated (additive storage: readers older
// than a writer's extra fields keep working). Field-level failures say only
// which field is wrong; the loader prefixes the file and line.
AuditRecord decode_audit_record(const json& value) {
    const auto field_error = [&](const std::string& problem) {
        throw AuditStoreError(problem);
    };
    const auto require_string = [&](const char* field) -> std::string {
        const auto it = value.find(field);
        if (it == value.end() || !it->is_string()) {
            field_error("field '" + std::string(field) + "' must be a string");
        }
        return it->get<std::string>();
    };
    const auto require_integer = [&](const char* field) -> std::int64_t {
        const auto it = value.find(field);
        if (it == value.end() || !it->is_number_integer()) {
            field_error("field '" + std::string(field) + "' must be an integer");
        }
        return it->get<std::int64_t>();
    };

    if (!value.is_object()) {
        field_error("record must be a JSON object");
    }

    AuditRecord record;
    record.time = require_string("time");
    record.time_epoch_ms = require_integer("time_epoch_ms");
    record.agent = require_string("agent");
    record.command = require_string("command");
    const auto parameters = value.find("parameters");
    if (parameters == value.end() || !parameters->is_array()) {
        field_error("field 'parameters' must be an array of strings");
    }
    for (const auto& parameter : *parameters) {
        if (!parameter.is_string()) {
            field_error("every 'parameters' entry must be a string");
        }
        record.parameters.push_back(parameter.get<std::string>());
    }
    record.seq = static_cast<std::uint32_t>(require_integer("seq"));
    record.outcome = require_string("outcome");
    record.record_type = require_string("record_type");
    record.event = require_string("event");
    record.job = require_string("job");
    record.delivered = require_string("delivered");
    record.detail = require_string("detail");
    return record;
}

} // namespace

std::string format_audit_time_iso8601(std::int64_t epoch_ms) {
    const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
    const int millis = static_cast<int>(epoch_ms % 1000);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char buffer[64];
    const std::size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc);
    if (written == 0) {
        return std::to_string(epoch_ms); // never claim a time we did not render
    }
    std::ostringstream out;
    out << buffer << '.' << std::setw(3) << std::setfill('0') << millis << 'Z';
    return out.str();
}

std::string classify_command_outcome(const std::string& error_code) {
    // Structured refusals: nothing ran, the system state is unchanged.
    if (error_code == "unknown_command" || error_code == "unsupported_platform" ||
        error_code == "missing_confirmation" || error_code == "invalid_args" ||
        error_code == "already_running" || error_code == "not_running") {
        return "refused";
    }
    // Everything else (not_found, access_denied, execute_failed, timeout,
    // cancelled, ...) is a command that failed.
    return "failed";
}

std::optional<JobEventHint> parse_job_event_line(const std::string& line) {
    // Exactly one convention: the agent's result text carries the event as a
    // line whose JSON object has the "tether_job_event" root key; any other
    // content, or a malformed event object, is not an event. (The key's
    // value is the nested {job, event} object.)
    try {
        const json document = json::parse(line);
        if (document.type() != json::value_t::object) {
            return std::nullopt;
        }
        const auto event = document.find("tether_job_event");
        if (event == document.end() || !event->is_object()) {
            return std::nullopt;
        }
        const auto job = event->find("job");
        const auto event_name = event->find("event");
        if (job == event->end() || !job->is_string() || event_name == event->end() ||
            !event_name->is_string()) {
            return std::nullopt;
        }
        return JobEventHint{job->get<std::string>(), event_name->get<std::string>()};
    } catch (const json::parse_error&) {
        return std::nullopt;
    }
}

std::vector<AuditRecord> load_audit_jsonl(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw AuditStoreError("audit file '" + path + "' cannot be opened");
    }
    std::vector<AuditRecord> records;
    std::string line;
    std::size_t line_index = 0;
    while (std::getline(in, line)) {
        ++line_index;
        if (line.empty()) {
            continue; // blank lines between records are legal
        }
        try {
            records.push_back(decode_audit_record(json::parse(line)));
        } catch (const json::parse_error& error) {
            throw AuditStoreError("audit file '" + path + "' line " +
                                  std::to_string(line_index) + ": malformed JSON (" +
                                  error.what() + ")");
        } catch (const AuditStoreError& error) {
            throw AuditStoreError("audit file '" + path + "' line " +
                                  std::to_string(line_index) + ": field problem: " +
                                  error.what());
        }
    }
    if (in.bad()) {
        throw AuditStoreError("audit file '" + path + "' could not be read");
    }
    return records;
}

AuditStore::AuditStore(const std::string& path) : path_(path) {
    if (path_.empty()) {
        return; // persistence disabled: memory-only query set
    }
    namespace fs = std::filesystem;
    if (fs::exists(path_)) {
        records_ = load_audit_jsonl(path_); // re-opened history stays queryable
    } else {
        std::ofstream create(path_, std::ios::binary | std::ios::app);
        if (!create) {
            throw AuditStoreError("audit file '" + path_ + "' cannot be created");
        }
    }
}

void AuditStore::append(const AuditRecord& record) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!path_.empty()) {
        std::ofstream out(path_, std::ios::binary | std::ios::app);
        if (!out) {
            throw AuditStoreError("audit file '" + path_ + "' cannot be opened for append");
        }
        out << render_audit_record(record) << '\n';
        out.flush();
        if (!out) {
            throw AuditStoreError("audit file '" + path_ + "' write failed");
        }
    }
    records_.push_back(record);
}

std::vector<AuditRecord> AuditStore::query(const AuditQuery& filter) const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<AuditRecord> matching;
    for (const AuditRecord& record : records_) {
        if (filter.agent && *filter.agent != record.agent) {
            continue;
        }
        if (filter.command && *filter.command != record.command) {
            continue;
        }
        if (filter.outcome && *filter.outcome != record.outcome) {
            continue;
        }
        if (filter.record_type && *filter.record_type != record.record_type) {
            continue;
        }
        if (filter.seq && *filter.seq != record.seq) {
            continue;
        }
        if (filter.event && *filter.event != record.event) {
            continue;
        }
        if (filter.since_ms && record.time_epoch_ms < *filter.since_ms) {
            continue;
        }
        if (filter.until_ms && record.time_epoch_ms > *filter.until_ms) {
            continue;
        }
        matching.push_back(record);
    }
    return matching;
}

} // namespace tether::cli
