// lib/common/logging.cpp - minimal structured logging implementation (D8).
#include "logging.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <ctime>

namespace tether::common {
namespace {

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

} // namespace

const char* log_level_name(LogLevel level) {
    return level_name(level);
}

std::string utc_timestamp_now() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    return buffer;
}

std::string format_log_record(const LogRecord& record) {
    std::string line = record.timestamp;
    line += " [";
    line += level_name(record.level);
    line += "] ";
    line += record.message;
    for (const LogField& field : record.fields) {
        line += ' ';
        line += field.name;
        line += '=';
        line += field.value;
    }
    return line;
}

StreamLogSink::StreamLogSink(std::ostream& out) : out_(out) {}

void StreamLogSink::write(const LogRecord& record) {
    out_ << format_log_record(record) << '\n';
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger() : sink_(std::make_unique<StreamLogSink>(std::cerr)) {}

void Logger::set_sink(std::unique_ptr<LogSink> sink) {
    if (!sink) {
        return; // never unhook the logger; injecting an empty sink is a caller bug
    }
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

void Logger::set_min_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_level_ = level;
}

bool Logger::enabled(LogLevel level) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level >= min_level_;
}

void Logger::write(LogLevel level, const std::string& message, std::vector<LogField> fields) {
    LogRecord record{level, utc_timestamp_now(), message, std::move(fields)};
    std::lock_guard<std::mutex> lock(mutex_);
    sink_->write(record);
}

void Logger::trace(const std::string& message, std::vector<LogField> fields) {
    if (enabled(LogLevel::Trace)) {
        write(LogLevel::Trace, message, std::move(fields));
    }
}

void Logger::debug(const std::string& message, std::vector<LogField> fields) {
    if (enabled(LogLevel::Debug)) {
        write(LogLevel::Debug, message, std::move(fields));
    }
}

void Logger::info(const std::string& message, std::vector<LogField> fields) {
    if (enabled(LogLevel::Info)) {
        write(LogLevel::Info, message, std::move(fields));
    }
}

void Logger::warn(const std::string& message, std::vector<LogField> fields) {
    if (enabled(LogLevel::Warn)) {
        write(LogLevel::Warn, message, std::move(fields));
    }
}

void Logger::error(const std::string& message, std::vector<LogField> fields) {
    if (enabled(LogLevel::Error)) {
        write(LogLevel::Error, message, std::move(fields));
    }
}

} // namespace tether::common
