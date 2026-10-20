// lib/syscalls/file.hpp - the file family interface (design D6, task 6.3).
// One std::filesystem implementation serves both operating systems (the task
// binding pins this above the design table's Win32 row: all four operations
// are direct filesystem calls with no OS-specific backend needed). Paths and
// names arrive as UTF-8 data and are used as literal path text - no shell or
// glob expansion happens anywhere, so arguments containing shell
// metacharacters look up the literal name (remote-commands delta).
//
// All methods may throw SyscallError (errors.hpp) carrying a stable short
// code ("not_found", "access_denied", "directory_not_empty", "already_exists",
// "cross_device", "execute_failed", "invalid_args"); the dispatcher turns
// those into the structured Outcome the agent reports over the protocol.
#pragma once

#include "errors.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tether::syscalls {

// One directory entry: name (the file/directory name only, never a full
// path), byte size (always 0 for directories), and the kind flag.
struct FileEntry {
    std::string name;
    std::uint64_t size = 0;
    bool is_directory = false;
};

class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    // Lists the entries of `path` (a directory); entries are sorted by name
    // so listings are deterministic. A missing path or one that is not a
    // directory throws "not_found"; an unreadable one "access_denied".
    virtual std::vector<FileEntry> list(const std::string& path) = 0;

    // Removes a file (or a single empty directory). Recursive tree deletion
    // is deliberately not offered from this surface. A missing path throws
    // "not_found"; a non-empty directory "directory_not_empty".
    virtual void remove(const std::string& path) = 0;

    // Renames (atomically, where the platform allows) `from` to `to`,
    // replacing an existing file destination. A missing source throws
    // "not_found"; a cross-device move "cross_device".
    virtual void move(const std::string& from, const std::string& to) = 0;

    // Copies `from` to `to`, replacing an existing file destination;
    // directories copy recursively with their content, overwriting targets.
    // A missing source throws "not_found".
    virtual void copy(const std::string& from, const std::string& to) = 0;
};

// Builds the platform implementation (single std::filesystem backend for both
// operating systems, as pinned by the task 6.3 binding).
std::shared_ptr<IFileSystem> make_file_system();

} // namespace tether::syscalls
