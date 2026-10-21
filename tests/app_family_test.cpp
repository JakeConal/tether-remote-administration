// tests/app_family_test.cpp - task 6.8: the app family on the platform in
// use, through the dispatcher with the REAL per-OS implementation and an
// INJECTED listing root. The launched children are always the tether-argv-
// probe helper built alongside the tests: start assertions observe the exact
// argv the child received (the parameters-are-data guarantee rides into the
// app launch path), and the stop test rides the probe's bounded "alive" mode.
//
//  - Windows: Start-menu `.lnk` fixtures created through the COM shell API
//    (IShellLinkW + IPersistFile::Save) - the same API the family resolves
//    with - into a temporary root. Broken shortcuts resolve to the
//    structured not_found outcome.
//  - Linux: freedesktop `.desktop` fixtures (Type=Application, Name=,
//    Exec=...) in a temporary root; Exec= is parsed as ARGV by the family
//    (parse_desktop_exec is pinned separately, on both platforms).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app.hpp"
#include "matrix.hpp"
#include "process.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib> // getenv
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#endif
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace tsys = tether::syscalls;
namespace fs = std::filesystem;

namespace {

std::atomic<int> g_dir_counter{0};

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
    const fs::path dir =
        fs::temp_directory_path() /
        ("tether-app-tests-" + std::to_string(g_dir_counter.fetch_add(1)));
    fs::create_directories(dir);
    return dir;
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

// "started pid N" rows come from the family glue (process start and app
// start share the row shape).
std::uint32_t pid_from_outcome(const tsys::Outcome& outcome) {
    REQUIRE(outcome.ok);
    REQUIRE(!outcome.lines.empty());
    const std::string& row = outcome.lines.front();
    const std::size_t marker = row.find("pid ");
    REQUIRE(marker != std::string::npos);
    const std::size_t first_digit = marker + 4;
    std::size_t end = first_digit;
    while (end < row.size() && row[end] >= '0' && row[end] <= '9') {
        ++end;
    }
    return static_cast<std::uint32_t>(std::stoul(row.substr(first_digit, end - first_digit)));
}

#if defined(_WIN32)

// Writes one Start-menu shortcut through the shell API the family resolves
// with (IShellLinkW + IPersistFile::Save). Returns success.
bool create_shortcut(const std::wstring& lnk_path, const std::wstring& target,
                     const std::wstring& arguments) {
    const HRESULT initialized =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool com_was_mine = SUCCEEDED(initialized);
    if (!com_was_mine && initialized != RPC_E_CHANGED_MODE) {
        return false;
    }
    IShellLinkW* link = nullptr;
    const HRESULT created =
        CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(created) || link == nullptr) {
        if (com_was_mine) {
            CoUninitialize();
        }
        return false;
    }
    bool saved = false;
    if (SUCCEEDED(link->SetPath(target.c_str())) &&
        SUCCEEDED(link->SetArguments(arguments.c_str()))) {
        IPersistFile* persist = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist))) && persist != nullptr) {
            saved = SUCCEEDED(persist->Save(lnk_path.c_str(), TRUE));
            persist->Release();
        }
    }
    link->Release();
    if (com_was_mine) {
        CoUninitialize();
    }
    return saved;
}

std::wstring widen_utf8(const char* narrow) { 
    // ASCII-only fixture paths are sufficient (and keep the text conversion
    // trivial): the argv-data assertions below use ASCII tokens by design.
    std::wstring wide;
    while (*narrow != '\0') {
        wide.push_back(static_cast<wchar_t>(*narrow++));
    }
    return wide;
}

#endif

struct AppFixtures {
    fs::path dir;
    std::string root;
    fs::path outfile;
    fs::path touchfile;

    AppFixtures()
        : dir(fresh_scratch_dir()), root(dir.string()), outfile(dir / "argv.txt"),
          touchfile(dir / "touched.txt") {}
    ~AppFixtures() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    AppFixtures(const AppFixtures&) = delete;
    AppFixtures& operator=(const AppFixtures&) = delete;
};

} // namespace

TEST_CASE("parse_desktop_exec splits an Exec value into argv per the spec") {
    CHECK(tsys::parse_desktop_exec("") == std::vector<std::string>{});
    CHECK(tsys::parse_desktop_exec("/usr/bin/prog --flag value") ==
          std::vector<std::string>{"/usr/bin/prog", "--flag", "value"});
    // Quoting: single-quoted strings carry no escapes (they close at the
    // next quote; a backslash is data); double-quoted ones honor the
    // \\ \" \` \$ set; unquoted whitespace splits.
    CHECK(tsys::parse_desktop_exec("prog 'two  spaces' \"esc\\\"in\\$de\"") ==
          std::vector<std::string>{"prog", "two  spaces", "esc\"in$de"});
    CHECK(tsys::parse_desktop_exec("prog 'stay \\ literal'") ==
          std::vector<std::string>{"prog", "stay \\ literal"});
    // Percent field codes are removed and ignored (this launcher serves no
    // file/URL/name fields); %% rides as one literal percent; % followed by
    // a non-alpha character is plain data.
    CHECK(tsys::parse_desktop_exec("prog -%f-file %U %n") ==
          std::vector<std::string>{"prog", "--file"});
    CHECK(tsys::parse_desktop_exec("prog 100%%literal") ==
          std::vector<std::string>{"prog", "100%literal"});
    CHECK(tsys::parse_desktop_exec("prog inline%20codes") ==
          std::vector<std::string>{"prog", "inline%20codes"});
}

