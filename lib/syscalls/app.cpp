// lib/syscalls/app.cpp - the app family per-OS implementations (task 6.8):
//
//   Windows- Start-menu `.lnk` enumeration over the injected roots, launch
//            resolution through the COM shell API (IShellLinkW + IPersistFile
//            - the documented shell-API path): the shortcut's target and its
//            argument tail resolve into plain argv (the tail is split by the
//            system's own argv rules via CommandLineToArgvW, not by a shell),
//            and the argv-based process family launches it. No shell is ever
//            spawned; no parameter is ever re-spliced into shell text.
//   Linux  - freedesktop `.desktop` entries: Type=Application entries from
//            the injected roots, Name= for the entry name, and Exec= parsed
//            as ARGV per the desktop-entry spec (quoting rules honoured, no
//            /bin/sh anywhere), launched through the process family.
//
// Failures surface in the structured vocabulary: unknown names -> not_found;
// broken/unresolvable shortcuts -> not_found or execute_failed per cause
// (never an unclassified outcome). Listing roots are injected (task 6.8
// binding: "an injectable root-dir override for tests"); production uses the
// platform's standard directories.
#include "app.hpp"

#include <algorithm>
#include <cstdlib>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW (the argv splitter, not a shell)
#include <shlobj.h>   // IShellLinkW / IPersistFile / CLSID_ShellLink
#if defined(_MSC_VER)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#endif
#else
#include <filesystem>
#include <fstream>
#endif

namespace tether::syscalls {

namespace {

std::string entry_name_from_path(const std::string& path) {
    const std::size_t last_slash = path.find_last_of("/\\");
    const std::string filename = last_slash == std::string::npos ? path : path.substr(last_slash + 1);
    const std::size_t dot = filename.rfind('.');
    return dot == std::string::npos ? filename : filename.substr(0, dot);
}

void sort_entries(std::vector<AppEntry>& entries) {
    std::sort(entries.begin(), entries.end(), [](const AppEntry& a, const AppEntry& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.path < b.path;
    });
}

const AppEntry* find_exact(const std::vector<AppEntry>& entries, const std::string& app_name) {
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&app_name](const AppEntry& entry) {
                                        return entry.name == app_name;
                                    });
    if (found == entries.end()) {
        throw SyscallError("not_found", "no application named '" + app_name + "'");
    }
    return &*found;
}

// ---- Windows: Start-menu shortcuts ------------------------------------------------

#if defined(_WIN32)

std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), size);
    wide.resize(static_cast<std::size_t>(size));
    return wide;
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                         static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(),
                        size, nullptr, nullptr);
    utf8.resize(static_cast<std::size_t>(size));
    return utf8;
}

// Finds every *.lnk under one root (a Start-menu tree nests several levels,
// so this walk is recursive by design; the injected test roots copy that
// shape).
void collect_lnk_files(const std::wstring& root, std::vector<std::string>& out) {
    WIN32_FIND_DATAW found{};
    const std::wstring pattern = root + L"\\*";
    const HANDLE handle = FindFirstFileW(pattern.c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE) {
        return; // an absent root lists nothing (a standard dir may not exist)
    }
    do {
        const std::wstring name(found.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }
        const std::wstring full = root + L"\\" + name;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            collect_lnk_files(full, out);
        } else if (full.size() > 4 && full.compare(full.size() - 4, 4, L".lnk") == 0) {
            out.push_back(wide_to_utf8(full));
        }
    } while (FindNextFileW(handle, &found));
    FindClose(handle);
}

class WindowsShortcutsAppLauncher final : public IAppLauncher {
public:
    WindowsShortcutsAppLauncher(std::vector<std::string> roots,
                                std::shared_ptr<IProcessControl> processes)
        : roots_(std::move(roots)), processes_(std::move(processes)) {}

    std::vector<AppEntry> list() override {
        std::vector<AppEntry> entries;
        for (const std::string& root : roots_) {
            if (root.empty()) {
                continue; // an absent environment variable contributes no root
            }
            std::vector<std::string> paths;
            collect_lnk_files(utf8_to_wide(root), paths);
            std::sort(paths.begin(), paths.end());
            for (const std::string& path : paths) {
                entries.push_back(AppEntry{entry_name_from_path(path), path});
            }
        }
        sort_entries(entries);
        return entries;
    }

