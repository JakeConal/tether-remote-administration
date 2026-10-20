// lib/syscalls/capture.hpp - the capture family interface (design D6, task
// 6.5). One verb (`screenshot`): grab the screen into a complete, decodable
// PNG file deliverable that streams through the session's file channel.
//
// Per-OS backends (remote-commands delta; Wayland has no capture parity per
// design D6 and resolves through this interface's structured errors):
//   Windows- GDI: GetDC(NULL) + BitBlt of the virtual screen, GetDIBits into
//            a top-down BGR buffer, PNG-encoded with opencv imgcodecs.
//   Linux  - X11: XOpenDisplay + XGetImage of the root window (Xlib),
//            normalized to BGR, PNG-encoded the same way. A host without an
//            X server refuses with a structured execute_failed noting
//            $DISPLAY - never a wedge, never a silent no-op.
//
// All methods may throw SyscallError (errors.hpp) with a stable short code;
// the dispatcher turns failures into the structured Outcome the agent sends.
#pragma once

#include "errors.hpp"
#include "matrix.hpp" // Outcome::Deliverable (the file-result shape)

#include <memory>

namespace tether::syscalls {

class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;

    // Grabs the screen; returns the byte-complete PNG deliverable (name
    // "screenshot.png"). Missing X server / refused GDI access / encode
    // failure raises a classified SyscallError.
    virtual Outcome::Deliverable grab() = 0;
};

// Builds the platform implementation (Windows GDI or Linux X11, decided at
// compile time) for the wired agent. Tests inject fakes at the dispatcher
// level; the REAL implementation is exercised by the per-platform smoke
// tests desktop/xvfb give each CI leg.
std::shared_ptr<IScreenCapture> make_screen_capture();

} // namespace tether::syscalls
