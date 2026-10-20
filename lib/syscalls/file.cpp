// lib/syscalls/file.cpp - the file family over std::filesystem (task 6.3).
// A single implementation serves both operating systems (the binding above
// the design table's Win32 row): every operation is a direct filesystem
// call - no shell, no tool-splice - and std::error_code results map onto the
// stable SyscallError codes so failures stay structured on the wire.
//
// UTF-8 <-> path conversion: arguments arrive as UTF-8 bytes on the wire; on
// Windows the path(text) constructor would decode them with the ANSI code
// page, so conversion goes through fs::u8path (and generic_u8string back),
// keeping names intact on both OSes.
#include "file.hpp"

#include <filesystem>
#include <system_error>

#include <algorithm>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace tether::syscalls {

namespace {

[[noreturn]] void throw_mapped(const std::error_code& ec, const std::string& operation) {
    // Map the common errc equivalents once (same mapping serves both OSes:
    // MSVC maps the Windows system codes onto the generic errc set).
    if (ec == std::errc::no_such_file_or_directory) {
        throw SyscallError("not_found", "no such path: " + operation + " (" + ec.message() + ")");
    }
    if (ec == std::errc::not_a_directory) {
        throw SyscallError("not_found", "not a directory: " + operation);
    }
    if (ec == std::errc::permission_denied) {
        throw SyscallError("access_denied", "access denied: " + operation + " (" + ec.message() + ")");
    }
    if (ec == std::errc::directory_not_empty) {
        throw SyscallError("directory_not_empty", "directory not empty: " + operation);
    }
    if (ec == std::errc::file_exists) {
        throw SyscallError("already_exists", "path exists: " + operation);
    }
    if (ec == std::errc::cross_device_link) {
        throw SyscallError("cross_device", "cross-device operation refused: " + operation);
    }
    throw SyscallError("execute_failed", operation + " failed (" + ec.message() + ")");
}

std::filesystem::path to_path(const std::string& utf8) {
    return fs::u8path(utf8); // explicit UTF-8 decoding on Windows too
}

} // namespace

namespace {

class FilesystemControl final : public IFileSystem {
public:
    std::vector<FileEntry> list(const std::string& path_utf8) override {
        const fs::path directory = to_path(path_utf8);
        std::error_code ec;
        if (!fs::exists(directory, ec) || ec) {
            throw SyscallError("not_found", "no such directory: '" + path_utf8 + "'");
        }
        if (!fs::is_directory(directory, ec) || ec) {
            throw SyscallError("not_found", "not a directory: '" + path_utf8 + "'");
        }
        std::vector<FileEntry> entries;
        fs::directory_iterator it(directory, ec);
        if (ec) { // constructor errors (unreadable dir) would otherwise read as empty
            throw_mapped(ec, "listing '" + path_utf8 + "'");
        }
        for (fs::directory_iterator end; it != end; it.increment(ec)) {
            if (ec) {
                throw_mapped(ec, "listing '" + path_utf8 + "'");
            }
            FileEntry entry;
            entry.name = it->path().filename().generic_u8string();
            entry.is_directory = it->is_directory(ec);
            if (ec) {
                throw_mapped(ec, "stating '" + path_utf8 + "/" + entry.name + "'");
            }
            if (!entry.is_directory) {
                entry.size = it->file_size(ec);
                if (ec) {
                    throw_mapped(ec, "stating '" + path_utf8 + "/" + entry.name + "'");
                }
            }
            entries.push_back(std::move(entry));
        }
        std::sort(entries.begin(), entries.end(),
                  [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
        return entries;
    }

    void remove(const std::string& path_utf8) override {
        const fs::path target = to_path(path_utf8);
        std::error_code ec;
        if (!fs::exists(target, ec) || ec) {
            throw SyscallError("not_found", "no such path: '" + path_utf8 + "'");
        }
        const bool removed = fs::remove(target, ec); // file or single (empty) directory
        if (ec) {
            throw_mapped(ec, "delete '" + path_utf8 + "'");
        }
        if (!removed) {
            // The existence probe raced a concurrent removal: still a
            // structured, honest not-found.
            throw SyscallError("not_found", "no such path (vanished during removal): '" +
                                                path_utf8 + "'");
        }
    }

    void move(const std::string& from_utf8, const std::string& to_utf8) override {
        const fs::path from = to_path(from_utf8);
        const fs::path to = to_path(to_utf8);
        std::error_code ec;
        if (!fs::exists(from, ec) || ec) {
            throw SyscallError("not_found", "no such path: '" + from_utf8 + "'");
        }
        fs::rename(from, to, ec); // replaces an existing file destination
        if (ec) {
            throw_mapped(ec, "move '" + from_utf8 + "' -> '" + to_utf8 + "'");
        }
    }

    void copy(const std::string& from_utf8, const std::string& to_utf8) override {
        const fs::path from = to_path(from_utf8);
        const fs::path to = to_path(to_utf8);
        std::error_code ec;
        if (!fs::exists(from, ec) || ec) {
            throw SyscallError("not_found", "no such path: '" + from_utf8 + "'");
        }
        const fs::copy_options options =
            fs::copy_options::overwrite_existing | fs::copy_options::recursive;
        fs::copy(from, to, options, ec); // files copy byte-exact; dirs recurse
        if (ec) {
            throw_mapped(ec, "copy '" + from_utf8 + "' -> '" + to_utf8 + "'");
        }
    }
};

} // namespace

std::shared_ptr<IFileSystem> make_file_system() {
    return std::make_shared<FilesystemControl>();
}

} // namespace tether::syscalls
