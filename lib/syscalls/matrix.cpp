// lib/syscalls/matrix.cpp - the matrix data and the dispatch layer's
// implementation (task 6.1), plus the family glue for the two families
// implemented in this build: process (task 6.2) and file (task 6.3). The
// glue owns only argument-shape validation and result formatting; every
// system effect goes through the family interface as typed data.
#include "matrix.hpp"

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
    // power family (task 6.4): implementation lands later; the matrix rows
    // exist now so shutdown/restart resolve to the structured unsupported
    // result instead of wedging or crashing.
    {"shutdown",        Family::Power,   true,  true, true},
    {"restart",         Family::Power,   true,  true, true},
    // capture family (task 6.5): GDI on Windows, X11 grab on Linux; Wayland
    // returns the structured unsupported result through the same rows.
    {"screenshot",      Family::Capture, false, true, true},
    // camera family (task 6.6): managed background jobs, both OSes.
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

bool CommandDispatcher::family_available(Family family) const {
    switch (family) {
        case Family::Process: return process_ != nullptr;
        case Family::File: return file_ != nullptr;
        // The remaining families are wired by their OWNING tasks (6.4-6.8);
        // no implementation exists in this build yet, so their verbs get the
        // structured unsupported resolution at step 3 of dispatch().
        default: return false;
    }
}

Outcome CommandDispatcher::dispatch(const std::string& verb,
                                    const std::vector<std::string>& args) const try {
    // 1. The closed set: rejection happens here, BEFORE any capability is
    // touched, so an unknown verb has no system effect by construction. The
    // structured error names the offending verb (remote-commands delta).
    const CommandSpec* spec = find_command(verb);
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
        // Unreachable: steps 2-3 already resolved every other family to the
        // structured unsupported result, and the table contains no other
        // families. The catch-all below still guarantees classification.
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