    std::uint32_t start(const std::string& app_name) override {
        // Resolve through the SAME listing the operator saw (deterministic
        // sort; the entry path is plain data from here on). The listing is
        // HELD in a named vector: a pointer into it must outlive this
        // statement (find_exact cannot return into a dropped temporary).
        const std::vector<AppEntry> entries = list();
        const AppEntry* entry = find_exact(entries, app_name);
        return processes_->start(resolve_shortcut_argv(app_name, entry->path));
    }

private:
    // THE shell-API resolution (IShellLinkW + IPersistFile, per the task
    // binding): the shortcut's target path and its argument tail arrive as
    // data. The COM apartment is per-call; a broken target refuses with a
    // structured error instead of a modal prompt (SLR_NO_UI).
    std::vector<std::string> resolve_shortcut_argv(const std::string& app_name,
                                                   const std::string& shortcut_path) const {
        std::vector<std::string> argv;
        const HRESULT initialized =
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool com_was_mine = SUCCEEDED(initialized);
        if (!com_was_mine && initialized != RPC_E_CHANGED_MODE) {
            throw SyscallError("execute_failed",
                               "the COM apartment refused the shortcut resolution for '" +
                                   app_name + "'");
        }
        auto com_release = [&]() {
            if (com_was_mine) {
                CoUninitialize();
            }
        };
        IShellLinkW* shell_link = nullptr;
        const HRESULT created = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                                 IID_PPV_ARGS(&shell_link));
        if (FAILED(created) || shell_link == nullptr) {
            com_release();
            throw SyscallError("execute_failed",
                               "the shell shortcut resolution failed for '" + app_name +
                                   "': 0x" + std::to_string(created));
        }
        IPersistFile* persist_file = nullptr;
        const HRESULT queried = shell_link->QueryInterface(IID_PPV_ARGS(&persist_file));
        auto close_com = [&]() {
            if (persist_file != nullptr) {
                persist_file->Release();
            }
            shell_link->Release();
            com_release();
        };
        if (FAILED(queried) || persist_file == nullptr) {
            close_com();
            throw SyscallError("execute_failed",
                               "the shell shortcut persistence interface failed for '" +
                                   app_name + "'");
        }
        const HRESULT loaded =
            persist_file->Load(utf8_to_wide(shortcut_path).c_str(), STGM_READ);
        if (FAILED(loaded)) {
            close_com();
            throw SyscallError("not_found",
                               "the shortcut '" + shortcut_path + "' could not be loaded");
        }
        const HRESULT resolved = shell_link->Resolve(nullptr, SLR_NO_UI);
        if (FAILED(resolved)) {
            close_com();
            throw SyscallError("not_found",
                               "the shortcut '" + shortcut_path +
                                   "' does not resolve to an existing target");
        }
        wchar_t target[MAX_PATH] = {};
        const HRESULT got_target = shell_link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH);
        wchar_t arguments[MAX_PATH] = {};
        const HRESULT got_arguments = shell_link->GetArguments(arguments, MAX_PATH);
        close_com();
        if (FAILED(got_target) || target[0] == L'\0') {
            throw SyscallError("not_found",
                               "the shortcut '" + shortcut_path + "' carries no target path");
        }
        if (got_arguments == -1) {
            throw SyscallError("execute_failed",
                               "the shortcut argument tail of '" + app_name +
                                   "' exceeds the readable range");
        }
        argv.push_back(wide_to_utf8(target));
        // The argument tail splits by the SYSTEM argv rules - the same
        // splitting the CRT applies, never shell parsing.
        if (arguments[0] != L'\0') {
            int raw_argc = 0;
            LPWSTR* raw = CommandLineToArgvW(arguments, &raw_argc);
            if (raw == nullptr) {
                throw SyscallError("execute_failed",
                                   "the shortcut argument tail of '" + app_name +
                                       "' could not be parsed into argv");
            }
            for (int i = 0; i < raw_argc; ++i) {
                argv.push_back(wide_to_utf8(raw[i]));
            }
            LocalFree(raw);
        }
        return argv;
    }

    std::vector<std::string> roots_;
    std::shared_ptr<IProcessControl> processes_;
};

