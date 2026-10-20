// tests/cli_stream_test.cpp - task 5.3: stdio streaming through run_cli() in
// process with injected streams (no sockets, no real time). Scenarios map to
// the standard-streams requirement of the operator-console delta:
//  - text results land on stdout verbatim; stderr stays clean
//  - a piped/redirected stdout carries identical content (faceless usage)
//  - file results assemble byte-exactly: FILE_META(FILE_CHUNK*FILE_END)
//    streams the exact bytes the agent produced, into a REMOTE FILE when the
//    operator redirects them, while progress lives on stderr
//  - an incomplete file delivery reports a failure instead of masquerading as
//    a truncated success
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cli.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace tc = tether::cli;
namespace tp = tether::protocol;

namespace {

int g_dir_counter = 0;

// Console fakes whose dispatch streams exactly the canned pieces then returns
// a canned terminal view.
struct StreamingFake final : tc::IOperConsole {
    std::vector<tp::RegistryEntry> rows;
    std::vector<tp::Message> pieces;
    tp::CommandView view;
    std::vector<std::string> dispatched;

    std::vector<tp::RegistryEntry> list_agents() override { return rows; }
    bool has_agent(const std::string& identity) override {
        for (const tp::RegistryEntry& row : rows) {
            if (row.identity == identity) {
                return true;
            }
        }
        return false;
    }
    std::optional<tp::CommandView> dispatch(const std::string& agent, const tp::Cmd&,
                                            const tp::ResultHook& hook) override {
        dispatched.push_back(agent);
        for (const tp::Message& piece : pieces) {
            hook(7, piece, false);
        }
        return view;
    }
    tc::CancelOutcome cancel(const std::string&, std::uint32_t) override {
        return tc::CancelOutcome::NotEligible;
    }
};

struct Forwarder final : tc::IOperConsole {
    StreamingFake* target = nullptr;
    explicit Forwarder(StreamingFake& t) : target(&t) {}
    std::vector<tp::RegistryEntry> list_agents() override { return target->list_agents(); }
    bool has_agent(const std::string& identity) override { return target->has_agent(identity); }
    std::optional<tp::CommandView> dispatch(const std::string& agent, const tp::Cmd& command,
                                            const tp::ResultHook& hook) override {
        return target->dispatch(agent, command, hook);
    }
    tc::CancelOutcome cancel(const std::string& agent, std::uint32_t seq) override {
        return target->cancel(agent, seq);
    }
};

struct RunResult {
    int exit_code = -1;
    std::string err;
};

// Drive run_cli with `out` as the result stream; `in_simulated_tty` only
// shapes the confirmation behavior (no interactive input on success paths).
RunResult drive(StreamingFake& fake, std::ostream& out, bool in_is_tty, int& dir_counter) {
    (void)dir_counter;
    std::ostringstream err;
    std::istringstream in;
    RunResult result;
    result.exit_code = tc::run_cli({"tether-console", "exec", "agent-1", "produce"},
                                   tc::CliStreams{out, err, in, /*stdout_is_tty=*/false, in_is_tty},
                                   [&fake](const std::string&) {
                                       return std::unique_ptr<tc::IOperConsole>(
                                           new Forwarder(fake));
                                   });
    result.err = err.str();
    return result;
}

tp::CommandView completed() {
    tp::CommandView view;
    view.seq = 7;
    view.status = tp::CommandStatus::Completed;
    view.command = "produce";
    return view;
}

tp::CommandView failed(const std::string& code) {
    tp::CommandView view;
    view.seq = 7;
    view.status = tp::CommandStatus::Failed;
    view.error_code = code;
    return view;
}

} // namespace

TEST_CASE("text results reach stdout verbatim and stderr stays clean") {
    StreamingFake fake;
    fake.rows = {{"agent-1", tp::RegistryState::Live}};
    fake.pieces = {tp::ResultText{"listing:\nfile-a\nfile-b\n"}};
    fake.view = completed();

    std::ostringstream out;
    const RunResult result = drive(fake, out, false, g_dir_counter);
    CHECK(result.exit_code == tc::kExitOk);
    CHECK(out.str() == "listing:\nfile-a\nfile-b\n"); // verbatim, no banners, no CRLF games
    CHECK(result.err.empty());
    REQUIRE(fake.dispatched.size() == 1);
    CHECK(fake.dispatched[0] == "agent-1");
}

