// tests/families_e2e_test.cpp - task 6.10: the scenario checks for every
// command family on the platform in use, over REAL loopback TCP sessions.
// Every verb below rides the FULL operator stack (run_cli -> TcpOperConsole
// -> TcpConsoleTransport -> ConsoleSession over live loopback sockets ->
// AgentSession -> agent::install_commands -> the REAL capability dispatcher),
// i.e. exactly the agent-side dispatch shape tether-agent's main wires, with
// per-family injections only where a host device would else be required:
//
//  - process (real per-OS backend):  list, a benign start of a probe child,
//    stop by pid (argv-based launch + signal/terminate, both live);
//  - file (real std::filesystem):    ls on a temp dir, the confirmed delete
//    actually removing a file (operator --yes/dialogue + the agent's own
//    confirm token), the unconfirmed delete refusing before anything runs,
//    and a metacharacter-bearing name looked up literally;
//  - power (refusal paths ONLY):     without confirmation the CLI refuses
//    before dispatch (nothing sent); operator-confirmed dispatch arrives and
//    is refused by the agent's own confirm gate (missing_confirmation). The
//    family slot is wired with a tripwire fake that fails loudly - no test
//    here ever asks the platform for a real power transition;
//  - capture:                        the fake-source screenshot streams its
//    DELIVERED capture through FILE_META/FILE_CHUNK/FILE_END byte-exact to
//    the console (stdout) plus the REAL per-OS grab smoke (GDI on the CI
//    Windows leg / X11 under xvfb on the CI Linux leg; a genuinely headless
//    host resolves to the classified refusal instead);
//  - camera (fake device, no webcam required):  start runs as a background
//    job, a duplicate start is refused with the incumbent untouched, and the
//    stop delivers the accumulated frame as a decodable PNG over the wire;
//  - input (injected seams):         the keylogger job accumulates through
//    the REAL file capture sink into a temp storage, delivers the events at
//    stop, and a Wayland environment resolves to the structured unsupported
//    refusal without wedging the session (environment probe injection);
//  - app (real per-OS fixtures):     Start-menu .lnk entries created through
//    the shell API on Windows / freedesktop .desktop entries on Linux, list
//    seeing them, craft-launch running the probe child verbatim, stop_app
//    ending the child;
//  - audit:                          the run leaves a persisted JSONL trail
//    spanning executed / refused / failed command records AND background-job
//    lifecycle events with every required field.
//
// Refusals are asserted session-preserving everywhere: the next command on
// the SAME session succeeds right after every refusal.
//
// Real waits stay tiny: handshake and dispatch budgets are ~1.5-2 s, the
// background cleanups ride poll loops (<= 15 s, event-driven where possible),
// and every wait between CLI calls keeps pumping both stack halves.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app.hpp"
#include "audit.hpp"
#include "camera.hpp"
#include "capture.hpp"
#include "command_handler.hpp"
#include "config.hpp"
#include "file.hpp"
#include "image_png.hpp"
#include "input.hpp"
#include "jobs.hpp"
#include "logging.hpp"
#include "power.hpp"
#include "process.hpp"
#include "session.hpp"
#include "tcp.hpp"
#include "tcp_console.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib> // getenv, std::_Exit
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX // already set by an included header's windows.h chain
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#endif
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace tcli = tether::cli;
namespace tcom = tether::common;
namespace tsys = tether::syscalls;
namespace tp = tether::protocol;
namespace tt = tether::transport;
namespace fs = std::filesystem;

namespace {

constexpr const char* kAgentIdentity = "e2e-agent";
constexpr const char* kAgentHost = "host-e2e";
constexpr const char* kAgentToken = "tok-e2e";

std::atomic<int> g_dir_counter{0};

fs::path fresh_scratch_dir(const char* prefix) {
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string(prefix) + "-" + std::to_string(g_dir_counter.fetch_add(1)));
    fs::create_directories(dir);
    return dir;
}

// Unbuffered progress markers on stderr: if a case wedges on a platform, the
// last marker still in the captured log names it exactly (std::cerr is
// unit-buffered and survives even a hard process termination).
void e2e_trace(const std::string& message) {
    std::cerr << "[e2e] " << message << std::endl;
}

void e2e_step(const std::string& label) { e2e_trace("step: " + label); }

// The case the test binary is currently inside (literals only, so the
// watchdog below may read it from its own thread without locks).
std::atomic<const char*> g_current_case{"<startup: no case entered yet>"};

// One breadcrumb per case entry: prints entry/exit and updates the watchdog's
// current-case pointer.
struct CaseTrace {
    explicit CaseTrace(const char* name) {
        g_current_case.store(name);
        e2e_trace(std::string("enter ") + name);
    }
    ~CaseTrace() { e2e_trace("exit"); }
};

// ---- the wedge watchdog -------------------------------------------------------
//
// CI evidence: this suite wedged on windows-latest for the full ctest budget
// (600 s) with ZERO captured output - arguably the first case never even
// entered. This watchdog is a static-initialization thread (it lives from
// before main across every future case), prints its own unbuffered markers,
// and at the hard budget self-terminates with _Exit(34): ctest then records a
// normal failed test with the captured stderr up to that point - never a mute
// kill whose partial output dies with the pipe. The per-case ctest TIMEOUT in
// tests/CMakeLists.txt stays the outer backstop (larger than this budget).
void e2e_watchdog_body() {
    constexpr int kWatchdogBudgetSeconds = 90;
    std::cerr << "[watchdog] thread up (budget " << kWatchdogBudgetSeconds
              << " s); case '" << g_current_case.load() << "'" << std::endl;
    for (int elapsed = 10; elapsed < kWatchdogBudgetSeconds; elapsed += 10) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::cerr << "[watchdog] alive " << elapsed << " s; case '"
                  << g_current_case.load() << "'" << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::cerr << "[watchdog] budget of " << kWatchdogBudgetSeconds
              << " s exceeded inside case '" << g_current_case.load()
              << "'; self-terminating so the wedge fails loud with its "
              << "captured stderr intact" << std::endl;
    std::_Exit(34);
}

struct E2eWatchdogLauncher {
    E2eWatchdogLauncher() { std::thread(e2e_watchdog_body).detach(); }
};

// static-initialization anchor: starts the thread before main and before
// any test case (the monitor always runs even for a startup-time wedge).
const E2eWatchdogLauncher g_e2e_watchdog{};

// ---- tiny guards over the logger (status noise must not leak into output) ----

class NullLogSink final : public tcom::LogSink {
public:
    void write(const tcom::LogRecord&) override {}
};

void silence_logging() {
    tcom::Logger::instance().set_sink(std::make_unique<NullLogSink>());
    tcom::Logger::instance().set_min_level(tcom::LogLevel::Error);
}

// ---- small file/process helpers ------------------------------------------------

std::string file_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_bytes(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out << content;
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

bool child_gone(std::uint32_t pid) {
#if defined(_WIN32)
    HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return true;
    }
    ::CloseHandle(handle);
    return false;
#else
    return ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
#endif
}

