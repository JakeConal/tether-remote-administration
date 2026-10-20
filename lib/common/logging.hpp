// lib/common/logging.hpp - minimal structured logging (design D8, task 2.3).
//
// Levels + ISO-8601 UTC timestamp + message + key=value fields, identified
// through an injectable sink (tests capture records instead of touching
// stderr; the default sink writes to stderr like the rest of status output).
// This replaces any bare `cerr` usage in new code; session and seq ride as
// regular contextual fields per D8.
#pragma once

#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

namespace tether::common {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

const char* log_level_name(LogLevel level);

// A contextual field, rendered as `name=value` after the message.
struct LogField {
    std::string name;
    std::string value;
};

// Scope and source location: the timestamp is set by the logger when the
// record is emitted, so entries carry the moment they were produced.
struct LogRecord {
    LogLevel level;
    std::string timestamp; // ISO-8601 UTC, e.g. 2026-10-20T12:34:56Z
    std::string message;
    std::vector<LogField> fields;
};

// One line per record:
//     2026-10-20T12:34:56Z [info] command dispatched session=agent-1 seq=7
// Field values render verbatim; structured sinks (real JSON output) may come
// later - keeping the text format for now keeps results diff-able in tests.
std::string format_log_record(const LogRecord& record);

// Destination of log records. Injectable so tests capture output instead of
// writing to the console, and so a phase later can swap in file sinks.
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
};

// Writes formatted lines to a stream (the default sink points at stderr).
class StreamLogSink : public LogSink {
public:
    explicit StreamLogSink(std::ostream& out);
    void write(const LogRecord& record) override;

private:
    std::ostream& out_;
};

// Timestamp format shared by the formatter above: `%Y-%m-%dT%H:%M:%SZ` UTC.
std::string utc_timestamp_now();

// Process-wide logger (default: stderr sink, Info minimum level). Tests
// redirect it via set_sink(); the service mains configure and then use it.
class Logger {
public:
    static Logger& instance();

    void write(LogLevel level, const std::string& message, std::vector<LogField> fields = {});
    void trace(const std::string& message, std::vector<LogField> fields = {});
    void debug(const std::string& message, std::vector<LogField> fields = {});
    void info(const std::string& message, std::vector<LogField> fields = {});
    void warn(const std::string& message, std::vector<LogField> fields = {});
    void error(const std::string& message, std::vector<LogField> fields = {});

    void set_sink(std::unique_ptr<LogSink> sink);
    void set_min_level(LogLevel level);
    bool enabled(LogLevel level) const;

private:
    Logger();

    mutable std::mutex mutex_;
    std::unique_ptr<LogSink> sink_;
    LogLevel min_level_ = LogLevel::Info;
};

} // namespace tether::common
