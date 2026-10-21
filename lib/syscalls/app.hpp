// lib/syscalls/app.hpp - the app family interface (design D6, task 6.8).
// Three verbs: list_app / start_app / stop_app, exposed through IAppLauncher.
//
// The family enumerates launchable applications from the per-OS standard
// store of application shortcuts - Start-menu `.lnk` files on Windows,
// freedesktop `.desktop` entries on Linux - and starts one by resolving its
// backing entry to a plain ARGV through the EXISTING argv-based process
// family launcher (no shell ever re-splices the target; the remote-commands
// delta's parameters-are-data requirement applies with full force: a
// shortcut's target and its arguments ride argv data end to end). stop_app
// reuses the process family's stop (pid or exact process name).
//
// Listing roots are injected (make_app_launcher_with_roots) so tests point
// the family at temporary fixture directories; production uses the standard
// directories of the platform.
#pragma once

#include "errors.hpp"
#include "process.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tether::syscalls {

// One launchable application: the display/entry name and the backing
// shortcut file's path (the .lnk / .desktop file the entry came from).
struct AppEntry {
    std::string name;
    std::string path;
};

class IAppLauncher {
public:
    virtual ~IAppLauncher() = default;

    // The applications the standard store (or the injected roots) describes:
    // .lnk files under the Windows roots, Type=Application .desktop entries
    // under the Linux roots. Deterministically sorted (name, then path);
    // duplicate names are legal and every entry is listed exactly once.
    virtual std::vector<AppEntry> list() = 0;

    // Resolves `app_name` against list() (first exact match in sorted order)
    // and launches the resolved target as ARGV through the process family
    // (the caller never reshapes the target itself). Unknown names refuse
    // with "not_found"; the underlying launcher maps exec failures to the
    // classified vocabulary (not_found / access_denied / execute_failed).
    // Returns the child's pid.
    virtual std::uint32_t start(const std::string& app_name) = 0;
};

// The production implementation: standard roots for this platform
// (Windows: per-user + all-users Start Menu Programs; Linux:
// XDG_DATA_HOME/applications plus XDG_DATA_DIRS/applications per the XDG
// base-directory spec) and a fresh process control for launches.
std::shared_ptr<IAppLauncher> make_app_launcher();

// Same, over the GIVEN process control (the agent main wires ONE instance
// across the process and app families so stop_app and the process family
// share one launcher) and the standard roots of the platform.
std::shared_ptr<IAppLauncher> make_app_launcher_with_processes(
    std::shared_ptr<IProcessControl> processes);

// Same, over the GIVEN process control (the agent main wires ONE instance
// across the process and app families so stop_app and the process family
// share one launcher) and the GIVEN listing roots (tests point the family
// at fixture directories).
std::shared_ptr<IAppLauncher> make_app_launcher_with_roots(
    std::vector<std::string> roots, std::shared_ptr<IProcessControl> processes);

// The freedesktop Exec= field parser (desktop-entry spec, argv semantics):
// splits one Exec value into argv, honouring the spec's quoting rules
// (double-quoted strings with the \\ \" \` \$ escapes, single-quoted strings
// with no escapes) and stripping the %field codes the launcher does not
// serve. Exposed for the family tests.
std::vector<std::string> parse_desktop_exec(const std::string& exec_value);

} // namespace tether::syscalls