// "started pid N" / "stopped pid N" rows come from the family glue (process
// start/stop and app start share the row shape).
std::uint32_t pid_from_row(const std::string& row) {
    const std::size_t marker = row.find("pid ");
    REQUIRE(marker != std::string::npos);
    const std::size_t first_digit = marker + 4;
    std::size_t end = first_digit;
    while (end < row.size() && row[end] >= '0' && row[end] <= '9') {
        ++end;
    }
    return static_cast<std::uint32_t>(std::stoul(row.substr(first_digit, end - first_digit)));
}

// The tether-argv-probe child built alongside the tests (the benign process
// the process and app scenarios launch and stop).
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
    const ssize_t length =
        ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    REQUIRE(length > 0);
    buffer[length] = '\0';
#endif
    const fs::path here = fs::path(buffer).parent_path();
    for (const fs::path& candidate : {here / "tether-argv-probe.exe", here / "tether-argv-probe"}) {
        std::error_code ignored;
        if (fs::exists(candidate, ignored) && !ignored) {
            return fs::weakly_canonical(candidate);
        }
    }
    return {};
}

// ---- the fake capture/camera/input sources (no device on any CI runner) -----

// A deterministic frame: pixel (b,g,r) = ((x*3 + y*7 + p) % 251).
tsys::Frame make_frame(const int width, const int height) {
    tsys::Frame frame;
    frame.width = width;
    frame.height = height;
    frame.bgr.resize(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 3;
            for (int p = 0; p < 3; ++p) {
                frame.bgr[offset + p] = static_cast<unsigned char>((x * 3 + y * 7 + p) % 251);
            }
        }
    }
    return frame;
}

bool frames_equal(const tsys::Frame& a, const tsys::Frame& b) {
    return a.width == b.width && a.height == b.height && a.bgr == b.bgr;
}

class FakeScreenCapture final : public tsys::IScreenCapture {
public:
    int grab_calls = 0;
    tsys::Outcome::Deliverable grab() override {
        ++grab_calls;
        tsys::Outcome::Deliverable deliverable;
        deliverable.name = "screenshot.png";
        deliverable.bytes = tsys::encode_png_bgr(make_frame(9, 7));
        return deliverable;
    }
};

// The camera worker's read counter: the probe increments on EVERY read, so
// each poll threshold proves the worker is (still) capturing; no threshold
// off-by-one can wedge a poll because reads pace every ~2 ms.
class FakeFrameSource final : public tsys::IFrameSource {
public:
    explicit FakeFrameSource(std::atomic<int>& probe) : probe_(probe) {}

    bool open() override { return true; }

    bool read(tsys::Frame& out) override {
        probe_.fetch_add(1); // one more read the worker completed
        out = make_frame(8, 6);
        return true;
    }

    void close() override {}

private:
    std::atomic<int>& probe_;
};

// The input environment seam: capable by default; the Wayland variant resolves
// the structured unsupported refusal (probe INJECTION, per task 6.7's shape).
class FakeInputProbe final : public tsys::IInputEnvironmentProbe {
public:
    explicit FakeInputProbe(tsys::InputEnvironment environment)
        : environment_(environment) {}
    tsys::InputEnvironment probe() override {
        calls_.fetch_add(1); // the family resolved this environment at the agent
        return environment_;
    }
    int probes() const { return calls_.load(); }

private:
    tsys::InputEnvironment environment_;
    std::atomic<int> calls_{0};
};

// The key-event source: a test-thread producer feeds lines the worker polls.
// `delivered_lines` is the drain gate: what poll() already returned to the
// worker (each return is followed by the worker appending those lines to the
// capture sink synchronously, before the next waiter tick).
class FakeKeySource final : public tsys::IKeyEventSource {
public:
    bool open() override {
        opened_.store(true);
        return true;
    }

    std::vector<std::string> poll() override {
        std::lock_guard<std::mutex> guard(mutex_);
        std::vector<std::string> batch;
        batch.swap(pending_);
        delivered_lines_.fetch_add(static_cast<int>(batch.size()));
        return batch;
    }

    void close() override {}

    void emit(const std::string& line) {
        std::lock_guard<std::mutex> guard(mutex_);
        pending_.push_back(line);
    }

    int delivered_lines() const { return delivered_lines_.load(); }

private:
    std::mutex mutex_;
    std::vector<std::string> pending_;
    std::atomic<bool> opened_{false};
    std::atomic<int> delivered_lines_{0};
};

// The keyboard-guard seam (not driven by these scenarios, but the family
// wiring requires a factory; a tripwire failing loudly beats a null).
class GuardedByDefault final : public tsys::IKeyboardGuard {
public:
    void engage() override {
        throw tsys::SyscallError("execute_failed", "the e2e suite must not lock a real desktop");
    }
    void release() override {}
};

// The power-family tripwire. Every power scenario below rides a REFUSAL gate
// (the operator-surface gate, or the agent's own confirm gate in the
// dispatcher), which refuses BEFORE the family is reached - so this backend
// must never run. Wiring a loud fake instead of the real per-OS backend is a
// structural guarantee for the task's "never a real shutdown" direction: if a
// regression ever dispatched past the gate, this throws (classpath visible)
// instead of shutting the machine down.
class NeverPower final : public tsys::IPowerControl {
public:
    void shutdown() override { never("shutdown"); }
    void restart() override { never("restart"); }

private:
    static void never(const char* verb) {
        throw tsys::SyscallError("execute_failed",
                                 "the e2e suite wired a refusal-only power family; verb '" +
                                     std::string(verb) +
                                     "' must never reach a platform power backend in tests");
    }
};

// ---- the per-OS app fixtures (real .lnk / .desktop store entries) --------------

#if defined(_WIN32)

std::wstring widen_utf8(const std::string& narrow) {
    // ASCII-only fixture paths by design (see app_family_test): keeps the
    // conversion trivial and the assertions stable.
    std::wstring wide;
    for (const char c : narrow) {
        wide.push_back(static_cast<wchar_t>(c));
    }
    return wide;
}

// Writes one Start-menu shortcut through the shell API the family resolves
// with (IShellLinkW + IPersistFile::Save).
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

#endif

struct AppFixtures {
    fs::path dir;
    std::string root;
    fs::path outfile;
    fs::path touchfile;

    AppFixtures() : dir(fresh_scratch_dir("tether-e2e-apps")), root(dir.string()),
                    outfile(dir / "argv.txt"), touchfile(dir / "touched.txt") {}
};