std::vector<std::string> standard_roots() {
    // The Start Menu's per-user and all-users Programs trees in their
    // documented environment shapes; an absent variable contributes nothing.
    std::vector<std::string> roots;
    if (const char* appdata = std::getenv("APPDATA")) {
        roots.push_back(std::string(appdata) + "\\Microsoft\\Windows\\Start Menu\\Programs");
    }
    if (const char* program_data = std::getenv("ProgramData")) {
        roots.push_back(std::string(program_data) + "\\Microsoft\\Windows\\Start Menu\\Programs");
    }
    return roots;
}

#else

// ---- Linux: freedesktop .desktop entries ------------------------------------------

// The XDG base-directory roots (data dirs), each with /applications:
// XDG_DATA_HOME (defaults to $HOME/.local/share) first, then XDG_DATA_DIRS
// (defaults /usr/local/share:/usr/share) in order.
std::vector<std::string> standard_roots() {
    std::vector<std::string> roots;
    if (const char* data_home = std::getenv("XDG_DATA_HOME");
        data_home != nullptr && *data_home != '\0') {
        roots.push_back(std::string(data_home) + "/applications");
    } else if (const char* home = std::getenv("HOME")) {
        roots.push_back(std::string(home) + "/.local/share/applications");
    }
    const std::string data_dirs = [dirs = std::getenv("XDG_DATA_DIRS")]() {
        return dirs != nullptr && *dirs != '\0' ? std::string(dirs)
                                                : std::string("/usr/local/share:/usr/share");
    }();
    std::string current;
    auto flush_current = [&] {
        if (!current.empty()) {
            if (current.back() != '/') {
                current += '/';
            }
            roots.push_back(current + "applications");
        }
        current.clear();
    };
    for (const char c : data_dirs) {
        if (c == ':') {
            flush_current();
        } else {
            current.push_back(c);
        }
    }
    flush_current();
    return roots;
}

// Reads Name= / Exec= / Type= out of a .desktop file per the desktop-entry
// spec: comments ride lines starting '#'; localized variants (Name[de]) are
// distinct keys and are not served in this build.
class DesktopEntriesAppLauncher final : public IAppLauncher {
public:
    DesktopEntriesAppLauncher(std::vector<std::string> roots,
                              std::shared_ptr<IProcessControl> processes)
        : roots_(std::move(roots)), processes_(std::move(processes)) {}

    std::vector<AppEntry> list() override {
        std::vector<AppEntry> entries;
        for (const std::string& root : roots_) {
            if (root.empty()) {
                continue;
            }
            std::error_code ec;
            for (const auto& item : std::filesystem::directory_iterator(root, ec)) {
                const std::filesystem::path path = item.path();
                if (path.extension() != ".desktop") {
                    continue;
                }
                std::string name;
                std::string exec;
                std::string type;
                std::string line;
                std::ifstream file(path, std::ios::binary);
                if (!file) {
                    continue;
                }
                while (std::getline(file, line)) {
                    if (line.rfind("Name=", 0) == 0 && name.empty()) {
                        name = line.substr(5);
                    } else if (line.rfind("Exec=", 0) == 0 && exec.empty()) {
                        exec = line.substr(5);
                    } else if (line.rfind("Type=", 0) == 0) {
                        type = line.substr(5);
                    }
                }
                if (type != "Application") {
                    continue; // links and directories are not launchable apps
                }
                if (name.empty()) {
                    name = entry_name_from_path(path.string());
                }
                entries.push_back(AppEntry{name, path.string()});
            }
        }
        sort_entries(entries);
        return entries;
    }

    std::uint32_t start(const std::string& app_name) override {
        // Resolve through the SAME listing the operator saw (deterministic
        // sort; the entry path is plain data from here on). The listing is
        // HELD in a named vector: a pointer into it must outlive this
        // statement (find_exact cannot return into a dropped temporary).
        const std::vector<AppEntry> entries = list();
        const AppEntry* entry = find_exact(entries, app_name);
        // Exec= parsed AS ARGV per the desktop-entry spec (quoting honoured,
        // no /bin/sh); the pieces land verbatim in the process family argv.
        const std::vector<std::string> argv =
            parse_desktop_exec(desktop_exec_value(entry->path));
        if (argv.empty()) {
            throw SyscallError("not_found",
                               "the entry '" + entry->path + "' carries no Exec=");
        }
        if (argv.front().empty()) {
            throw SyscallError("execute_failed",
                               "the Exec= of entry '" + entry->path + "' names no program");
        }
        return processes_->start(argv);
    }

private:
    std::string desktop_exec_value(const std::string& path) const {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            throw SyscallError("not_found", "the entry '" + path + "' could not be loaded");
        }
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.front() != '#' && line.rfind("Exec=", 0) == 0) {
                return line.substr(5);
            }
        }
        return {};
    }

    std::vector<std::string> roots_;
    std::shared_ptr<IProcessControl> processes_;
};

