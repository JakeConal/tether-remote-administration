// lib/syscalls/matrix.hpp - the capability matrix and its dispatch layer
// (design D6, task 6.1). This is the agent's entire command surface in data
// form:
//
//  - `command_set()` holds exactly the v2 command set carried over from v1
//    (19 verbs across the 7 families). Anything outside it is rejected
//    BEFORE any capability call: the closed set (remote-commands delta:
//    "unknown command is refused harmlessly").
//  - Each row pins the family interface the verb maps to and its per-platform
//    support. A verb whose row says the platform lacks it - or whose family
//    implementation is not wired into this build yet - resolves to the
//    structured "unsupported_platform" refusal (spec: "capability matrix
//    instead of silent gaps": one definitive resolution per command/platform
//    pair, session never wedged).
//  - Rows flagged `destructive` carry the agent-layer confirmation gate
//    (spec: "destructive commands carry explicit authorization"): `shutdown`
//    and `restart` (the power family) plus file `delete`; the task 6.3
//    binding adds `move` and `copy` (both replace destinations), so all five
//    require the confirmation token as args[0] BEFORE anything touches disk.
//
// Parameters never become shell text: dispatch hands every argument to the
// family interface as data (see process.hpp / file.hpp).
//
// The dispatcher is also the extension point for the remaining families:
// power / capture / camera landed (6.4-6.6, real interfaces in power.hpp /
// capture.hpp / camera.hpp - wired through `set_power_control` /
// `set_screen_capture` / `set_camera_capture`); input / app slots land with
// their owning tasks (6.7-6.8). Until then those verbs dispatch to the
// structured unsupported result below - the definitive, non-wedging
// resolution.
#pragma once

#include "errors.hpp"
#include "file.hpp"
#include "process.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tether::syscalls {

enum class Platform { Windows, Linux };

const char* platform_name(Platform platform); // "windows" | "linux" (refusal text)

// Compile-time constant: the platform this binary serves (CI covers exactly
// these two). Injections exist for tests of the matrix mechanism itself.
Platform current_platform();

enum class Family { Process, File, Power, Capture, Camera, App, Input };

const char* family_name(Family family); // "process" | ... (refusal text)

// Stable short codes for the wire-visible Outcome::code. Success carries
// "ok"; failures carry exactly one of:
//   unknown_command      the verb is outside the closed set (names the verb)
//   unsupported_platform a known verb without a resolution on this platform
//                        (names the platform and the family)
//   missing_confirmation destructive verb without the confirmation token
//   invalid_args         verb present but the argument shape is malformed
//   not_found / access_denied / directory_not_empty / already_exists /
//     cross_device / execute_failed  (see errors.hpp for the family codes)
//   already_running  a background-job start arrived for a running job
//                    (camera start_webcam; later the input family)
//   not_running      a background-job stop arrived with nothing running
//   internal_error   an unexpected failure was converted, never leaked
//                    as an unclassified outcome
struct Outcome {
    bool ok = false;
    std::string code = "internal_error";
    std::string message;              // human detail; refusals name verb/platform/family
    std::vector<std::string> lines;   // success text payload, one row per entry
    // Success file payload (byte-complete + named on the wire; the background
    // job families land here through the same outcome shape in 6.5-6.7).
    struct Deliverable {
        std::string name;
        std::vector<unsigned char> bytes;
    };
    std::optional<Deliverable> file;
};

// The confirmation token verb-gating a destructive command (above). It is
// the FIRST argument on the wire, stripped before the family sees the argv.
inline constexpr const char* kConfirmMarker = "confirm";

// One matrix row (pure data; nothing here runs a system call).
struct CommandSpec {
    const char* verb;     // the wire command word, exactly as parsed from CMD
    Family family;
    bool destructive;     // agent-layer confirmation gate applies
    bool on_windows;
    bool on_linux;
};

// The closed command set, one row per verb (19 rows, 7 families). The set is
// the v1 vocabulary carried over: process list/start/stop as "list"/"start"/
// "stop", the file family as "ls"/"delete"/"move"/"copy", the camera pair as
// "start_webcam"/"stop_webcam", and so on (the operator-console delta's CLI
// gate pins "delete"/"shutdown"/"restart" against the same words).
const std::vector<CommandSpec>& command_set();

// Exact-match lookup against the closed set: nullptr means the verb is
// outside it (no aliases, no fuzzy matching - the rejected space stays empty
// so scripts can rely on rejections).
const CommandSpec* find_command(const std::string& verb);

bool supported_on(const CommandSpec& spec, Platform platform);

// Family interfaces for the families implemented in this build. The file
// and process families are defined in their own headers (included above);
// power (power.hpp), capture (capture.hpp) and camera (camera.hpp) are
// declared here, defined there, and wired through the setters on
// CommandDispatcher below. Input (6.7) / app (6.8) remain the
// task-6.1 stubs until their owning tasks grow them.
class IPowerControl;
class IScreenCapture;
class ICameraCapture;

class IInputControl {  // keylogger / keyboard lock land with task 6.7
public:
    virtual ~IInputControl() = default;
};

class IAppLauncher {   // app list/start/stop land with task 6.8
public:
    virtual ~IAppLauncher() = default;
};

class CommandDispatcher {
public:
    // `platform` is injectable so tests exercise both refusal paths without
    // fabricating binaries; `table` is injectable for the same reason (the
    // tests may pin one row to a platform without the row to prove the
    // matrix gate). Both default to the real build environment.
    explicit CommandDispatcher(Platform platform = current_platform(),
                               const std::vector<CommandSpec>& table = command_set());

    // Family wiring for the families implemented in this build. A null slot
    // keeps every verb of that family resolving to the structured
    // unsupported refusal (this is the state between family tasks).
    void set_process_control(std::shared_ptr<IProcessControl> impl);
    void set_file_system(std::shared_ptr<IFileSystem> impl);
    // 6.4 power, 6.5 capture, 6.6 camera (the camera implementation owns
    // its shared background-job registry internally - see camera.hpp).
    void set_power_control(std::shared_ptr<IPowerControl> impl);
    void set_screen_capture(std::shared_ptr<IScreenCapture> impl);
    void set_camera_capture(std::shared_ptr<ICameraCapture> impl);

    // THE dispatch entry: one definitive Outcome for ANY (verb, args), in
    // this order of checks, each refusing without touching any capability:
    //   1. closed set       -> "unknown_command" (names the verb)
    //   2. matrix row       -> "unsupported_platform" if the platform lacks
    //                          the family (names platform + family)
    //   3. family impl      -> "unsupported_platform" if the family is not
    //                          implemented in this build (names both)
    //   4. confirmation     -> "missing_confirmation" for destructive verbs
    //                          without the marker token
    //   5. family execution -> the family Outcome, or a classified failure
    //                          from its SyscallError / std::exception.
    Outcome dispatch(const std::string& verb, const std::vector<std::string>& args) const;

    Platform platform() const noexcept { return platform_; }
    const std::vector<CommandSpec>& table() const noexcept { return table_; }

private:
    bool family_available(Family family) const;

    Platform platform_;
    const std::vector<CommandSpec>& table_;
    std::shared_ptr<IProcessControl> process_;
    std::shared_ptr<IFileSystem> file_;
    std::shared_ptr<IPowerControl> power_;
    std::shared_ptr<IScreenCapture> capture_;
    std::shared_ptr<ICameraCapture> camera_;
};

} // namespace tether::syscalls
