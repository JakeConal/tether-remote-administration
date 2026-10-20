// Phase 2 task 2.3: structured logging tests (design D8).
// The logger's level + timestamp + message + key=value fields render with the
// expected shape, session/seq context renders as fields, level filtering
// holds, and the injected sink captures output instead of touching stderr.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "logging.hpp"

#include <memory>
#include <regex>
#include <string>
#include <vector>

namespace {

// Captures formatted records instead of writing anywhere.
class StringLogSink final : public tether::common::LogSink {
public:
    void write(const tether::common::LogRecord& record) override {
        lines_.push_back(tether::common::format_log_record(record));
    }

    const std::vector<std::string>& lines() const { return lines_; }

private:
    std::vector<std::string> lines_;
};

} // namespace

TEST_CASE("log record renders level, timestamp, message, and contextual fields") {
    auto& logger = tether::common::Logger::instance();
    auto sink = std::make_unique<StringLogSink>();
    const StringLogSink& sink_ref = *sink;
    logger.set_sink(std::move(sink));
    logger.set_min_level(tether::common::LogLevel::Info);

    logger.info("command dispatched", {{"session", "agent-laptop"}, {"seq", "7"}});
    logger.error("session dead", {{"session", "agent-laptop"}});
    logger.trace("below minimum level, must stay invisible");

    REQUIRE(sink_ref.lines().size() == 2);

    // Level + ISO-8601 UTC timestamp prefix, e.g. 2026-10-20T12:34:56Z [info] ...
    static const std::regex kTimestampRegex(
        R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z )");
    static const std::regex kFieldRegex("session=agent-laptop seq=7");

    const std::string first = sink_ref.lines()[0];
    CHECK(std::regex_search(first, kTimestampRegex));
    CHECK(first.find("[info] command dispatched") != std::string::npos);
    CHECK(std::regex_search(first, kFieldRegex));

    const std::string second = sink_ref.lines()[1];
    CHECK(std::regex_search(second, kTimestampRegex));
    CHECK(second.find("[error] session dead") != std::string::npos);
    CHECK(second.find("session=agent-laptop") != std::string::npos);
}

TEST_CASE("level names render for all levels") {
    using tether::common::LogLevel;
    using tether::common::log_level_name;
    CHECK(std::string(log_level_name(LogLevel::Trace)) == "trace");
    CHECK(std::string(log_level_name(LogLevel::Debug)) == "debug");
    CHECK(std::string(log_level_name(LogLevel::Info)) == "info");
    CHECK(std::string(log_level_name(LogLevel::Warn)) == "warn");
    CHECK(std::string(log_level_name(LogLevel::Error)) == "error");
}

TEST_CASE("entries below the minimum level are suppressed") {
    auto& logger = tether::common::Logger::instance();
    auto sink = std::make_unique<StringLogSink>();
    const StringLogSink& sink_ref = *sink;
    logger.set_sink(std::move(sink));
    logger.set_min_level(tether::common::LogLevel::Warn);

    logger.info("not shown");
    logger.error("shown");
    CHECK(sink_ref.lines().size() == 1);
    CHECK(sink_ref.lines()[0].find("[error] shown") != std::string::npos);
}