#if defined(_WIN32)

TEST_CASE("windows: the app family lists, launches and stops Start-menu entries") {
    REQUIRE_MESSAGE(fs::exists(probe_executable()), "probe binary missing next to the tests");
    AppFixtures fixtures;
    const std::wstring target = probe_executable().wstring();

    // Fixtures: two launchable shortcuts (one nested, proving the recursive
    // walk) and a non-shortcut file that contributes nothing to listings.
    // The probe's contract is `<mode> <outfile> <data...>`: the outfile rides
    // argv[2] and everything after it is observed verbatim.
    REQUIRE(create_shortcut(fixtures.dir / L"Echo A.lnk", target,
                            L"argv " + widen_utf8(fixtures.outfile.string().c_str()) +
                                L" argv --with \"tail quoted\" a;b c|d"));
    fs::create_directories(fixtures.dir / "Nested");
    REQUIRE(create_shortcut(fixtures.dir / L"Nested" / L"Echo B.lnk", target,
                            L"alive --until --stopped"));
    {
        std::ofstream plain(fixtures.dir / "not-a-launcher.txt", std::ios::binary);
        plain << "ignored\n";
    }

    tsys::CommandDispatcher dispatcher;
    const std::shared_ptr<tsys::IProcessControl> processes = tsys::make_process_control();
    dispatcher.set_process_control(processes);
    dispatcher.set_app_launcher(tsys::make_app_launcher_with_roots({fixtures.root}, processes));

    // list: every .lnk under (and under subdirectories of) the root, sorted
    // by name; non-shortcuts contribute nothing.
    const tsys::Outcome listed = dispatcher.dispatch("list_app", {});
    REQUIRE(listed.ok);
    std::vector<std::string> names;
    for (const std::string& line : listed.lines) {
        REQUIRE(!line.empty());
        names.push_back(line.substr(0, line.find(' ')));
    }
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "Echo A");
    CHECK(names[1] == "Echo B");
    CHECK(listed.lines[0].find(fixtures.root + "\\Echo A.lnk") != std::string::npos);
    CHECK(listed.lines[1].find(fixtures.root + "\\Nested\\Echo B.lnk") != std::string::npos);

    // start: the shortcut resolves through the shell API into plain argv and
    // the probe receives the tail VERBATIM - including metacharacters and a
    // quoted piece - proving no shell or resplitting happened in between.
    std::error_code ec;
    fs::remove(fixtures.outfile, ec);
    const tsys::Outcome launched = dispatcher.dispatch("start_app", {"Echo A"});
    REQUIRE(launched.ok); // structured start outcome naming the child's pid
    (void)pid_from_outcome(launched);
    REQUIRE(poll_until([&] { return fs::exists(fixtures.outfile, ec) && !ec; }, 15000));
    const std::vector<std::string> argv_lines = read_lines(fixtures.outfile);
    REQUIRE(argv_lines.size() >= 6); // "argv0" header + the five data args
    CHECK(argv_lines[1] == "argv");
    CHECK(argv_lines[2] == "--with");
    CHECK(argv_lines[3] == "tail quoted");
    CHECK(argv_lines[4] == "a;b");
    CHECK(argv_lines[5] == "c|d");

    // stop: the "alive" child runs until the family stops it.
    const tsys::Outcome started_b = dispatcher.dispatch("start_app", {"Echo B"});
    REQUIRE(started_b.ok);
    const std::uint32_t b_pid = pid_from_outcome(started_b);
    REQUIRE(poll_until(
        [&] { return fs::exists(fixtures.touchfile, ec) && !ec; }, 15000));
    const tsys::Outcome stopped = dispatcher.dispatch("stop_app", {std::to_string(b_pid)});
    REQUIRE(stopped.ok);
    CHECK(stopped.lines.front().find("stopped pid " + std::to_string(b_pid)) != std::string::npos);

    // stop after termination: the structured not_found outcome.
    const tsys::Outcome stopped_again = dispatcher.dispatch("stop_app", {std::to_string(b_pid)});
    CHECK(!stopped_again.ok);
    CHECK(stopped_again.code == "not_found");

    // Unknown names refuse with the structured outcome.
    const tsys::Outcome unknown = dispatcher.dispatch("start_app", {"Named No Such"});
    CHECK(!unknown.ok);
    CHECK(unknown.code == "not_found");

    // Broken shortcut (target does not exist): a structured refusal, not a
    // modal prompt and not an unclassified crash.
    REQUIRE(create_shortcut(fixtures.dir / L"Broken.lnk", L"C:\\tether-missing-target\\absent.exe",
                            L""));
    const tsys::Outcome broken = dispatcher.dispatch("start_app", {"Broken"});
    CHECK(!broken.ok);
    CHECK(broken.code == "not_found");
}

