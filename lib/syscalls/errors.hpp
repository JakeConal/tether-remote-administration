// lib/syscalls/errors.hpp - the syscall-layer error taxonomy (design D6,
// remote-commands delta). Every failing capability operation throws
// SyscallError carrying a stable short code plus a human-readable message;
// the dispatcher (matrix.hpp) converts it into the structured Outcome the
// agent reports back over the protocol (task 6.1: every command resolves to
// a definitive outcome - success or a classified failure, never an
// unclassified one).
//
// Codes (stable vocabulary, shared with the dispatcher's Outcome::code):
//   invalid_args   parameters malformed for the operation
//   not_found      target of the lookup/operation does not exist
//   access_denied  the platform refused the operation on this target
//   cross_device   a move/copy across devices/filesystems was refused
//   execute_failed the OS call failed in a way the other codes do not name
#pragma once

#include <stdexcept>
#include <string>

namespace tether::syscalls {

class SyscallError : public std::runtime_error {
public:
    // `code` is the stable short code; `message` the human-readable detail.
    // Both survive into the wire-visible outcome verbatim.
    SyscallError(std::string code, const std::string& message)
        : std::runtime_error(message), code_(std::move(code)) {}

    const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

} // namespace tether::syscalls
