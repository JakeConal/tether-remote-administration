// lib/syscalls/input.hpp - the input family interface (design D6, task
// 6.7). Four verbs: start_keylogger / stop_keylogger / lock_keyboard /
// unlock_keyboard, exposed in the matrix (matrix.hpp) through IInputControl.
//
// The keylogger runs as a managed background job in the SHARED registry
// (jobs.hpp, slot "input") exactly like the camera family, so the job
// lifecycle is registry-frozen behavior reuse: duplicate start is refused
// by the registry, stop delivers the accumulated capture, and the session
// stays responsive while the job runs. The configurable capture storage
// comes from the start command's parameters: `start_keylogger [path]`,
// `kDefaultCaptureStorage` when omitted (documented default, agent-local).
//
// Authorized-use framing: both jobs are explicit console commands that ride
// the audit trail (console-side records, task 6.9); there is no hidden
// startup path and no stealth behavior anywhere in this family.
//
// Every platform reality sits behind an injected seam so tests never touch
// a real global hook, a real input lock, or a real desktop:
//   - IInputEnvironmentProbe  Wayland/no-display environments resolve to
//                             the structured unsupported result here;
//   - IKeyEventSource         the capture engine (Windows low-level keyboard
//                             hook / Linux XQueryKeymap, see input.cpp);
//   - IKeyCaptureSink         the accumulating capture storage
//                             (production: the file sink at the configured
//                             storage path);
//   - IKeyboardGuard          the keyboard lock (Windows BlockInput /
//                             Linux XGrabKeyboard - the picks are documented
//                             at their implementations).
#pragma once