TEST_CASE("piped stdout carries the identical content as an attached stream") {
    // Same command, same pieces: the faceless delta says redirected usage
    // yields identical output content.
    StreamingFake attached_source;
    attached_source.rows = {{"agent-1", tp::RegistryState::Live}};
    attached_source.pieces = {tp::ResultText{"frame one\nframe two\n"},
                              tp::ResultText{"tail"}};
    attached_source.view = completed();

    std::ostringstream attached;
    const RunResult attached_run = drive(attached_source, attached, false, g_dir_counter);
    CHECK(attached_run.exit_code == tc::kExitOk);

    StreamingFake redirected_source;
    redirected_source.rows = {{"agent-1", tp::RegistryState::Live}};
    redirected_source.pieces = attached_source.pieces;
    redirected_source.view = completed();

    std::ostringstream redirected;
    const RunResult redirected_run = drive(redirected_source, redirected, false, g_dir_counter);
    CHECK(redirected_run.exit_code == tc::kExitOk);

    CHECK(redirected.str() == attached.str());
    CHECK(redirected.str() == "frame one\nframe two\ntail");
    CHECK(redirected_run.err.empty());
}

TEST_CASE("file results assemble byte-exactly, target file included") {
    // The agent's file: bytes that would lose a text-vs-binary fight.
    std::vector<unsigned char> png_bytes;
    png_bytes.insert(png_bytes.end(), {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'});
    for (std::size_t i = 0; i < 64; ++i) {
        png_bytes.push_back(static_cast<unsigned char>(i * 37));
    }
    png_bytes.insert(png_bytes.end(), {'\n', 0x00, '\n'});

    const auto file_pieces = [&png_bytes] {
        std::vector<tp::Message> pieces;
        pieces.push_back(tp::FileMeta{"shot.png", png_bytes.size()});
        constexpr std::size_t kChunk = 30;
        for (std::size_t offset = 0; offset < png_bytes.size(); offset += kChunk) {
            const std::size_t end =
                offset + kChunk > png_bytes.size() ? png_bytes.size() : offset + kChunk;
            pieces.push_back(
                tp::FileChunk{std::vector<unsigned char>(png_bytes.begin() + offset,
                                                          png_bytes.begin() + end)});
        }
        pieces.push_back(tp::FileEnd{});
        return pieces;
    }();

    // (a) attached simulation: the result stream holds the exact bytes.
    StreamingFake attached;
    attached.rows = {{"agent-1", tp::RegistryState::Live}};
    attached.pieces = file_pieces;
    attached.view = completed();
    std::ostringstream attached_out;
    const RunResult attached_run = drive(attached, attached_out, false, g_dir_counter);
    CHECK(attached_run.exit_code == tc::kExitOk);
    const auto bytes_equal = [](unsigned char expected, char actual) {
        return expected == static_cast<unsigned char>(actual);
    };
    std::string as_string = attached_out.str();
    REQUIRE(as_string.size() == png_bytes.size());
    CHECK(std::equal(png_bytes.begin(), png_bytes.end(), as_string.begin(), bytes_equal));

    // (b) the operator redirects stdout into a file: byte-identical file.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("tether-stream-tests-" + std::to_string(g_dir_counter++));
    std::filesystem::create_directories(dir);
    const std::filesystem::path target = dir / "shot.png";
    {
        std::ofstream redirected(target, std::ios::binary);
        const RunResult redirected_run = drive(attached, redirected, false, g_dir_counter);
        CHECK(redirected_run.exit_code == tc::kExitOk);
        // Progress/status rides stderr, never the result stream.
        CHECK(redirected_run.err.find("status: file 'shot.png'") != std::string::npos);
        CHECK(redirected_run.err.find("status: file received (") != std::string::npos);
    }

    std::ifstream verify(target, std::ios::binary);
    std::string file_bytes((std::istreambuf_iterator<char>(verify)),
                           std::istreambuf_iterator<char>());
    verify.close();
    REQUIRE(file_bytes.size() == png_bytes.size());
    CHECK(std::equal(png_bytes.begin(), png_bytes.end(), file_bytes.begin(), bytes_equal));
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

TEST_CASE("an incomplete file delivery is a reported failure, not a truncated success") {
    StreamingFake fake;
    fake.rows = {{"agent-1", tp::RegistryState::Live}};
    fake.pieces = {tp::FileMeta{"plan.bin", 10},
                   tp::FileChunk{std::vector<unsigned char>{'a', 'b', 'c'}},
                   tp::FileEnd{}};
    fake.view = completed();

    std::ostringstream out;
    const RunResult result = drive(fake, out, false, g_dir_counter);
    CHECK(result.exit_code == tc::kExitFailure);
    CHECK(result.err.find("error: file delivery incomplete") != std::string::npos);
}

TEST_CASE("a failed file command never fakes completion") {
    StreamingFake fake;
    fake.rows = {{"agent-1", tp::RegistryState::Live}};
    fake.pieces = {tp::FileMeta{"plan.bin", 4},
                   tp::FileChunk{std::vector<unsigned char>{'a', 'b', 'c', 'd'}}};
    fake.view = failed("delivery_failed");

    std::ostringstream out;
    const RunResult result = drive(fake, out, false, g_dir_counter);
    CHECK(result.exit_code == tc::kExitFailure);
    CHECK(result.err.find("error: command failed: delivery_failed") != std::string::npos);
}
