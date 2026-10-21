// lib/syscalls/process.cpp - the process family per-OS implementations
// (design D6, task 6.2):
//
//   Linux  - list walks /proc/<pid>/ (cmdline argv[0] basename, comm
//            fallback); start is fork+execvp with the full argv handed to
//            exec as data (never /bin/sh); stop sends SIGTERM with a bounded
//            escalation to SIGKILL; stderr-style errors become
//            SyscallError outcomes.
//   Windows- list snapshots processes through Toolhelp32; start is
//            CreateProcessW whose command line is built by quoting each argv
//            element individually (parameters stay data: no shell ever
//            parses them); stop OpenProcess + TerminateProcess.
//
// Both build branches are in this TU behind preprocessor gates so the family
// ships as one interface with per-OS impls, exactly as directed.
#include "process.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>

#include <filesystem>
#include <fstream>
#endif

#include <algorithm>
#include <string>
#include <vector>

namespace tether::syscalls {

#if defined(_WIN32)

namespace {

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                         static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(),
                        size, nullptr, nullptr);
    utf8.resize(static_cast<std::size_t>(size));
    return utf8;
}

std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(),
                        size);
    wide.resize(static_cast<std::size_t>(size));
    return wide;
}

// Turns one argv element into its CreateProcess command-line contribution
// with the Microsoft C runtime's own argv rules, so the child's runtime
// splits back EXACTLY the elements that arrived here (backslash runs before
// quotes double, quotes escape, and tokens with whitespace/empties get
// wrapped). This is the data-not-syntax guarantee: the metacharacters of a
// shell never appear in this algorithm because no shell is involved.
void append_quoted_argument(const std::string& argument, std::string& out) {
    const bool needs_quotes =
        argument.empty() ||
        argument.find_first_of(" \t\"") != std::string::npos;
    if (!needs_quotes) {
        out += argument;
        return;
    }
    out.push_back('"');
    std::size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // A quote: every backslash in front of it must be doubled, plus
            // one more escaping the quote itself.
            out.append(backslashes * 2 + 1, '\\');
            backslashes = 0;
            out.push_back('"');
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out.push_back(c);
    }
    // Closing quote: trailing backslashes double so they cannot escape it.
    out.append(backslashes * 2, '\\');
    out.push_back('"');
}

std::string quote_command_line(const std::vector<std::string>& argv) {
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        append_quoted_argument(argv[i], out);
    }
    return out;
}

} // namespace

class WindowsProcessControl final : public IProcessControl {
public:
    std::vector<ProcessInfo> list() override {
        // Native snapshot path (Win32 Toolhelp) - replaces v1's
        // `_popen("tasklist /FO CSV")` shell round-trip with a direct API.
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            throw SyscallError("execute_failed", "process snapshot failed (GetLastError()=" +
                                                     std::to_string(GetLastError()) + ")");
        }
        std::vector<ProcessInfo> processes;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID != 0) { // pid 0 is the System Idle anchor, not a process
                    processes.push_back(ProcessInfo{entry.th32ProcessID,
                                                    wide_to_utf8(entry.szExeFile)});
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return processes;
    }

    std::uint32_t start(const std::vector<std::string>& argv) override {
        if (argv.empty()) {
            throw SyscallError("invalid_args", "start needs argv with at least a program name");
        }
        // CreateProcessW's command line must be a mutable buffer; the wide
        // copy below IS the mutation target (argv arrives as utf-8 data and
        // leaves as data - the first element is also the launched program).
        const std::wstring command_line = utf8_to_wide(quote_command_line(argv));
        std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
        buffer.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &startup, &process)) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                throw SyscallError("not_found", "program not found: '" + argv[0] + "'");
            }
            if (error == ERROR_ACCESS_DENIED) {
                throw SyscallError("access_denied", "access denied launching '" + argv[0] + "'");
            }
            throw SyscallError("execute_failed",
                               "CreateProcessW failed for '" + argv[0] +
                                   "' (GetLastError()=" + std::to_string(error) + ")");
        }
        const std::uint32_t pid = process.dwProcessId;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess); // the child runs detached; callers go by pid
        return pid;
    }

    void stop(std::uint32_t pid) override {
        if (pid == 0) {
            throw SyscallError("invalid_args", "pid 0 cannot be signalled");
        }
        const HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (handle == nullptr) {
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER || error == ERROR_FILE_NOT_FOUND ||
                error == ERROR_PATH_NOT_FOUND) {
                throw SyscallError("not_found", "no process with pid " + std::to_string(pid));
            }
            if (error == ERROR_ACCESS_DENIED) {
                throw SyscallError("access_denied",
                                   "access denied stopping pid " + std::to_string(pid));
            }
            throw SyscallError("execute_failed", "OpenProcess failed for pid " +
                                                     std::to_string(pid) +
                                                     " (GetLastError()=" + std::to_string(error) +
                                                     ")");
        }
        // Mark-for-termination (the v1 `taskkill /F` path, now a direct API):
        const BOOL terminated = TerminateProcess(handle, 1);
        const DWORD error = GetLastError();
        CloseHandle(handle);
        if (!terminated) {
            if (error == ERROR_ACCESS_DENIED) {
                throw SyscallError("access_denied", "access denied stopping pid " +
                                                        std::to_string(pid));
            }
            throw SyscallError("execute_failed",
                               "TerminateProcess failed for pid " + std::to_string(pid) +
                                   " (GetLastError()=" + std::to_string(error) + ")");
        }
    }
};

