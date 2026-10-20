// tests/process_family_test.cpp - task 6.2: the process family through its
// REAL per-OS implementation (Toolhelp/CreateProcessW/TerminateProcess or
// /proc/fork+exec/signals). CI-safe by construction: the only children this
// suite launches are the tether-argv-probe helper built alongside the tests
// - nothing system-touching - and every "alive until killed" child stops
// through the family itself (or self-expires after 20 s if the suite dies).
//
// Scenarios map to the remote-commands delta:
//  - "parameter containing shell metacharacters is treated literally": the
//    probe child receives ';' '|' '&' '>' tokens (and quoting-exotic ones)
//    VERBATIM as argv data after a real argv-based launch - the proof that
//    no shell ever re-spliced them;
//  - well-formed parameters take the structured path (list/start/stop all
//    report structured outcomes; failures are classified, never uncaught).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "matrix.hpp"
#include "process.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdlib> // getenv
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace tsys = tether::syscalls;
namespace fs = std::filesystem;

namespace {

std::atomic<int> g_dir_counter{0};

constexpr const char* kNoWait = "the probe never produced its output in time";

// Probe binary + per-case scratch dir. The probe builds NEXT to the test
// binary (same target dir on single- and multi-config generators), located
// with the same probing pattern the tcp suite uses for the agent binary
// (TETHER_PROBE_EXE overrides for advanced use).
fs::path probe_executable() {
    if (const char* from_env = std::getenv("TETHER_PROBE_EXE")) {
        std::error_code ignored;
        const fs::path candidate(from_env);
        if (fs::exists(candidate, ignored) && !ignored) {
            return fs::weakly_canonical(candidate);
        }
    }
    char buffer[4096]{};
#if defined(_WIN32)
    const DWORD length = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
    REQUIRE(length > 0);
#else
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    REQUIRE(length > 0);
    buffer[static_cast<std::size_t>(length)] = '\0';
#endif
    const fs::path here = fs::path(buffer).parent_path();
    for (const fs::path& candidate :
         {here / "tether-argv-probe.exe", here / "tether-argv-probe"}) {
        std::error_code ignored;
        if (fs::exists(candidate, ignored) && !ignored) {
            return fs::weakly_canonical(candidate);
        }
    }
    return {};
}

fs::path fresh_scratch_dir() {
    const fs::path dir = fs::temp_directory_path() /
                         ("tether-process-tests-" + std::to_string(g_dir_counter++));
    fs::create_directories(dir);
    return dir;
}

bool alive(std::uint32_t pid) {
#if defined(_WIN32)
    // No family handle survives a launch (the impl releases them): liveness
    // re-opens by pid. A terminated unreferenced process loses its object.
    const HANDLE open = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (open == nullptr) {
        return false;
    }
    const bool running = WaitForSingleObject(open, 0) == WAIT_TIMEOUT;
    CloseHandle(open);
    return running;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

bool poll_until(const std::function<bool()>& condition, int budget_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    for (;;) {
        if (condition()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool file_exists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec) && !ec;
}

std::vector<std::string> read_lines(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back(); // text-mode newline on Windows reads
        }
        lines.push_back(line);
    }
    return lines;
}

// "started pid N" / "stopped pid N (...)" rows come from the family glue.
std::uint32_t pid_from_outcome(const tsys::Outcome& outcome) {
    REQUIRE(outcome.ok);
    REQUIRE(!outcome.lines.empty());
    const std::string& row = outcome.lines.front();
    std::size_t marker = row.find("pid ");
    REQUIRE(marker != std::string::npos);
    marker += 4;
    std::size_t end = marker;
    while (end < row.size() && row[end] >= '0' && row[end] <= '9') {
        ++end;
    }
    return static_cast<std::uint32_t>(std::stoul(row.substr(marker, end - marker)));
}

struct Fixture {
    Fixture() : dir(fresh_scratch_dir()) {}
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    fs::path dir;
};

} // namespace

TEST_CASE("list reports a live snapshot through the structured path") {
    const std::shared_ptr<tsys::IProcessControl> control = tsys::make_process_control();
    const std::vector<tsys::ProcessInfo> processes = control->list();
    REQUIRE_MESSAGE(!processes.empty(), "list found no processes at all");

    // The test's own process runs here, so the snapshot must contain it -
    // a sharper claim than "found something".
    const std::uint32_t self_pid = []() -> std::uint32_t {
#if defined(_WIN32)
        return ::GetCurrentProcessId();
#else
        return static_cast<std::uint32_t>(::getpid());
#endif
    }();
    bool found_self = false;
    for (const tsys::ProcessInfo& info : processes) {
        if (info.pid == self_pid) {
            found_self = true;
            CHECK(!info.name.empty()); // every row carries a name
        }
    }
    CHECK(found_self);
}