#else

TEST_CASE("linux: the app family lists, launches and stops .desktop entries") {
    REQUIRE_MESSAGE(fs::exists(probe_executable()), "probe binary missing next to the tests");
    AppFixtures fixtures;
    const std::string probe = probe_executable().string();
    const std::string argv_out = fixtures.outfile.string();
    const std::string touch_out = fixtures.touchfile.string();

    // Fixtures: two Application entries (one without a Name=, riding the
    // filename stem) plus a Type=Link entry that is not launchable.
    {
        std::ofstream entry(fixtures.dir / "echo-a.desktop", std::ios::binary);
        entry << "[Desktop Entry]\n";
        entry << "Type=Application\n";
        entry << "Name=Echo A\n";
        // The Exec value is DATA: metacharacters and %codes must never reach
        // a shell - the probe proves they arrive verbatim (or not at all).
        entry << "Exec=" << probe << " argv " << argv_out << " argv a;b c|d with%20codes-%%lit\n";
    }
    {
        std::ofstream entry(fixtures.dir / "echo-b.desktop", std::ios::binary);
        entry << "[Desktop Entry]\n";
        entry << "Type=Application\n";
        entry << "Exec=" << probe << " alive " << touch_out << "\n";
    }
    {
        std::ofstream entry(fixtures.dir / "linked.desktop", std::ios::binary);
        entry << "[Desktop Entry]\n";
        entry << "Type=Link\n";
        entry << "Name=Not An App\n";
        entry << "URL=http://example.invalid\n";
    }

    tsys::CommandDispatcher dispatcher;
    const std::shared_ptr<tsys::IProcessControl> processes = tsys::make_process_control();
    dispatcher.set_process_control(processes);
    dispatcher.set_app_launcher(tsys::make_app_launcher_with_roots({fixtures.root}, processes));

    // list: the two Application entries sorted by name, the link excluded.
    const tsys::Outcome listed = dispatcher.dispatch("list_app", {});
    REQUIRE(listed.ok);
    REQUIRE(listed.lines.size() == 2);
    CHECK(listed.lines[0].rfind("Echo A ", 0) == 0);
    CHECK(listed.lines[0].find(fixtures.root + "/echo-a.desktop") != std::string::npos);
    CHECK(listed.lines[1].rfind("echo-b ", 0) == 0); // no Name= -> the file stem

    // start: the Exec= pieces become the child's argv verbatim - including
    // the metacharacters - and the %field codes are stripped (the %% rides
    // as one literal percent).
    std::error_code ec;
    fs::remove(fixtures.outfile, ec);
    const tsys::Outcome launched = dispatcher.dispatch("start_app", {"Echo A"});
    const std::string launch_detail = launched.code + ": " + launched.message;
    REQUIRE_MESSAGE(launched.ok, launch_detail);
    (void)pid_from_outcome(launched);
    REQUIRE(poll_until([&] { return fs::exists(fixtures.outfile, ec) && !ec; }, 15000));
    const std::vector<std::string> argv_lines = read_lines(fixtures.outfile);
    REQUIRE(argv_lines.size() >= 5); // "argv0" header + the data args
    CHECK(argv_lines[1] == "argv");
    CHECK(argv_lines[2] == "a;b");
    CHECK(argv_lines[3] == "c|d");
    CHECK(argv_lines[4] == "with%20codes-%lit");

    // stop: the "alive" child runs until the family stops it.
    const tsys::Outcome started_b = dispatcher.dispatch("start_app", {"echo-b"});
    REQUIRE(started_b.ok);
    const std::uint32_t b_pid = pid_from_outcome(started_b);
    REQUIRE(poll_until(
        [&] { return fs::exists(fixtures.touchfile, ec) && !ec; }, 15000));
    const tsys::Outcome stopped = dispatcher.dispatch("stop_app", {std::to_string(b_pid)});
    REQUIRE(stopped.ok);
    CHECK(stopped.lines.front().find("stopped pid " + std::to_string(b_pid)) != std::string::npos);

    // stop after termination: the structured not_found outcome.
    const tsys::Outcome stopped_again = dispatcher.dispatch("stop_app", {std::to_string(b_pid)});
    CHECK(!stopped_again.ok);
    CHECK(stopped_again.code == "not_found");

    // Unknown names refuse with the structured outcome.
    const tsys::Outcome unknown = dispatcher.dispatch("start_app", {"no-such-app"});
    CHECK(!unknown.ok);
    CHECK(unknown.code == "not_found");

    // A dead Exec target (ENOENT at exec time) refuses with not_found too.
    {
        std::ofstream entry(fixtures.dir / "absent.desktop", std::ios::binary);
        entry << "[Desktop Entry]\n";
        entry << "Type=Application\n";
        entry << "Name=Absent Target\n";
        entry << "Exec=/tether-no-such-dir/absent-binary\n";
    }
    const tsys::Outcome broken = dispatcher.dispatch("start_app", {"Absent Target"});
    CHECK(!broken.ok);
    CHECK(broken.code == "not_found");
}

#endif