#include "errors.hpp"
#include "jobs.hpp"
#include "matrix.hpp" // Outcome::Deliverable (the file-result shape)

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tether::syscalls {

// The capture storage file this agent accumulates into when a start carries
// no explicit path. Agent-local by design: the operator names overrides on
// the wire, the audit trail (6.9) records whichever path was used.
inline constexpr const char* kDefaultCaptureStorage = "keylog.txt";

// The desktop-input environment this process could serve. Wayland (or a
// missing X server on Linux) is a structured UNSUPPORTED resolution, not a
// crash and not a silent no-op (remote-commands delta: capability matrix).
struct InputEnvironment {
    bool input_capable = false; // a desktop input channel this family can use
    bool wayland = false;       // the session identifies as Wayland
    std::string note;           // human detail surfaced in refusals
};

// The environment seam. Production reads the real session state (Windows:
// the process owns a desktop session; Linux: XDG_SESSION_TYPE, DISPLAY);
// tests inject fakes to exercise the unsupported resolution everywhere.
class IInputEnvironmentProbe {
public:
    virtual ~IInputEnvironmentProbe() = default;
    virtual InputEnvironment probe() = 0;
};

// The accumulating capture storage: the worker appends events, the stop path
// hands back the accumulated result. `take_result` returns ready=false when
// the sink could not produce a deliverable at all (the stop refuses with the
// structured execute_failed outcome instead of an apparently-successful
// empty one; an EMPTY capture is honest and delivered as a zero-byte file).
class IKeyCaptureSink {
public:
    virtual ~IKeyCaptureSink() = default;

    // One captured event (the production sink appends a line per event).
    virtual void append(const std::string& line) = 0;

    // The accumulated result: name + byte content for the file result the
    // stop command delivers.
    virtual JobDeliverable take_result() = 0;

    // Releases the storage (idempotent).
    virtual void close() = 0;
};

// The raw capture seam. Built by the family's factory and driven by the
// background worker: open -> poll ticks (producing the events since the last
// tick) -> close. `open` false maps to the structured start-then-fail path
// the registry models (the failure surfaces through stop, never wedging).
class IKeyEventSource {
public:
    virtual ~IKeyEventSource() = default;

    // Acquires the capture engine; false when the platform refuses it (no
    // hook available / no display). Never blocks long.
    virtual bool open() = 0;

    // One paced tick: the events captured since the last tick (possibly
    // none). A disconnect/loss of the source returns empty and keeps the
    // worker in its bounded loop until stop (the stop is the authority).
    virtual std::vector<std::string> poll() = 0;

    virtual void close() = 0;
};

// The keyboard-lock seam. The family owns the locked/unlocked STATE (and the
// already-locked / not-locked structured refusals); the guard owns only the
// platform mechanics of engaging and releasing the lock.
class IKeyboardGuard {
public:
    virtual ~IKeyboardGuard() = default;

    // Engages the platform's reversible system-level keyboard block. Throws
    // SyscallError("access_denied"/"execute_failed") when the platform
    // refuses; on success the lock holds until release().
    virtual void engage() = 0;

    // Releases the lock (idempotent; a lost display degrades to a no-op).
    virtual void release() = 0;
};

class IInputControl {
public:
    virtual ~IInputControl() = default;

    // Starts the keylogger background job on the shared registry. An empty
    // `storage` uses kDefaultCaptureStorage; a path the storage cannot open
    // is the structured refusal (the job is never seated). A duplicate start
    // while the job is running is the structured already-running refusal.
    virtual void start_keylogger(const std::string& storage) = 0;

    // Stops the running job (signal + join) and returns its accumulated
    // capture as the file result. Nothing running -> "not_running"; a
    // failed job or a sink without a deliverable -> "execute_failed".
    virtual Outcome::Deliverable stop_keylogger() = 0;

    // Engages the keyboard lock. Already locked -> "already_running" (the
    // running lock is untouched); the unengaged platform error paths come
    // from the guard as classified failures.
    virtual void lock_keyboard() = 0;

    // Releases the keyboard lock. Not locked -> "not_running".
    virtual void unlock_keyboard() = 0;
};

// The concrete family implementation: one registry-backed job lifecycle plus
// the lock state machine, all platform mechanics arriving through the
// bindings below (production wires the per-OS real ones; tests inject
// fakes and a tiny waiter).
class InputControl final : public IInputControl {
public:
    struct Bindings {
        using SinkFactory =
            std::function<std::shared_ptr<IKeyCaptureSink>(const std::string& storage)>;
        using SourceFactory = std::function<std::shared_ptr<IKeyEventSource>()>;
        using GuardFactory = std::function<std::shared_ptr<IKeyboardGuard>()>;
        using ProbeFactory = std::function<std::shared_ptr<IInputEnvironmentProbe>()>;

        ProbeFactory probe;
        SourceFactory source;
        SinkFactory sink;
        GuardFactory guard;
    };

    // `waiter` paces the worker between capture ticks (production: the real
    // tick interval; tests: a no-op). `registry` is the SHARED job registry
    // the keylogger plugs into with its own slot (task 6.7 binding).
    InputControl(Bindings bindings, std::shared_ptr<BackgroundJobRegistry> registry,
                 std::function<void()> waiter)
        : bindings_(std::move(bindings)), registry_(std::move(registry)),
          waiter_(std::move(waiter)) {}

    void start_keylogger(const std::string& storage) override;
    Outcome::Deliverable stop_keylogger() override;
    void lock_keyboard() override;
    void unlock_keyboard() override;

    // Test/inspection hook: whether a keyboard engagement is currently held.
    bool locked() const noexcept { return guard_ != nullptr; }

private:
    // The one structured unsupported resolution for Wayland / no-X
    // environments (never a wedge: see the callers).
    void require_capable_environment(const std::string& verb);

    Bindings bindings_;
    std::shared_ptr<BackgroundJobRegistry> registry_;
    std::function<void()> waiter_;
    std::shared_ptr<IKeyboardGuard> guard_; // held while the keyboard is locked
};

// Builds the production implementation over the SHARED registry instance of
// the agent process (the same instance the camera family registers with).
std::shared_ptr<IInputControl> make_input_control();

// Same, over an INJECTED registry (agent main wires ONE shared registry
// across families; tests inject their own).
std::shared_ptr<IInputControl> make_input_control_with_registry(
    std::shared_ptr<BackgroundJobRegistry> registry);

// The production capture sink: appends one line per captured event to the
// configured storage file (created/truncated lazily on first write; existing
// content is preserved - the file IS the accumulated capture) and reads the
// file back wholesale at take_result. The storage's filename becomes the
// delivered capture's name.
std::shared_ptr<IKeyCaptureSink> make_file_capture_sink(const std::string& storage);

} // namespace tether::syscalls