TEST_CASE("the launcher passes shell metacharacters to argv as literal data") {
    Fixture fixture;
    REQUIRE_MESSAGE(fs::exists(probe_executable()), "probe binary missing next to the tests");

    const std::vector<std::string> data{"a;b", "c|d", "e&f", "g>h"};
    const fs::path outfile = fixture.dir / "argv-metachars.txt";

    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(tsys::make_process_control());
    std::vector<std::string> argv{probe_executable().string(), "argv", outfile.string()};
    argv.insert(argv.end(), data.begin(), data.end());
    const tsys::Outcome launched = dispatcher.dispatch("start", argv);
    REQUIRE(launched.ok); // structured start outcome
    (void)pid_from_outcome(launched);

    // The probe exits by itself once argv has been written (auto-reaped on
    // Linux); the file is the single source of truth for this scenario.
    REQUIRE_MESSAGE(poll_until([&] { return file_exists(outfile); }, 3000), kNoWait);
    const std::vector<std::string> lines = read_lines(outfile);
    REQUIRE(lines.size() == data.size() + 1);
    CHECK(lines[0] == "argv0");
    for (std::size_t i = 0; i < data.size(); ++i) {
        CHECK(lines[i + 1] == data[i]); // ; | & > arrived one-to-one, un-split
    }
}

TEST_CASE("quoting-exotic tokens also arrive one-to-one") {
    Fixture fixture;
    const std::vector<std::string> data{"has space", "ap\"ple", "tr\\ail\\"};
    const fs::path outfile = fixture.dir / "argv-spaces.txt";

    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(tsys::make_process_control());
    std::vector<std::string> argv{probe_executable().string(), "argv", outfile.string()};
    argv.insert(argv.end(), data.begin(), data.end());
    REQUIRE(dispatcher.dispatch("start", argv).ok);

    REQUIRE_MESSAGE(poll_until([&] { return file_exists(outfile); }, 3000), kNoWait);
    const std::vector<std::string> lines = read_lines(outfile);
    REQUIRE(lines.size() == data.size() + 1);
    CHECK(lines[0] == "argv0");
    for (std::size_t i = 0; i < data.size(); ++i) {
        CHECK(lines[i + 1] == data[i]);
    }
}

TEST_CASE("stop terminates a benign child; stopping it again reports not_found") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(tsys::make_process_control());

    const fs::path flag = fixture.dir / "alive.flag";
    const tsys::Outcome launched =
        dispatcher.dispatch("start", {probe_executable().string(), "alive", flag.string()});
    REQUIRE(launched.ok);
    const std::uint32_t pid = pid_from_outcome(launched);
    REQUIRE_MESSAGE(poll_until([&] { return file_exists(flag); }, 3000),
                    kNoWait); // the child is up and looping

    const tsys::Outcome stopped = dispatcher.dispatch("stop",
                                                      {std::to_string(pid)});
    REQUIRE(stopped.ok); // structured stop outcome
    CHECK_MESSAGE(poll_until([&] { return !alive(pid); }, 2000),
                  "the stopped child kept running past two seconds");

    // Same pid, second stop: dead target, structured not_found, no crash.
    const tsys::Outcome repeated = dispatcher.dispatch("stop", {std::to_string(pid)});
    CHECK_FALSE(repeated.ok);
    CHECK(repeated.code == "not_found");
}

TEST_CASE("stop resolves a process by exact image name") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(tsys::make_process_control());

    const fs::path flag = fixture.dir / "alive-by-name.flag";
    const fs::path name_only = probe_executable().filename();
    const tsys::Outcome launched =
        dispatcher.dispatch("start", {probe_executable().string(), "alive", flag.string()});
    REQUIRE(launched.ok);
    const std::uint32_t pid = pid_from_outcome(launched);
    REQUIRE_MESSAGE(poll_until([&] { return file_exists(flag); }, 3000), kNoWait);

    const tsys::Outcome stopped = dispatcher.dispatch("stop", {name_only.string()});
    REQUIRE(stopped.ok);
    CHECK_MESSAGE(poll_until([&] { return !alive(pid); }, 2000),
                  "the named child kept running past two seconds");
}

TEST_CASE("failures come back structured, never uncaught") {
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_process_control(tsys::make_process_control());
    Fixture fixture;

    // No argv at all: the argument shape is refused before any launch.
    const tsys::Outcome empty = dispatcher.dispatch("start", {});
    CHECK_FALSE(empty.ok);
    CHECK(empty.code == "invalid_args");

    // A program that does not exist: classified, from the OS error up.
    const tsys::Outcome missing_program =
        dispatcher.dispatch("start", {(fixture.dir / "no-such-program-xyz-481516").string()});
    CHECK_FALSE(missing_program.ok);
    CHECK(missing_program.code == "not_found");

    // Stop targets: pid-less name, unknown pid - both classified not_found.
    const tsys::Outcome missing_name = dispatcher.dispatch("stop", {"no-such-name-xyz-481516"});
    CHECK_FALSE(missing_name.ok);
    CHECK(missing_name.code == "not_found");
    const tsys::Outcome unknown_pid = dispatcher.dispatch("stop", {"99999999"});
    CHECK_FALSE(unknown_pid.ok);
    CHECK(unknown_pid.code == "not_found");

    // Arity guards: structured, before any capability call.
    CHECK(dispatcher.dispatch("stop", {}).code == "invalid_args");
    CHECK(dispatcher.dispatch("stop", {"4242", "again"}).code == "invalid_args");
    CHECK(dispatcher.dispatch("list", {"extra"}).code == "invalid_args");
}
