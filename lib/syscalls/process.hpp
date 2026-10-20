// lib/syscalls/process.hpp - the process family interface (design D6, task
// 6.2). The interface is the per-OS seam: Linux walks /proc and stops with
// signals, Windows snapshots via Toolhelp32 and stops with TerminateProcess,
// and both launch through argv - fork+exec on Linux, CreateProcessW on
// Windows - with NO shell anywhere, so parameters stay typed data end to end
// (remote-commands delta: "parameters are data, never shell syntax").
//
// All methods may throw SyscallError (errors.hpp) with a stable short code;
// the dispatcher turns failures into the structured Outcome the agent sends.
#pragma once

#include "errors.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tether::syscalls {

// One running process: pid plus the image name (Windows) or the argv[0] base
// name falling back to the comm name (Linux). Names are matched exactly by
// stop-by-name callers.
struct ProcessInfo {
    std::uint32_t pid = 0;
    std::string name;
};

class IProcessControl {
public:
    virtual ~IProcessControl() = default;

    // A snapshot of the running processes. There is no atomicity guarantee
    // across the OS calls; callers treat it as a point-in-time view.
    virtual std::vector<ProcessInfo> list() = 0;

    // Launch `argv` with argv[0] as the program and the remaining elements as
    // its argv, verbatim - each element is one piece of argv data, never
    // re-spliced into shell text. The child runs detached (the parent does
    // not wait for it); on the child failing to exec, a SyscallError names
    // the cause (ENOENT -> "not_found", EACCES -> "access_denied"). Returns
    // the child's pid.
    virtual std::uint32_t start(const std::vector<std::string>& argv) = 0;

    // Terminate the process with the given pid. On Linux SIGTERM is sent
    // first with a bounded escalation to SIGKILL; on Windows TerminateProcess
    // terminates directly. A pid that does not exist throws "not_found".
    virtual void stop(std::uint32_t pid) = 0;
};

// Builds the platform implementation (Windows or Linux, decided at compile
// time). Construction installs the Linux SIGCHLD auto-reap so started
// children never accumulate as zombies in the long-lived agent.
std::shared_ptr<IProcessControl> make_process_control();

} // namespace tether::syscalls