// Writes the platform's real app-store fixtures the app verbs list and launch.
void build_app_fixtures(const AppFixtures& apps) {
    const fs::path probe = probe_executable();
    REQUIRE_MESSAGE(!probe.empty(), "tether-argv-probe binary not found next to the tests");
#if defined(_WIN32)
    const std::wstring target = probe.wstring();
    // "argv <outfile> <tail...>" observes the launched argv verbatim;
    // "alive <touchfile>" runs a bounded stop-able child for stop_app.
    REQUIRE(create_shortcut(apps.dir / L"Echo E2E.lnk", target,
                            L"argv " + widen_utf8(apps.outfile.string()) + L" e2e_sent"));
    REQUIRE(create_shortcut(apps.dir / L"Alive E2E.lnk", target,
                            L"alive " + widen_utf8(apps.touchfile.string())));
#else
    {
        std::ofstream entry(apps.dir / "echo-e2e.desktop", std::ios::binary);
        entry << "[Desktop Entry]\n";
        entry << "Type=Application\n";
        entry << "Name=Echo E2E\n";
        entry << "Exec=" << probe.string() << " argv " << apps.outfile.string()
              << " e2e_sent\n";
    }
    {
        std::ofstream entry(apps.dir / "alive-e2e.desktop", std::ios::binary);
        entry << "[Desktop Entry]\n";
        entry << "Type=Application\n";
        entry << "Name=Alive E2E\n";
        entry << "Exec=" << probe.string() << " alive " << apps.touchfile.string() << "\n";
    }
#endif
}

// ---- the loopback habitat -------------------------------------------------------

// Console/agent session timing for the suite: real wall clock, small values,
// roomy enough for the per-verb round-trips (waits between CLI calls keep
// pumping both halves, so keepalives flow).
tp::SessionConfig loopback_config() {
    tp::SessionConfig config;
    config.command_timeout = 5000;
    config.auth_timeout = 2000;
    config.keepalive_interval = 100;
    config.keepalive_window = 400;
    return config;
}

// The agent-side family wiring one scenario variant asks for: the real per-OS
// screen capture takes over the capture slot, and the environment probe can
// declare the session Wayland (the structured-unsupported input scenario).
struct AgentWiring {
    bool real_screen_capture = false;
    tsys::InputEnvironment input_environment{true, false,
                                             "injected capable desktop environment"};
};

struct Habitat {
    // Destruction (reverse of declaration) removes the scratch dir LAST: the
    // registry and its worker threads join before any file is unlinked.
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    fs::path scratch;
    Cleanup cleanup;
    std::shared_ptr<tsys::BackgroundJobRegistry> registry; // FIRST of the machinery
    std::shared_ptr<tsys::IProcessControl> processes;
    std::shared_ptr<FakeKeySource> keys;
    std::shared_ptr<FakeInputProbe> input_probe;
    std::shared_ptr<std::atomic<int>> camera_probe;
    tsys::CommandDispatcher dispatcher;
    tt::TcpConsoleTransport console;
    std::unique_ptr<tt::TcpAgentLink> agent;
    std::atomic<bool> agent_pump_run{false};
    std::thread agent_pump_worker;
    tcli::TcpOperConsole cli;
    AppFixtures apps;
    const fs::path console_config_path;
    const fs::path audit_path;

    explicit Habitat(AgentWiring wiring)
        : scratch(fresh_scratch_dir("tether-e2e-tests")),
          cleanup{scratch},
          registry(std::make_shared<tsys::BackgroundJobRegistry>()),
          processes(tsys::make_process_control()),
          keys(std::make_shared<FakeKeySource>()),
          input_probe(std::make_shared<FakeInputProbe>(wiring.input_environment)),
          camera_probe(std::make_shared<std::atomic<int>>(0)),
          dispatcher(),
          console(console_options()),
          agent(),
        // The CLI's side pump stays a no-op: the agent half pumps itself on
        // its own thread below (the production shape - separate processes,
        // independent loops; required so a large agent-side send that fills
        // the kernel socket buffer cannot starve the console's reads).
          cli(console, tt::monotonic_now_ms, [] {}),
          apps(),
          console_config_path(scratch / "console.json"),
          audit_path(scratch / "audit.jsonl") {
        // The agent dials INTO the console (the shape agent/main.cpp runs in
        // a real deployment): bind the listener first, then connect, THEN
        // wire the dispatcher onto the live session.
        e2e_step("habitat: console listener starting");
        console.start();
        REQUIRE(console.listening_port() != 0);
        e2e_step("habitat: console listening (port " +
                 std::to_string(console.listening_port()) + ")");
        e2e_step("habitat: agent link connecting");
        agent = make_agent();
        e2e_step("habitat: agent link connected; wiring the dispatcher");
        wire_dispatcher(wiring);
        e2e_step("habitat: dispatcher wired; writing console config");
        write_console_config();
        e2e_step("habitat: console config written; building app fixtures");
        build_app_fixtures(apps);
        e2e_trace("habitat up, console port " +
                  std::to_string(console.listening_port()));
    }

private:
    tt::ConsoleTransportOptions console_options() const {
        tt::ConsoleTransportOptions options;
        options.listen_address = "127.0.0.1";
        options.port = 0; // ephemeral
        options.session = loopback_config();
        options.resolve_token = [](const std::string& identity) {
            return identity == kAgentIdentity ? std::string{kAgentToken} : std::string{};
        };
        return options;
    }

    std::unique_ptr<tt::TcpAgentLink> make_agent() const {
        REQUIRE(console.listening_port() != 0);
        return tt::TcpAgentLink::connect({"127.0.0.1", console.listening_port()},
                                         {kAgentIdentity, kAgentHost}, kAgentToken,
                                         loopback_config());
    }

    // The dispatcher wiring of agent/main.cpp, with the per-host device seams
    // injected exactly where the family suites inject them.
    void wire_dispatcher(const AgentWiring& wiring) {
        // process + app share ONE process control (stop_app rides the launcher).
        dispatcher.set_process_control(processes);
        dispatcher.set_file_system(tsys::make_file_system());
        // Refusal-only power: no test path here ever reaches a real backend.
        dispatcher.set_power_control(std::make_shared<NeverPower>());
        dispatcher.set_screen_capture(wiring.real_screen_capture
                                          ? tsys::make_screen_capture()
                                          : std::shared_ptr<tsys::IScreenCapture>(
                                                std::make_shared<FakeScreenCapture>()));
        dispatcher.set_camera_capture(std::make_shared<tsys::VideoCameraCapture>(
            [probe = camera_probe]() -> std::shared_ptr<tsys::IFrameSource> {
                return std::make_shared<FakeFrameSource>(*probe);
            },
            registry, [] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }));
        // The keylogger's capture sink is the REAL production file sink; the
        // storage path is chosen per command (an agent-local temp file here).
        tsys::InputControl::Bindings bindings;
        bindings.probe = [this]() -> std::shared_ptr<tsys::IInputEnvironmentProbe> {
            return input_probe;
        };
        bindings.source = [this]() -> std::shared_ptr<tsys::IKeyEventSource> { return keys; };
        bindings.sink =
            [](const std::string& storage) -> std::shared_ptr<tsys::IKeyCaptureSink> {
            return tsys::make_file_capture_sink(storage);
        };
        bindings.guard = []() -> std::shared_ptr<tsys::IKeyboardGuard> {
            return std::make_shared<GuardedByDefault>();
        };
        dispatcher.set_input_control(std::make_shared<tsys::InputControl>(
            std::move(bindings), registry, [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }));
        dispatcher.set_app_launcher(tsys::make_app_launcher_with_roots(
            std::vector<std::string>{apps.root}, processes));
        tether::agent::install_commands(dispatcher, *agent);
    }

    // The operator's console config (the audit trail field is task 6.9's
    // additive `audit_path`; forward slashes keep the JSON portable).
    void write_console_config() const {
        std::string audit_text = audit_path.string();
        std::replace(audit_text.begin(), audit_text.end(), '\\', '/');
        std::ofstream out(console_config_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        out << "{\"listen_address\":\"127.0.0.1\",\"port\":47474,"
            << "\"token_store_path\":\"tokens.json\","
            << "\"audit_path\":\"" << audit_text << "\"}\n";
    }

public:
    // The persisted audit trail wiring, mirroring console/main.cpp exactly:
    // the invocation's config file carries `audit_path`; absent/empty stays
    // the off switch.
    tcli::AuditStoreFactory audit_factory() const {
        return [path = console_config_path](const std::string&) -> std::shared_ptr<tcli::AuditStore> {
            if (!fs::exists(path)) {
                return nullptr;
            }
            const tcom::ConsoleConfig config = tcom::load_console_config(path.string());
            if (!config.audit_path.has_value() || config.audit_path->empty()) {
                return nullptr;
            }
            return std::make_shared<tcli::AuditStore>(*config.audit_path);
        };
    }

    // A CLI argv for the tail, carrying --config so the audit hook resolves.
    std::vector<std::string> cli_argv(const std::vector<std::string>& tail) const {
        std::vector<std::string> argv{"tether-console", "--config", console_config_path.string()};
        for (const std::string& token : tail) {
            argv.push_back(token);
        }
        return argv;
    }
};

