// lib/syscalls/power.cpp - the power family per-OS implementations (task 6.4):
//
//   Windows- ExitWindowsEx after enabling SeShutdownPrivilege on the process
//            token (the "explicit authorization" of the desktop API). The
//            call either initiates the transition (returns an outcome) or
//            fails with a Windows error mapped to the structured vocabulary.
//            Failure to obtain the privilege is "access_denied".
//   Linux  - logind (org.freedesktop.login1): PowerOff/Reboot through
//            sd-bus. libsystemd is dlopen'd at call time so a build without
//            the dev package still runs wherever logind exists; absence
//            degrades to the documented structured refusal. Tests bind a
//            fake ILogindBinding - no test ever reaches the real bus.
//
// Both build branches live in this TU behind preprocessor gates, as with
// process.cpp.
#include "power.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>

#include <atomic>
#include <mutex>
#include <string>
#endif

#include <memory>

namespace tether::syscalls {

namespace {

SyscallError refused(const std::string& code, const std::string& message) {
    return SyscallError(code, message);
}

} // namespace

// ---- Windows: the desktop shutdown API --------------------------------------

#if defined(_WIN32)

namespace {

// AdjustTokenPrivileges dance for SeShutdownPrivilege: ExitWindowsEx fails
// with ERROR_PRIVILEGE_NOT_HELD otherwise.
bool enable_shutdown_privilege() {
    HANDLE process_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &process_token)) {
        return false;
    }
    TOKEN_PRIVILEGES privileges = {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege",
                               &privileges.Privileges[0].Luid)) {
        CloseHandle(process_token);
        return false;
    }
    const BOOL adjusted =
        AdjustTokenPrivileges(process_token, FALSE, &privileges, 0, nullptr, nullptr);
    const bool ok = adjusted && GetLastError() == ERROR_SUCCESS;
    CloseHandle(process_token);
    return ok;
}

std::string last_error_message(DWORD error) {
    LPSTR buffer = nullptr;
    const DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, error,
                                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string message = size > 0 ? std::string(buffer, size) : std::string();
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' ||
                                message.back() == ' ')) {
        message.pop_back();
    }
    return "Windows error " + std::to_string(error) +
           (message.empty() ? std::string() : ": " + message);
}

void request_power_off(const bool shutdown_requested) {
    if (!enable_shutdown_privilege()) {
        throw refused("access_denied",
                      "could not enable SeShutdownPrivilege on the process token: " +
                          last_error_message(GetLastError()));
    }
    const DWORD flags =
        (shutdown_requested ? EWX_SHUTDOWN : EWX_REBOOT) | EWX_FORCEIFHUNG;
    const DWORD reason =
        SHTDN_REASON_MAJOR_SYSTEM /*operator-initiated via a managed console*/ |
        SHTDN_REASON_MINOR_MAINTENANCE;
    if (ExitWindowsEx(flags, reason) == 0) {
        throw refused("execute_failed",
                      shutdown_requested ? "shutdown was refused by the system: " : "restart was refused by the system: " +
                          last_error_message(GetLastError()));
    }
    // On success the transition is now owned by the OS; the process may be
    // terminated mid-return. The dispatcher reports the initiated outcome.
}

} // namespace

void ExitWindowsPowerControl::shutdown() { request_power_off(true); }
void ExitWindowsPowerControl::restart() { request_power_off(false); }

std::shared_ptr<IPowerControl> make_windows_power_control() {
    return std::make_shared<ExitWindowsPowerControl>();
}

std::shared_ptr<IPowerControl> make_power_control() {
    return make_windows_power_control();
}

// ---- Linux: logind over D-Bus through dlopen'd sd-bus -----------------------

#else

namespace {

// The exact slice of the sd-bus ABI the binding uses; opaque types and a
// stable-struct only (sd_bus_error's three-field layout is fixed by
// libsystemd's public headers). Every call resolves through dlsym off the
// dlopen'd libsystemd.so.0, so no build/link-time dependency exists - the
// documented availability check covers hosts without it.
struct SdBus;      // opaque
struct SdBusMessage; // opaque

struct SdBusError {
    const char* name;
    const char* message;
    int need_free;
};

using FnSdBusOpenSystem = int (*)(SdBus**);
using FnSdBusCallMethod = int (*)(SdBus*, const char*, const char*, const char*,
                                 const char*, SdBusError*, SdBusMessage**, const char*, ...);
using FnSdBusGetPropertyString = int (*)(SdBus*, const char*, const char*, const char*,
                                        const char*, SdBusError*, const char**);
using FnSdBusUnrefMessage = void (*)(SdBusMessage*);
using FnSdBusUnref = SdBus* (*)(SdBus*);
using FnSdBusErrorFree = void (*)(SdBusError*);

struct SdSymbols {
    FnSdBusOpenSystem open_system = nullptr;
    FnSdBusCallMethod call_method = nullptr;
    FnSdBusGetPropertyString get_property_string = nullptr;
    FnSdBusUnrefMessage unref_message = nullptr;
    FnSdBusUnref unref = nullptr;
    FnSdBusErrorFree error_free = nullptr;
    bool load_error = false;
    std::string error_text;
};

// dlopen is idempotent (refcounted) and dlsym is table-driven; loading is
// guarded once. A load failure is remembered as `load_error` with `error_text`
// carrying the reason (missing libsystemd), which IS the structured refusal
// the power verbs report on such hosts.
SdSymbols load_sd_symbols() {
    static std::mutex load_mutex;
    static std::atomic<SdSymbols*> cached{nullptr};
    static SdSymbols table;
    {
        const std::lock_guard<std::mutex> guard(load_mutex);
        if (cached.load() != nullptr) {
            return *cached.load();
        }
        void* handle = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            table.load_error = true;
            table.error_text = std::string("libsystemd (sd-bus) not available: ") + dlerror();
            cached.store(&table);
            return table;
        }
        table.open_system = reinterpret_cast<FnSdBusOpenSystem>(dlsym(handle, "sd_bus_open_system"));
        table.call_method = reinterpret_cast<FnSdBusCallMethod>(dlsym(handle, "sd_bus_call_method"));
        table.get_property_string = reinterpret_cast<FnSdBusGetPropertyString>(
            dlsym(handle, "sd_bus_get_property_string"));
        table.unref_message = reinterpret_cast<FnSdBusUnrefMessage>(dlsym(handle, "sd_bus_message_unref"));
        table.unref = reinterpret_cast<FnSdBusUnref>(dlsym(handle, "sd_bus_unref"));
        table.error_free = reinterpret_cast<FnSdBusErrorFree>(dlsym(handle, "sd_bus_error_free"));
        const bool ok = table.open_system != nullptr && table.call_method != nullptr &&
                        table.get_property_string != nullptr && table.unref_message != nullptr &&
                        table.unref != nullptr && table.error_free != nullptr;
        if (!ok) {
            table.load_error = true;
            table.error_text = "libsystemd present but the required sd-bus symbols did not resolve";
        }
        cached.store(&table);
        return table;
    }
}

