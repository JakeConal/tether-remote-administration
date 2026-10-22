// lib/syscalls/power.hpp - the power family interface (design D6, task 6.4).
// Two verbs, both destructive at the dispatcher layer (matrix.hpp flags them
// `destructive`, so the agent-layer confirmation gate holds them BEFORE the
// family runs: an unconfirmed shutdown/restart reaches nothing here and the
// spec's "missing confirmation blocks a destructive command" scenario proves
// zero effect - this factory's real APIs are never invoked in test code).
//
// Per-OS backends (remote-commands delta + task 6.4 binding):
//   Windows- the desktop shutdown API: SeShutdownPrivilege is enabled on the
//            process token, then ExitWindowsEx(EWX_SHUTDOWN/EWX_REBOOT).
//   Linux  - logind over D-Bus (system shutdown via
//            org.freedesktop.login1.Manager PowerOff/Reboot). The sd-bus
//            calls ride libsystemd AT RUNTIME (dlopen) so the agent binary
//            builds and links without libsystemd while degrading to a
//            structured refusal where logind/libsystemd is absent.
//
// The logind binding is injectable: production binds the dlopen'd sd-bus
// binding, tests bind fakes - no test ever triggers a real power transition.
//
// All methods may throw SyscallError (errors.hpp) with a stable short code;
// the dispatcher turns failures into the structured Outcome the agent sends.
#pragma once

#include "errors.hpp"

#include <memory>

namespace tether::syscalls {

class IPowerControl {
public:
    virtual ~IPowerControl() = default;

    // Requests the OS to power the machine off. On success the call returns
    // normally while the OS sequences the transition; on any refused request
    // (privilege missing, service absent) it throws a SyscallError.
    virtual void shutdown() = 0;

    // Requests the OS to restart the machine; same contract as shutdown().
    virtual void restart() = 0;
};

// The Linux logind seam: exactly what the logind backend asks of
// org.freedesktop.login1 (task 6.4: tests bind a fake here; the production
// binding is the dlopen'd sd-bus one below). Both methods may throw
// SyscallError; `available()` reports whether the manager answers at all
// (no system bus, no logind, or libsystemd unloadable).
class ILogindBinding {
public:
    virtual ~ILogindBinding() = default;

    virtual bool available() = 0;
    virtual void power_off(bool restart) = 0;
};

// The concrete logind-backed implementation (the injected-binding shape of
// the family itself). `make_power_control()` returns this on Linux.
class LogindPowerControl : public IPowerControl {
public:
    explicit LogindPowerControl(std::shared_ptr<ILogindBinding> binding)
        : binding_(std::move(binding)) {}

    void shutdown() override;
    void restart() override;

private:
    void confirm_available_or_throw();

    std::shared_ptr<ILogindBinding> binding_;
};

// The Windows ExitWindowsEx-backed implementation (never invoked from tests:
// a confirmed shutdown call really is the OS shutdown transition).
class ExitWindowsPowerControl : public IPowerControl {
public:
    void shutdown() override;
    void restart() override;
};

// Production sd-bus binding: dlopen("libsystemd.so.0") and resolve the six
// sd_bus symbols the logind calls need. `available()` is the documented
// degradation path: when the library, the system bus, or the login manager
// is missing, the binding is unavailable and `power_off` raises the
// structured refusal ("execute_failed") the dispatcher reports.
std::shared_ptr<ILogindBinding> make_sd_bus_logind_binding();

// Builds the platform implementation (Windows or Linux, decided at compile
// time) for the wired agent.
std::shared_ptr<IPowerControl> make_power_control();

// The named per-OS factories (tests inject fakes at the dispatcher instead
// of these, per the task 6.4 binding).
std::shared_ptr<IPowerControl> make_windows_power_control();
std::shared_ptr<IPowerControl> make_linux_power_control(
    std::shared_ptr<ILogindBinding> binding);

} // namespace tether::syscalls