std::shared_ptr<IProcessControl> make_process_control() {
    return std::make_shared<WindowsProcessControl>();
}

#else // Linux

namespace {

// SIGCHLD auto-reap: `start` children run detached and are never joined by
// this library, so without it they would sit as zombies in the long-lived
// agent process. Install once, lazily at construction.
void ensure_children_are_autoreaped() {
    static const bool installed = [] {
        struct sigaction ignore_children {};
        ignore_children.sa_handler = SIG_IGN;
        sigemptyset(&ignore_children.sa_mask);
        ignore_children.sa_flags = 0;
        return ::sigaction(SIGCHLD, &ignore_children, nullptr) == 0;
    }();
    (void)installed;
}

// The argv[0] base name, falling back to the comm name (kernel threads and
// zombies have an empty cmdline but a live comm entry).
std::string read_process_name(std::uint32_t pid) {
    const std::string proc = "/proc/" + std::to_string(pid);
    {
        std::ifstream cmdline(proc + "/cmdline", std::ios::binary);
        if (cmdline) {
            std::string raw((std::istreambuf_iterator<char>(cmdline)),
                            std::istreambuf_iterator<char>());
            std::size_t nul = raw.find('\0');
            if (nul == std::string::npos) {
                nul = raw.size();
            }
            std::string argv0 = raw.substr(0, nul);
            std::size_t slash = argv0.rfind('/');
            if (slash != std::string::npos) {
                argv0 = argv0.substr(slash + 1); // basename: stop-by-name matches it
            }
            if (!argv0.empty()) {
                return argv0;
            }
        }
    }
    std::ifstream comm(proc + "/comm", std::ios::binary);
    if (comm) {
        std::string name;
        std::getline(comm, name);
        if (!name.empty()) {
            return name;
        }
    }
    return "unknown";
}

// Bounded micro-wait (the stop grace loop): milliseconds, via nanosleep so
// no feature-macro ever hides the declaration.
void sleep_ms(int milliseconds) {
    ::timespec rest{};
    rest.tv_sec = milliseconds / 1000;
    rest.tv_nsec = static_cast<long>(milliseconds % 1000) * 1000L * 1000L;
    while (::nanosleep(&rest, &rest) == -1 && errno == EINTR) {
        // resume the remaining wait
    }
}

[[noreturn]] void exec_failed_report_and_exit(int report_fd, unsigned short errno_value) {
    // Report the errno as the disambiguator (2 raw little-endian bytes) and
    // exit fast so the parent classifies instead of guessing.
    unsigned char bytes[2] = {static_cast<unsigned char>(errno_value & 0xFF),
                              static_cast<unsigned char>(errno_value >> 8)};
    ssize_t written = 0;
    do {
        const ssize_t got = ::write(report_fd, bytes + written, sizeof(bytes) - written);
        if (got < 0 && errno != EINTR) {
            break;
        }
        written += got;
    } while (written < static_cast<ssize_t>(sizeof(bytes)));
    ::close(report_fd);
    ::_exit(127); // never wander as a failed exec
}

} // namespace

class LinuxProcessControl final : public IProcessControl {
public:
    LinuxProcessControl() { ensure_children_are_autoreaped(); }