// Manages one open system bus plus the per-call sd_bus_error scratch (the
// binding's ownership discipline mirrors what sd-bus C code does).
class SystemBus {
public:
    SystemBus() {
        const SdSymbols symbols = load_sd_symbols();
        if (symbols.load_error) {
            throw refused("execute_failed", symbols.error_text);
        }
        symbols_ = symbols;
        SdBus* bus = nullptr;
        const int result = symbols_.open_system(&bus);
        if (result < 0) {
            throw refused("execute_failed",
                          "could not connect to the system bus: "
                          "logind is not available on this host (" +
                              std::to_string(result) + ")");
        }
        bus_ = bus;
    }

    ~SystemBus() {
        if (bus_ != nullptr) {
            symbols_.unref(bus_);
        }
    }

    SystemBus(const SystemBus&) = delete;
    SystemBus& operator=(const SystemBus&) = delete;

    const SdSymbols& symbols() const { return symbols_; }
    SdBus* bus() const { return bus_; }
    void release() { bus_ = nullptr; }

private:
    SdSymbols symbols_{};
    SdBus* bus_ = nullptr;
};

// The production binding: every call opens the system bus fresh (no
// long-lived state) and asks logind's Manager. Availability is proven by
// asking the CanPowerOff property - logind answers with a readiness verdict.
class SdBusLogindBinding final : public ILogindBinding {
public:
    bool available() override {
        try {
            SystemBus bus;
            SdBusError error = {};
            const char* verdict = nullptr;
            const int result = bus.symbols().get_property_string(
                bus.bus(), "org.freedesktop.login1", "/org/freedesktop/login1",
                "org.freedesktop.login1.Manager", "CanPowerOff", &error, &verdict);
            const bool answered = result >= 0;
            if (error.name != nullptr) {
                bus.symbols().error_free(&error);
            }
            return answered && verdict != nullptr;
        } catch (const SyscallError&) {
            return false;
        }
    }

    void power_off(const bool restart) override {
        SystemBus bus;
        SdBusError error = {};
        SdBusMessage* reply = nullptr;
        const int result = bus.symbols().call_method(
            bus.bus(), "org.freedesktop.login1", "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager", restart ? "Reboot" : "PowerOff", &error, &reply,
            "b", 0 /*interactive=false: an explicitly authorized request*/);
        const std::string failed = result < 0 ? std::to_string(result)
                                             : (error.name != nullptr ? error.name : std::string{});
        if (error.name != nullptr) {
            bus.symbols().error_free(&error);
        }
        if (reply != nullptr) {
            bus.symbols().unref_message(reply);
        }
        if (!failed.empty()) {
            throw refused("execute_failed",
                          (restart ? "reboot" : std::string("power off")) +
                              " was refused by logind: " + failed);
        }
        // Success: the manager sequences the transition (the agent process
        // may be terminated while returning).
    }
};

} // namespace

std::shared_ptr<ILogindBinding> make_sd_bus_logind_binding() {
    return std::make_shared<SdBusLogindBinding>();
}

void LogindPowerControl::confirm_available_or_throw() {
    if (!binding_->available()) {
        throw refused("execute_failed",
                      "logind is not available on this system (no libsystemd, "
                      "no system bus, or no login manager): nothing was executed");
    }
}

void LogindPowerControl::shutdown() {
    confirm_available_or_throw();
    try {
        binding_->power_off(false);
    } catch (const SyscallError&) {
        throw; // already in the structured vocabulary
    } catch (const std::exception& error) {
        throw refused("execute_failed", std::string("power off failed: ") + error.what());
    }
}

void LogindPowerControl::restart() {
    confirm_available_or_throw();
    try {
        binding_->power_off(true);
    } catch (const SyscallError&) {
        throw;
    } catch (const std::exception& error) {
        throw refused("execute_failed", std::string("reboot failed: ") + error.what());
    }
}

std::shared_ptr<IPowerControl> make_linux_power_control(
    std::shared_ptr<ILogindBinding> binding) {
    return std::make_shared<LogindPowerControl>(std::move(binding));
}

std::shared_ptr<IPowerControl> make_power_control() {
    return make_linux_power_control(make_sd_bus_logind_binding());
}

#endif

} // namespace tether::syscalls