#endif // platform split

} // namespace

std::vector<std::string> parse_desktop_exec(const std::string& exec_value) {
    // Desktop-entry spec, "Exec" grammar: arguments split on unquoted
    // whitespace, "\"-quoted strings honour the \\ \" \` \$ escapes,
    // "\"'-quoted strings allow NO escapes (lab contract: a single quote
    // closes at the next quote, backslash is data), and an unquoted
    // backslash only escapes those same four characters. Percent field
    // codes (% followed by an alpha character) are removed and ignored -
    // this fixed-argv launcher serves no file/URL/name fields. A literal
    // percent rides as %% (the spec's own escape), and % followed by a
    // non-alpha character is plain data. Nothing else is interpreted; no
    // shell is ever involved.
    std::vector<std::string> argv;
    std::string current;
    std::size_t i = 0;
    while (i < exec_value.size()) {
        const char c = exec_value[i];
        if (c == ' ' || c == '\t' || c == '\n') {
            if (!current.empty()) {
                argv.push_back(current);
                current.clear();
            }
            ++i;
            continue;
        }
        if (c == '\'') {
            ++i;
            while (i < exec_value.size() && exec_value[i] != '\'') {
                current.push_back(exec_value[i]);
                ++i;
            }
            if (i < exec_value.size()) {
                ++i; // the closing quote; an unterminated string ends with it
            }
            continue;
        }
        if (c == '"') {
            ++i;
            while (i < exec_value.size() && exec_value[i] != '"') {
                const char inner = exec_value[i];
                if (inner == '\\' && i + 1 < exec_value.size()) {
                    const char escaped = exec_value[i + 1];
                    if (escaped == '\\' || escaped == '"' || escaped == '`' ||
                        escaped == '$') {
                        current.push_back(escaped);
                        i += 2;
                        continue;
                    }
                    current.push_back(inner); // a lone backslash stays literal
                    ++i;
                    continue;
                }
                current.push_back(inner);
                ++i;
            }
            if (i < exec_value.size()) {
                ++i;
            }
            continue;
        }
        if (c == '\\' && i + 1 < exec_value.size()) {
            const char escaped = exec_value[i + 1];
            if (escaped == '\\' || escaped == '"' || escaped == '`' || escaped == '$') {
                current.push_back(escaped);
                i += 2;
                continue;
            }
        }
        if (c == '%') {
            if (i + 1 < exec_value.size()) {
                if (exec_value[i + 1] == '%') {
                    current.push_back('%'); // a literal percent rides as %%
                    i += 2;
                    continue;
                }
                if (std::isalpha(static_cast<unsigned char>(exec_value[i + 1])) != 0) {
                    // A percent + alpha is a field code: this fixed-argv
                    // launcher serves no file/URL/name fields, so per the
                    // spec the code is removed and ignored.
                    i += 2;
                    continue;
                }
                // % followed by a non-alpha character is not a field code:
                // the percent is plain data (inline%20codes stays inline).
            }
            current.push_back('%');
            ++i;
            continue;
        }
        current.push_back(c);
        ++i;
    }
    if (!current.empty()) {
        argv.push_back(current);
    }
    return argv;
}

std::shared_ptr<IAppLauncher> make_app_launcher() {
    return make_app_launcher_with_roots(standard_roots(), make_process_control());
}

std::shared_ptr<IAppLauncher> make_app_launcher_with_processes(
    std::shared_ptr<IProcessControl> processes) {
    return make_app_launcher_with_roots(standard_roots(), std::move(processes));
}

std::shared_ptr<IAppLauncher> make_app_launcher_with_roots(
    std::vector<std::string> roots, std::shared_ptr<IProcessControl> processes) {
#if defined(_WIN32)
    return std::make_shared<WindowsShortcutsAppLauncher>(std::move(roots), std::move(processes));
#else
    return std::make_shared<DesktopEntriesAppLauncher>(std::move(roots), std::move(processes));
#endif
}

} // namespace tether::syscalls