    std::vector<ProcessInfo> list() override {
        // Direct /proc inspection - replaces v1's tool-splice (`tasklist`)
        // with kernel text the process family reads itself.
        std::vector<ProcessInfo> processes;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
            const std::string name = entry.path().filename().string();
            if (!std::all_of(name.begin(), name.end(),
                             [](char c) { return c >= '0' && c <= '9'; })) {
                continue;
            }
            const std::uint32_t pid = static_cast<std::uint32_t>(std::stoul(name));
            processes.push_back(ProcessInfo{pid, read_process_name(pid)});
        }
        return processes;
    }

    std::uint32_t start(const std::vector<std::string>& argv) override {
        if (argv.empty()) {
            throw SyscallError("invalid_args", "start needs argv with at least a program name");
        }
        int report_pipe[2] = {-1, -1};
        if (::pipe(report_pipe) != 0) {
            throw SyscallError("execute_failed", "pipe() failed to spawn a launch report channel");
        }
        // Write end closes on a SUCCESSFUL exec (FD_CLOEXEC): the parent's
        // empty read is therefore the "child is running" signal.
        ::fcntl(report_pipe[1], F_SETFD, FD_CLOEXEC);
        const ::pid_t child = ::fork();
        if (child < 0) {
            ::close(report_pipe[0]);
            ::close(report_pipe[1]);
            throw SyscallError("execute_failed", "fork() failed to launch '" + argv[0] + "'");
        }
        if (child == 0) {
            ::close(report_pipe[0]);
            // execv's argv is char* const[]: own mutable copies, no const casts.
            std::vector<std::vector<char>> buffers;
            buffers.reserve(argv.size());
            for (const std::string& element : argv) {
                buffers.emplace_back(element.begin(), element.end());
                buffers.back().push_back('\0');
            }
            std::vector<char*> pointer;
            pointer.reserve(buffers.size() + 1);
            for (std::vector<char>& one : buffers) {
                pointer.push_back(one.data());
            }
            pointer.push_back(nullptr);
            ::execvp(pointer[0], pointer.data()); // argv as data; PATH-searched program
            exec_failed_report_and_exit(report_pipe[1],
                                        static_cast<unsigned short>(errno));
        }
        ::close(report_pipe[1]);
        unsigned char report[2] = {0, 0};
        const ssize_t got = ::read(report_pipe[0], report, sizeof(report));
        const bool exec_failed = got == static_cast<ssize_t>(sizeof(report));
        ::close(report_pipe[0]);
        if (exec_failed) {
            ::waitpid(child, nullptr, 0); // failed exec child: joined immediately
                                             // (harmless no-op under the auto-reap)
            const unsigned short errno_value = static_cast<unsigned short>(report[0]) |
                                               (static_cast<unsigned short>(report[1]) << 8);
            if (errno_value == ENOENT) {
                throw SyscallError("not_found", "program not found: '" + argv[0] + "'");
            }
            if (errno_value == EACCES) {
                throw SyscallError("access_denied", "access denied launching '" + argv[0] + "'");
            }
            throw SyscallError("execute_failed",
                               "exec failed for '" + argv[0] + "' (errno=" +
                                   std::to_string(errno_value) + ")");
        }
        return static_cast<std::uint32_t>(child);
    }

    void stop(std::uint32_t pid) override {
        if (pid == 0) {
            throw SyscallError("invalid_args", "pid 0 cannot be signalled");
        }
        // Signal-based stop (the v1 taskkill /F path, now kernel signals):
        // SIGTERM first for a graceful exit, SIGKILL behind a bounded grace
        // so no test or agent waits on a hung target.
        ::kill(static_cast<::pid_t>(pid), 0); // probe: classify before signalling
        if (errno == ESRCH) {
            throw SyscallError("not_found", "no process with pid " + std::to_string(pid));
        }
        if (errno == EPERM) {
            throw SyscallError("access_denied", "access denied stopping pid " + std::to_string(pid));
        }
        ::kill(static_cast<::pid_t>(pid), SIGTERM);
        bool gone = false;
        for (int probe = 0; probe < 16 && !gone; ++probe) { // <= ~48ms total grace
            if (::kill(static_cast<::pid_t>(pid), 0) == -1 && errno == ESRCH) {
                gone = true;
                break;
            }
            sleep_ms(3);
        }
        if (!gone) {
            ::kill(static_cast<::pid_t>(pid), SIGKILL);
            for (int probe = 0; probe < 64 && !gone; ++probe) { // <= ~192ms
                if (::kill(static_cast<::pid_t>(pid), 0) == -1 && errno == ESRCH) {
                    gone = true;
                    break;
                }
                sleep_ms(3);
            }
        }
        if (!gone) {
            throw SyscallError("execute_failed",
                               "pid " + std::to_string(pid) + " did not terminate; SIGKILL was ignored");
        }
    }
};

std::shared_ptr<IProcessControl> make_process_control() {
    return std::make_shared<LinuxProcessControl>();
}

#endif // platform split

} // namespace tether::syscalls