// Drives both stack halves until the handshake completes (the loopback
// pattern: sockets are real so the wait is deadline-bounds with tiny sleeps).
bool established_handshake(Habitat& habitat, tt::TransportMillis budget_ms = 1500) {
    habitat.agent->begin_handshake(tt::monotonic_now_ms());
    const tt::TransportMillis deadline = tt::monotonic_now_ms() + budget_ms;
    for (;;) {
        const tt::TransportMillis now = tt::monotonic_now_ms();
        habitat.console.pump(now);
        habitat.agent->pump(now);
        const auto session = habitat.console.session_for(kAgentIdentity);
        if (habitat.agent->established() && session &&
            session->state() == tp::SessionState::Established) {
            e2e_step("handshake established");
            return true;
        }
        if (now >= deadline) {
            e2e_step("handshake NOT established within budget");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Keeps both halves pumping between CLI calls (keepalive cadence).
void pump_both_until(Habitat& habitat, tt::TransportMillis budget_ms) {
    const tt::TransportMillis deadline = tt::monotonic_now_ms() + budget_ms;
    while (tt::monotonic_now_ms() < deadline) {
        const tt::TransportMillis now = tt::monotonic_now_ms();
        habitat.console.pump(now);
        habitat.agent->pump(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// poll with the stack kept alive: long-ish waits (child start/end) sit
// BETWEEN CLI calls, so both halves keep pumping the whole time.
bool poll_with_pump(Habitat& habitat, const std::function<bool()>& condition, int budget_ms) {
    e2e_step("poll begin (budget " + std::to_string(budget_ms) + " ms)");
    const tt::TransportMillis deadline = tt::monotonic_now_ms() + budget_ms;
    for (;;) {
        const tt::TransportMillis now = tt::monotonic_now_ms();
        habitat.console.pump(now);
        habitat.agent->pump(now);
        if (condition()) {
            e2e_step("poll ok");
            return true;
        }
        if (now >= deadline) {
            e2e_step("poll EXPIRED (condition never became true)");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

struct RunResult {
    int exit_code = -1;
    std::string out;
    std::string err;
};

// One CLI invocation against the habitat's real stack. Piped-surface shape:
// stdout_is_tty stays false (byte-exact redirect discipline), stdin_tty turns
// the interactive confirmation gate on with scripted answers.
RunResult run_verb(Habitat& habitat, const std::vector<std::string>& tail,
                   const char* stdin_bytes = "", bool stdin_tty = false) {
    std::string verb_label;
    for (const std::string& token : tail) {
        verb_label += (verb_label.empty() ? "'" : " '") + token + "'";
    }
    e2e_step("verb begin " + verb_label + (stdin_tty ? " [interactive stdin]" : ""));
    std::ostringstream out;
    std::ostringstream err;
    std::istringstream in(stdin_bytes);
    RunResult result;
    result.exit_code = tcli::run_cli(
        habitat.cli_argv(tail), tcli::CliStreams{out, err, in, false, stdin_tty},
        [&habitat](const std::string&) -> std::unique_ptr<tcli::IOperConsole> {
            return std::unique_ptr<tcli::IOperConsole>(
                new tcli::TcpOperConsole(habitat.console, tt::monotonic_now_ms,
                                         [&habitat] { habitat.agent->pump(tt::monotonic_now_ms()); }));
        },
        habitat.audit_factory());
    result.out = out.str();
    result.err = err.str();
    e2e_step("verb ended " + verb_label + " (exit " + std::to_string(result.exit_code) + ")");
    return result;
}

std::string as_text(const std::vector<unsigned char>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

std::vector<unsigned char> as_bytes(const std::string& text) {
    return std::vector<unsigned char>(text.begin(), text.end());
}

bool byte_exactly_two_status_lines(const std::string& err, const std::string& deliverable_name) {
    // Result-stream discipline for file results: exactly the two delivery
    // status lines on stderr ("status: file '<name>' ..."), never an error.
    const std::string first = "status: file '" + deliverable_name;
    if (err.rfind(first, 0) != 0) {
        return false;
    }
    if (err.find('\n') == std::string::npos) {
        return false;
    }
    if (err.find("status: file received") == std::string::npos) {
        return false;
    }
    if (err.find("error:") != std::string::npos) {
        return false;
    }
    const std::size_t lines =
        static_cast<std::size_t>(std::count(err.begin(), err.end(), '\n'));
    return lines == 2;
}

} // namespace

// ---- the family scenarios ----------------------------------------------------------

// process (real per-OS backend, platform in use): list + a benign start/stop.
TEST_CASE("process family over the CLI: list then a benign start and stop of a real child") {
    CaseTrace case_trace("process-family-over-cli");
    silence_logging();
    Habitat habitat{AgentWiring{}};
    REQUIRE(established_handshake(habitat));

    // The operator surface finds the agent through the real registry.
    RunResult listing = run_verb(habitat, {"agents"});
    REQUIRE(listing.exit_code == tcli::kExitOk);
    CHECK(listing.out == std::string(kAgentIdentity) + " live\n");
    CHECK(listing.err.empty()); // text-result verbs keep stderr clean (5.5 discipline)

    // list: the real toolhelp//proc snapshot, streamed to stdout only.
    RunResult listed = run_verb(habitat, {"exec", kAgentIdentity, "list"});
    REQUIRE(listed.exit_code == tcli::kExitOk);
    CHECK(listed.err.empty());
    REQUIRE_FALSE(listed.out.empty());
    // every listing row is "<pid> <name>" (a leading digit row per line)
    std::string first_row;
    {
        std::istringstream rows(listed.out);
        std::getline(rows, first_row);
        REQUIRE_FALSE(first_row.empty());
    }
    CHECK(first_row.find(' ') != std::string::npos);
    const bool leading_digit = first_row[0] >= '0' && first_row[0] <= '9';
    CHECK(leading_digit);

    // start: a benign probe child rides the real argv-based launcher.
    const fs::path touch = habitat.scratch / "proc-touch.txt";
    const fs::path probe = probe_executable();
    REQUIRE_MESSAGE(!probe.empty(), "probe binary missing next to the tests");
    RunResult started = run_verb(
        habitat, {"exec", kAgentIdentity, "start", probe.string(), "alive", touch.string()});
    REQUIRE(started.exit_code == tcli::kExitOk);
    CHECK(started.err.empty());
    REQUIRE(started.out.rfind("started pid ", 0) == 0);
    const std::uint32_t child_pid = pid_from_row(started.out);
    REQUIRE(poll_with_pump(habitat,
                           [&] {
                               std::error_code ec;
                               return fs::exists(touch, ec) && !ec;
                           },
                           15000));

    // stop: the real signal/terminate path resolves to "stopped pid N".
    RunResult stopped = run_verb(habitat, {"exec", kAgentIdentity, "stop", std::to_string(child_pid)});
    REQUIRE(stopped.exit_code == tcli::kExitOk);
    CHECK(stopped.err.empty());
    CHECK(stopped.out == "stopped pid " + std::to_string(child_pid) + "\n");
    REQUIRE(poll_with_pump(habitat, [&] { return child_gone(child_pid); }, 15000));

    // the same session keeps serving (the family is live end to end)
    RunResult served = run_verb(habitat, {"exec", kAgentIdentity, "list"});
    CHECK(served.exit_code == tcli::kExitOk);

    pump_both_until(habitat, 40);
}

// file (real std::filesystem): list + the confirmed write path in a temp dir +
// the unconfirmed delete refusals at BOTH gates.
TEST_CASE("file family over the CLI: list, confirmed delete removes the file, unconfirmed refuses") {
    CaseTrace case_trace("file-family-over-cli");
    silence_logging();
    Habitat habitat{AgentWiring{}};
    REQUIRE(established_handshake(habitat));

    const fs::path home = habitat.scratch / "home";
    fs::create_directories(home);
    const fs::path victim = home / "victim.txt";
    const fs::path keeper = home / "keeper.txt";
    write_bytes(victim, "delete me\n");
    write_bytes(keeper, "stay\n");
    write_bytes(home / "met;achars.txt", "literal data\n");

    // list: the entries of the temp dir, the metacharacter name included -
    // the parameters-are-data scenario at the CLI level (a path that a shell
    // would have split is looked up LITERALLY).
    RunResult listed = run_verb(habitat, {"exec", kAgentIdentity, "ls", home.string()});
    REQUIRE(listed.exit_code == tcli::kExitOk);
    CHECK(listed.err.empty());
    CHECK(listed.out.find("victim.txt") != std::string::npos);
    CHECK(listed.out.find("keeper.txt") != std::string::npos);
    CHECK(listed.out.find("met;achars.txt") != std::string::npos);

    // unconfirmed, non-terminal stdin: the OPERATOR gate refuses before any
    // dispatch - nothing was sent, the file survives (spec: destructive verbs
    // "are refused at the console before dispatch").
    RunResult refused = run_verb(habitat, {"exec", kAgentIdentity, "delete", victim.string()});
    REQUIRE(refused.exit_code == tcli::kExitRefused);
    CHECK(refused.out.empty());
    CHECK(refused.err.rfind("error: refusal: 'delete'", 0) == 0);
    CHECK(refused.err.find("nothing was sent") != std::string::npos);
    CHECK(fs::exists(victim));

    // operator-confirmed (--yes) yet no agent confirm token: the dispatch
    // happens, and the AGENT's own destructive gate refuses it before the
    // family (remote-commands: "the agent refuses to run it").
    RunResult agent_refused =
        run_verb(habitat, {"exec", kAgentIdentity, "delete", victim.string(), "--yes"});
    REQUIRE(agent_refused.exit_code == tcli::kExitFailure);
    CHECK(agent_refused.err.rfind("error: command failed: missing_confirmation", 0) == 0);
    CHECK(agent_refused.err.find("confirm") != std::string::npos);
    CHECK(agent_refused.out.empty());
    CHECK(fs::exists(victim));

    // confirmed write path: operator confirmation (scripted dialogue) AND the
    // agent token as the first argument - the file is really gone.
    RunResult confirmed =
        run_verb(habitat, {"exec", kAgentIdentity, "delete", tsys::kConfirmMarker, victim.string()},
                 "y\n", true);
    REQUIRE(confirmed.exit_code == tcli::kExitOk);
    CHECK(confirmed.out == "deleted " + victim.string() + "\n");
    // stderr carries the scripted confirmation banner (nothing else) and the
    // file channel then delivered the result untouched.
    CHECK(confirmed.err.rfind("status: 'delete' targets '" + std::string(kAgentIdentity),
                              0) == 0);
    CHECK(confirmed.err.find("confirm? [y/N]") != std::string::npos);
    CHECK_FALSE(fs::exists(victim));
    CHECK(fs::exists(keeper)); // the unconfirmed target was never touched

    // the follow-up command on the SAME session proves refusals never wedge it.
    RunResult after = run_verb(habitat, {"exec", kAgentIdentity, "ls", home.string()});
    REQUIRE(after.exit_code == tcli::kExitOk);
    CHECK(after.out.find("victim") == std::string::npos);
    CHECK(after.out.find("keeper.txt") != std::string::npos);

    pump_both_until(habitat, 40);
}

// power: REFUSAL PATHS ONLY - never a real shutdown/restart on this machine.
TEST_CASE("power family over the CLI: refusals only, nothing ever reaches a power backend") {
    CaseTrace case_trace("power-family-over-cli");
    silence_logging();
    Habitat habitat{AgentWiring{}};
    REQUIRE(established_handshake(habitat));

    // (1) no confirmation: refused at the console BEFORE dispatch (nothing sent).
    RunResult refused = run_verb(habitat, {"exec", kAgentIdentity, "shutdown"});
    REQUIRE(refused.exit_code == tcli::kExitRefused);
    CHECK(refused.out.empty());
    CHECK(refused.err.rfind("error: refusal: 'shutdown'", 0) == 0);
    CHECK(refused.err.find("nothing was sent") != std::string::npos);

    // (2) the interactive gate with a scripted "n": the same local refusal.
    RunResult scripted_no = run_verb(habitat, {"exec", kAgentIdentity, "restart"}, "n\n", true);
    REQUIRE(scripted_no.exit_code == tcli::kExitRefused);
    CHECK(scripted_no.out.empty());
    CHECK(scripted_no.err.find("error: refusal: 'restart'") != std::string::npos);

    // (3) operator-confirmed: the dispatch happens and the agent's own confirm
    // gate refuses before the family (the spec's agent-side gate; the tripwire
    // fake would fail loudly if the family were ever reached - the machine
    // itself never sees a transition in this suite).
    RunResult agent_refused = run_verb(habitat, {"exec", kAgentIdentity, "shutdown", "--yes"});
    REQUIRE(agent_refused.exit_code == tcli::kExitFailure);
    CHECK(agent_refused.err.rfind("error: command failed: missing_confirmation", 0) == 0);
    CHECK(agent_refused.out.empty());

    // every refusal left the session serving normally.
    RunResult served = run_verb(habitat, {"exec", kAgentIdentity, "list"});
    CHECK(served.exit_code == tcli::kExitOk);

    pump_both_until(habitat, 40);
}

// capture: the fake source's screenshot rides the file channel byte-exact
// (delivered capture), plus the REAL per-OS grab smoke on this platform.
TEST_CASE("capture family over the CLI: the fake capture delivers byte-exact over the wire") {
    CaseTrace case_trace("capture-fake-over-cli");
    silence_logging();
    Habitat habitat{AgentWiring{}}; // fake screen source
    REQUIRE(established_handshake(habitat));

    RunResult grab = run_verb(habitat, {"exec", kAgentIdentity, "screenshot"});
    REQUIRE(grab.exit_code == tcli::kExitOk);

    // stdout = the capture bytes, byte-exact (FILE_META + FILE_CHUNK + FILE_END
    // -> stdout; the status lines ride stderr).
    const std::string expected = as_text(tsys::encode_png_bgr(make_frame(9, 7)));
    CHECK(grab.out == expected);
    CHECK(byte_exactly_two_status_lines(grab.err, "screenshot.png"));

    // the delivered bytes are a usable file of the format (decode proof).
    const std::vector<unsigned char> png = as_bytes(grab.out);
    CHECK(tsys::looks_like_png(png));
    const tsys::Frame decoded = tsys::decode_png(png);
    CHECK(decoded.width == 9);
    CHECK(decoded.height == 7);

    // background-job event machinery must NOT fire for a non-job verb.
    const std::vector<tcli::AuditRecord> records = tcli::load_audit_jsonl(habitat.audit_path.string());
    REQUIRE(records.size() == 1);
    CHECK(records.front().record_type == "command");

    pump_both_until(habitat, 40);
}

TEST_CASE("capture family on the platform in use: the REAL per-OS grab produces a decodable image") {
    CaseTrace case_trace("capture-real-grab");
    silence_logging();
    AgentWiring wiring{}; // the REAL per-OS grab below the CLI: GDI / X11, per compile
    wiring.real_screen_capture = true;
    Habitat habitat{wiring};
    REQUIRE(established_handshake(habitat));

    RunResult grab = run_verb(habitat, {"exec", kAgentIdentity, "screenshot"});
    if (grab.exit_code == tcli::kExitOk) {
        CHECK(grab.err.find("error:") == std::string::npos);
        const std::vector<unsigned char> png = as_bytes(grab.out);
        REQUIRE(tsys::looks_like_png(png));
        const tsys::Frame decoded = tsys::decode_png(png);
        CHECK(decoded.width > 1);
        CHECK(decoded.height > 1);
        CHECK(decoded.bgr.size() ==
              static_cast<std::size_t>(decoded.width) * decoded.height * 3);
        INFO("real grab: " << decoded.width << "x" << decoded.height);
    } else {
        // A genuinely headless host is the documented degradation: the
        // classified refusal naming the missing desktop resource - never a
        // wedge; the session keeps serving.
        REQUIRE(grab.exit_code == tcli::kExitFailure);
        CHECK(grab.err.find("execute_failed") != std::string::npos);
        RunResult served = run_verb(habitat, {"exec", kAgentIdentity, "list"});
        CHECK(served.exit_code == tcli::kExitOk);
    }

    pump_both_until(habitat, 40);
}

// camera: the managed background job over the fake device - start, the
// duplicate-start refusal, and the stop that delivers the accumulated capture.
TEST_CASE("camera family over the CLI: start, duplicate-refuse, stop-and-deliver") {
    CaseTrace case_trace("camera-over-cli");
    silence_logging();
    Habitat habitat{AgentWiring{}}; // fake camera device
    REQUIRE(established_handshake(habitat));

    habitat.camera_probe->store(0);

    RunResult started = run_verb(habitat, {"exec", kAgentIdentity, "start_webcam"});
    REQUIRE(started.exit_code == tcli::kExitOk);
    CHECK(started.err.empty());
    CHECK(started.out.rfind("camera job started\n", 0) == 0);

    // The job keeps capturing in the background while the session serves a
    // further command (the duplicate goes out next; both are lifecycle
    // commands on the same live session).
    REQUIRE(poll_with_pump(habitat, [&] { return habitat.camera_probe->load() >= 3; }, 2000));

    // duplicate start: structured refusal, incumbent untouched.
    RunResult duplicated = run_verb(habitat, {"exec", kAgentIdentity, "start_webcam"});
    REQUIRE(duplicated.exit_code == tcli::kExitFailure);
    CHECK(duplicated.out.empty());
    CHECK(duplicated.err.rfind("error: command failed: already_running", 0) == 0);
    CHECK(habitat.registry->running(tsys::BackgroundJobRegistry::kCameraSlot));

    // further (non-job) commands are ALSO served while the job keeps running.
    RunResult served = run_verb(habitat, {"exec", kAgentIdentity, "list"});
    REQUIRE(served.exit_code == tcli::kExitOk);
    REQUIRE(poll_with_pump(habitat, [&] { return habitat.camera_probe->load() >= 12; }, 2000));

    // stop: the accumulated capture is delivered byte-exact as a decodable PNG.
    RunResult stopped = run_verb(habitat, {"exec", kAgentIdentity, "stop_webcam"});
    REQUIRE(stopped.exit_code == tcli::kExitOk);
    const std::string expected = as_text(tsys::encode_png_bgr(make_frame(8, 6)));
    CHECK(stopped.out == expected);
    CHECK(byte_exactly_two_status_lines(stopped.err, "webcam.png"));
    const tsys::Frame decoded = tsys::decode_png(as_bytes(stopped.out));
    CHECK(frames_equal(decoded, make_frame(8, 6)));
    CHECK_FALSE(habitat.registry->running(tsys::BackgroundJobRegistry::kCameraSlot));

    // nothing running: the not-running refusal, session still fine.
    RunResult stop_empty = run_verb(habitat, {"exec", kAgentIdentity, "stop_webcam"});
    REQUIRE(stop_empty.exit_code == tcli::kExitFailure);
    CHECK(stop_empty.err.rfind("error: command failed: not_running", 0) == 0);
    RunResult still_serving = run_verb(habitat, {"exec", kAgentIdentity, "list"});
    CHECK(still_serving.exit_code == tcli::kExitOk);

    pump_both_until(habitat, 40);
}

// input: the keylogger job through the REAL file sink into a temp storage,
// stop-delivered events, and the Wayland-structured refusal via probe injection.
TEST_CASE("input family over the CLI: keylogger job start/stop with temp storage and delivered events") {
    CaseTrace case_trace("input-keylogger-over-cli");
    silence_logging();
    Habitat habitat{AgentWiring{}}; // capable environment
    REQUIRE(established_handshake(habitat));

    const fs::path storage = habitat.scratch / "keys-e2e.log";
    RunResult started =
        run_verb(habitat, {"exec", kAgentIdentity, "start_keylogger", storage.string()});
    REQUIRE(started.exit_code == tcli::kExitOk);
    CHECK(started.err.empty());
    CHECK(started.out.rfind("keylogger job started\n", 0) == 0);
    CHECK(habitat.registry->running(tsys::BackgroundJobRegistry::kInputSlot));

    // The operator "types"; the background worker accumulates through the
    // production file sink into the temp storage (the drain gate says the
    // worker has pulled both lines out of the source).
    habitat.keys->emit("keydown e");
    habitat.keys->emit("keyup e");
    REQUIRE(poll_with_pump(habitat, [&] { return habitat.keys->delivered_lines() >= 2; }, 2000));

    // a further command is served WHILE the job runs (session not blocked).
    RunResult served = run_verb(habitat, {"exec", kAgentIdentity, "list"});
    CHECK(served.exit_code == tcli::kExitOk);

    // stop: the accumulated events are the delivered file result, byte-exact.
    RunResult stopped = run_verb(habitat, {"exec", kAgentIdentity, "stop_keylogger"});
    REQUIRE(stopped.exit_code == tcli::kExitOk);
    CHECK(stopped.out == "keydown e\nkeyup e\n");
    CHECK(byte_exactly_two_status_lines(stopped.err, "keys-e2e.log"));
    CHECK_FALSE(habitat.registry->running(tsys::BackgroundJobRegistry::kInputSlot));

    // the configurable temp storage carried exactly the delivered capture
    // (the storage file IS the accumulated capture: same content).
    CHECK(file_text(storage) == "keydown e\nkeyup e\n");

    // nothing running: the structured refusal; the session keeps serving.
    RunResult stop_empty = run_verb(habitat, {"exec", kAgentIdentity, "stop_keylogger"});
    REQUIRE(stop_empty.exit_code == tcli::kExitFailure);
    CHECK(stop_empty.err.rfind("error: command failed: not_running", 0) == 0);
    RunResult follow_up = run_verb(habitat, {"exec", kAgentIdentity, "list"});
    CHECK(follow_up.exit_code == tcli::kExitOk);

    pump_both_until(habitat, 40);
}

TEST_CASE("input family over the CLI: a Wayland environment refuses structurally and never wedges") {
    CaseTrace case_trace("input-wayland-refusal");
    silence_logging();
    AgentWiring wiring{}; // only the environment probe differs from the defaults
    wiring.input_environment =
        tsys::InputEnvironment{false, true, "the session reports wayland via XDG_SESSION_TYPE"};
    Habitat habitat{wiring};
    REQUIRE(established_handshake(habitat));

    // The probe injection resolves the family's unsupported resolution at the
    // AGENT: a structured refusal over the wire, the slot left untouched.
    RunResult refused_start =
        run_verb(habitat, {"exec", kAgentIdentity, "start_keylogger", "keys.log"});
    REQUIRE(refused_start.exit_code == tcli::kExitFailure);
    CHECK(refused_start.out.empty());
    // the console's refusal line: the agent's coded refusal rendered on the
    // CLI surface (code + the console's command-ended rendering).
    CHECK(refused_start.err.rfind("error: command failed: unsupported_platform", 0) == 0);
    // the resolution happened AT THE AGENT: the injected probe was consulted,
    // its note drove the family's structured refusal, and no job was started.
    REQUIRE(habitat.input_probe->probes() >= 1);
    CHECK_FALSE(habitat.registry->running(tsys::BackgroundJobRegistry::kInputSlot));

    // The keyboard-lock verb resolves the same structured way.
    RunResult refused_lock = run_verb(habitat, {"exec", kAgentIdentity, "lock_keyboard"});
    REQUIRE(refused_lock.exit_code == tcli::kExitFailure);
    CHECK(refused_lock.err.find("unsupported_platform") != std::string::npos);

    // A refusal NEVER wedges the session: the very next command succeeds.
    RunResult served = run_verb(habitat, {"exec", kAgentIdentity, "list"});
    REQUIRE(served.exit_code == tcli::kExitOk);
    CHECK(served.err.empty());

    pump_both_until(habitat, 40);
}

// app: real per-OS store fixtures - list, a craft-launch observed verbatim,
// and the stop that ends the child.
TEST_CASE("app family over the CLI: list, craft-launch of a probe child, stop") {
    CaseTrace case_trace("app-over-cli");
    silence_logging();
    Habitat habitat{AgentWiring{}}; // the habitat built the per-OS fixtures
    REQUIRE(established_handshake(habitat));

    // list: both fixture entries, sorted by name; paths included.
    RunResult listed = run_verb(habitat, {"exec", kAgentIdentity, "list_app"});
    REQUIRE(listed.exit_code == tcli::kExitOk);
    CHECK(listed.err.empty());
    const std::size_t alive_row = listed.out.find("Alive E2E");
    const std::size_t echo_row = listed.out.find("Echo E2E");
    REQUIRE(alive_row != std::string::npos);
    REQUIRE(echo_row != std::string::npos);
    CHECK(alive_row < echo_row); // sorted order

    // launch: the craft-launch child observes its data argument verbatim -
    // the launch goes through the same argv-based path the process family uses.
    std::error_code ec;
    fs::remove(habitat.apps.outfile, ec);
    RunResult launched = run_verb(habitat, {"exec", kAgentIdentity, "start_app", "Echo E2E"});
    REQUIRE(launched.exit_code == tcli::kExitOk);
    CHECK(launched.out.rfind("started pid ", 0) == 0);
    REQUIRE(poll_with_pump(habitat,
                           [&] {
                               std::error_code inner;
                               return fs::exists(habitat.apps.outfile, inner) && !inner;
                           },
                           15000));
    {
        const std::vector<std::string> lines = read_lines(habitat.apps.outfile);
        REQUIRE(lines.size() >= 2); // "argv0" header + the data arg
        CHECK(lines[1] == "e2e_sent");
    }

    // launch again in the bounded-alive mode, then stop_by pid via stop_app.
    fs::remove(habitat.apps.touchfile, ec);
    RunResult alive = run_verb(habitat, {"exec", kAgentIdentity, "start_app", "Alive E2E"});
    REQUIRE(alive.exit_code == tcli::kExitOk);
    const std::uint32_t alive_pid = pid_from_row(alive.out);
    REQUIRE(poll_with_pump(habitat,
                           [&] {
                               std::error_code inner;
                               return fs::exists(habitat.apps.touchfile, inner) && !inner;
                           },
                           15000));

    RunResult stopped =
        run_verb(habitat, {"exec", kAgentIdentity, "stop_app", std::to_string(alive_pid)});
    REQUIRE(stopped.exit_code == tcli::kExitOk);
    CHECK(stopped.out ==
          "stopped pid " + std::to_string(alive_pid) + "\n");
    REQUIRE(poll_with_pump(habitat, [&] { return child_gone(alive_pid); }, 15000));

    // unknown names refuse with the structured not-found outcome; the
    // session keeps serving.
    RunResult unknown = run_verb(habitat, {"exec", kAgentIdentity, "start_app", "Named No Such"});
    REQUIRE(unknown.exit_code == tcli::kExitFailure);
    CHECK(unknown.err.rfind("error: command failed: not_found", 0) == 0);
    RunResult follow_up = run_verb(habitat, {"exec", kAgentIdentity, "list_app"});
    CHECK(follow_up.exit_code == tcli::kExitOk);

    pump_both_until(habitat, 40);
}

// audit: THE E2E RUN's trail - executed / refused / failed command records
// plus background-job lifecycle events, persisted JSONL with required fields.
TEST_CASE("the gauntlet leaves an audit trail spanning executed, refused, failed and job events") {
    CaseTrace case_trace("audit-gauntlet");
    silence_logging();
    Habitat habitat{AgentWiring{}};
    REQUIRE(established_handshake(habitat));

    // executed: a real command outcome.
    CHECK(run_verb(habitat, {"exec", kAgentIdentity, "list"}).exit_code == tcli::kExitOk);

    // refused at the OPERATOR surface (nothing dispatched: seq 0).
    CHECK(run_verb(habitat, {"exec", kAgentIdentity, "shutdown"}).exit_code == tcli::kExitRefused);

    // refused at the AGENT layer (dispatched: a nonzero seq) - the unknown verb.
    CHECK(run_verb(habitat, {"exec", kAgentIdentity, "not_a_real_verb"}).exit_code ==
          tcli::kExitFailure);

    // failed: a real classified failure (a pid nothing matches).
    CHECK(run_verb(habitat, {"exec", kAgentIdentity, "stop", "999999"}).exit_code ==
          tcli::kExitFailure);

    // background job lifecycle: the camera job start + stop (delivered capture).
    habitat.camera_probe->store(0);
    CHECK(run_verb(habitat, {"exec", kAgentIdentity, "start_webcam"}).exit_code == tcli::kExitOk);
    REQUIRE(poll_with_pump(habitat, [&] { return habitat.camera_probe->load() >= 3; }, 2000));
    CHECK(run_verb(habitat, {"exec", kAgentIdentity, "stop_webcam"}).exit_code == tcli::kExitOk);

    // and the keylogger job start + stop (delivered events from temp storage).
    RunResult keylog_start =
        run_verb(habitat, {"exec", kAgentIdentity, "start_keylogger",
                           (habitat.scratch / "keys-audit.log").string()});
    CHECK(keylog_start.exit_code == tcli::kExitOk);
    habitat.keys->emit("keydown a");
    REQUIRE(poll_with_pump(habitat, [&] { return habitat.keys->delivered_lines() >= 1; }, 2000));
    CHECK(run_verb(habitat, {"exec", kAgentIdentity, "stop_keylogger"}).exit_code == tcli::kExitOk);

    pump_both_until(habitat, 40);

    // The persisted JSONL: the strict loader re-parses every line (all fields
    // present with the right types), and the classes span the run.
    const std::vector<tcli::AuditRecord> records =
        tcli::load_audit_jsonl(habitat.audit_path.string());
    REQUIRE(records.size() >= 7);

    bool saw_executed = false;
    bool saw_refused_local = false;
    bool saw_refused_agent = false;
    bool saw_failed = false;
    bool saw_job_started = false;
    bool saw_job_stopped = false;
    for (const tcli::AuditRecord& record : records) {
        // the six required fields every record carries
        CHECK(record.agent == kAgentIdentity);
        CHECK_FALSE(record.command.empty());
        CHECK_FALSE(record.outcome.empty());
        CHECK_FALSE(record.time.empty());
        CHECK(record.time.find('T') != std::string::npos);
        CHECK(record.time.back() == 'Z'); // ISO-8601 UTC rendering
        CHECK(record.time_epoch_ms > 0);
        const bool known_record_type =
            record.record_type == "command" || record.record_type == "job_event";
        CHECK(known_record_type);

        if (record.outcome == "executed") {
            saw_executed = true;
        }
        if (record.outcome == "refused") {
            if (record.seq == 0) {
                saw_refused_local = true;
                CHECK(record.detail.find("not confirmed") != std::string::npos);
            } else {
                saw_refused_agent = true;
                CHECK(record.detail.find("unknown_command") != std::string::npos);
            }
        }
        if (record.outcome == "failed") {
            saw_failed = true;
            CHECK(record.detail.find("not_found") != std::string::npos);
        }
        if (record.record_type == "job_event") {
            CHECK_FALSE(record.job.empty());
            CHECK_FALSE(record.event.empty());
            // job events reference their initiating/finalizing commands,
            // preserving the verb identity that produced the lifecycle event.
            CHECK(record.seq != 0);
            const bool event_verb_matches =
                (record.event == "started"
                 && (record.command == "start_webcam" || record.command == "start_keylogger"))
                || (record.event == "stopped"
                    && (record.command == "stop_webcam" || record.command == "stop_keylogger"));
            CHECK(event_verb_matches);
            if (record.event == "started") {
                saw_job_started = true;
                const bool known_job = record.job == "camera" || record.job == "input";
                CHECK(known_job);
            }
            if (record.event == "stopped") {
                saw_job_stopped = true;
                CHECK_FALSE(record.delivered.empty());
            }
        }
    }
    CHECK(saw_executed);
    CHECK(saw_refused_local);
    CHECK(saw_refused_agent);
    CHECK(saw_failed);
    CHECK(saw_job_started);
    CHECK(saw_job_stopped);

    // the operator's history re-opens queryable (fixture access).
    tcli::AuditStore reopened(habitat.audit_path.string());
    CHECK_FALSE(reopened.query({}).empty());
    tcli::AuditQuery events;
    events.record_type = std::string("job_event");
    CHECK(reopened.query(events).size() >= 2);
    tcli::AuditQuery refusals;
    refusals.outcome = std::string("refused");
    CHECK_FALSE(reopened.query(refusals).empty());
    tcli::AuditQuery failures;
    failures.outcome = std::string("failed");
    CHECK_FALSE(reopened.query(failures).empty());
}

// the unknown-command refusal path also rides the CLI over the wire: a
// structured refusal from the closed set that leaves the session usable.
TEST_CASE("an unknown command over the CLI is refused harmlessly and the session keeps serving") {
    CaseTrace case_trace("unknown-command-refusal");
    silence_logging();
    Habitat habitat{AgentWiring{}};
    REQUIRE(established_handshake(habitat));

    RunResult refused =
        run_verb(habitat, {"exec", kAgentIdentity, "definitely_not_a_verb", "with", "args"});
    REQUIRE(refused.exit_code == tcli::kExitFailure);
    CHECK(refused.out.empty());
    // the AGENT's closed-set refusal, rendered on the CLI surface: the coded
    // refusal line with the console's command-ended note.
    CHECK(refused.err.rfind("error: command failed: unknown_command", 0) == 0);
    CHECK(refused.err.find("command ended: unknown_command") != std::string::npos);

    // the very next command on the SAME session executes normally.
    RunResult served = run_verb(habitat, {"exec", kAgentIdentity, "list"});
    REQUIRE(served.exit_code == tcli::kExitOk);
    CHECK(served.err.empty());

    pump_both_until(habitat, 40);
}
