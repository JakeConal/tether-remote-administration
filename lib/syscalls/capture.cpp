// lib/syscalls/capture.cpp - the capture family per-OS implementations
// (task 6.5):
//
//   Windows- GDI: GetDC(NULL) + BitBlt (CAPTUREBLT keeps layered windows)
//            of the primary surface, GetDIBits into a TOP-DOWN BGR buffer,
//            PNG encode through image_png (opencv imgcodecs).
//   Linux  - X11: XOpenDisplay($DISPLAY) + XGetImage of the root window,
//            normalized to BGR byte order, PNG-encoded identically. This is
//            the own-library implementation whose benchmark against
//            shelling to `scrot`/`xwd` is recorded in docs/capture-notes.md
//            (the decision: this TU is the production path).
//
// Both build branches live in this TU behind preprocessor gates, as in
// process.cpp / power.cpp. Failure surfaces: no X server -> "execute_failed"
// naming DISPLAY; refused/failed grabs -> "access_denied"/"execute_failed";
// an empty screen surface -> "execute_failed".
#include "capture.hpp"

#include "image_png.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <X11/Xlib.h> // Xlib: XOpenDisplay + XGetImage (the own-lib capture path)
#include <X11/Xutil.h> // XDestroyImage lives here on modern glibc X11 headers
#endif

#include <string>
#include <vector>

namespace tether::syscalls {

// ---- Windows: GDI BitBlt -----------------------------------------------------

#if defined(_WIN32)

class WinGdiScreenCapture final : public IScreenCapture {
public:
    Outcome::Deliverable grab() override;
};

namespace {

Outcome::Deliverable copy_screen_gdi() {
    const HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) {
        throw SyscallError("access_denied", "the screen device context could not be opened");
    }
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    if (width <= 0 || height <= 0) {
        ReleaseDC(nullptr, screen_dc);
        throw SyscallError("execute_failed", "the system reports no screen surface");
    }
    const HDC memory_dc = CreateCompatibleDC(screen_dc);
    const HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, width, height);
    const HGDIOBJ previous = SelectObject(memory_dc, bitmap);
    // SRCCOPY|CAPTUREBLT: full copy including layered windows (the cursor is
    // not composited - the capture wants the frame, not the pointer).
    if (!BitBlt(memory_dc, 0, 0, width, height, screen_dc, 0, 0, SRCCOPY | CAPTUREBLT)) {
        SelectObject(memory_dc, previous);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        throw SyscallError("execute_failed", "the BitBlt screen copy failed");
    }

    // Top-down 32bpp DIB via GetDIBits (negative biHeight forces the row
    // order; the alpha byte is dropped into the BGR Frame below).
    BITMAPINFOHEADER header = {};
    header.biSize = sizeof(header);
    header.biWidth = width;
    header.biHeight = -height; // top-down
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    std::vector<unsigned char> raw(static_cast<std::size_t>(width) * height * 4);
    const int rows = GetDIBits(memory_dc, bitmap, 0, height, raw.data(),
                               reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS);
    if (rows != height) {
        SelectObject(memory_dc, previous);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        throw SyscallError("execute_failed", "GetDIBits could not copy the screen buffer");
    }

    Frame frame;
    frame.width = width;
    frame.height = height;
    frame.bgr.resize(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t in = (static_cast<std::size_t>(y) * width + x) * 4;
            const std::size_t out = (static_cast<std::size_t>(y) * width + x) * 3;
            // GetDIBits with BI_RGB 32bpp yields BGRX byte order.
            frame.bgr[out] = raw[in];
            frame.bgr[out + 1] = raw[in + 1];
            frame.bgr[out + 2] = raw[in + 2];
        }
    }

    SelectObject(memory_dc, previous);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);

    Outcome::Deliverable deliverable;
    deliverable.name = "screenshot.png";
    deliverable.bytes = encode_png_bgr(frame);
    return deliverable;
}

} // namespace

Outcome::Deliverable WinGdiScreenCapture::grab() { return copy_screen_gdi(); }

std::shared_ptr<IScreenCapture> make_screen_capture() {
    return std::make_shared<WinGdiScreenCapture>();
}

// ---- Linux: X11 XGetImage ----------------------------------------------------

#else

class X11ScreenCapture final : public IScreenCapture {
public:
    Outcome::Deliverable grab() override;
};

namespace {

// Swallows and records X protocol errors for the guarded part of the grab:
// without a handler, Xlib's default one PRINTS and TERMINATES the process -
// a single bad grab could wave goodbye to the live agent session. Errors
// that happen inside the guard map to the structured SyscallError below.
int swallow_x_errors(Display*, XErrorEvent*) { return 0; }

// Reads the XImage's pixels (any mask layout the server can hand over; X
// root visuals are 24/32bpp with the channels possibly in either order)
// into the canonical BGR Frame.
Frame frame_from_ximage(XImage* image) {
    Frame frame;
    frame.width = image->width;
    frame.height = image->height;
    frame.bgr.resize(static_cast<std::size_t>(image->width) * image->height * 3);

    // Decompose the masks once - shift offsets via the lowest set bit; an
    // all-zero mask (shouldn't happen for ZPixmap truecolor visuals) falls
    // back to the standard BGR byte positions.
    const unsigned long mask_r = image->red_mask;
    const unsigned long mask_g = image->green_mask;
    const unsigned long mask_b = image->blue_mask;
    int shift_r = 16, shift_g = 8, shift_b = 0;
    if (mask_r != 0 && mask_g != 0 && mask_b != 0) {
        const auto lowest_set = [](unsigned long value) {
            int shift = 0;
            while ((value & (1ul << shift)) == 0) {
                ++shift;
            }
            return shift;
        };
        shift_r = lowest_set(mask_r);
        shift_g = lowest_set(mask_g);
        shift_b = lowest_set(mask_b);
    }

    const int bytes_per_pixel = image->bits_per_pixel / 8;
    for (int y = 0; y < image->height; ++y) {
        const unsigned char* row =
            reinterpret_cast<const unsigned char*>(image->data) +
            static_cast<std::size_t>(y) * image->bytes_per_line;
        std::size_t offset = static_cast<std::size_t>(y) * image->width * 3;
        for (int x = 0; x < image->width; ++x) {
            const unsigned char* pixel = row + static_cast<std::size_t>(x) * bytes_per_pixel;
            // Reassemble the packed pixel LITTLE-ENDIAN over however many
            // bytes the visual gives (2/3/4), then split it by the masks.
            // Byte-count exactness matters: never read past the pixel.
            unsigned long packed = 0;
            for (int byte = 0; byte < bytes_per_pixel && byte < 4; ++byte) {
                packed |= static_cast<unsigned long>(static_cast<unsigned char>(pixel[byte]))
                          << (8 * byte);
            }
            const unsigned char r = static_cast<unsigned char>((packed & mask_r) >> shift_r);
            const unsigned char g = static_cast<unsigned char>((packed & mask_g) >> shift_g);
            const unsigned char b = static_cast<unsigned char>((packed & mask_b) >> shift_b);
            frame.bgr[offset] = b;
            frame.bgr[offset + 1] = g;
            frame.bgr[offset + 2] = r;
            offset += 3;
        }
    }
    return frame;
}

Outcome::Deliverable grab_root_x11() {
    // $DISPLAY (or the compiled default): its absence is the structured
    // refusal for capture on a headless host - never a wedge, never a
    // shell call, never a silent no-op.
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        throw SyscallError(
            "execute_failed",
            "no X display is reachable (is DISPLAY set?): the capture family is X11 on Linux; "
            "Wayland has no capture parity here");
    }
    // Guard the protocol errors of this grab: ask Xlib not to terminate on
    // a BadMatch etc.; the failing request surfaces as a null result below.
    // The handler is restored after the synchronous round trips finished
    // (an XGetImage error reaches us during the call; XSync flushed the
    // async CopyArea errors beforehand).
    const XErrorHandler previous_error_handler = XSetErrorHandler(swallow_x_errors);
    XImage* image = nullptr;
    {
        const int screen = DefaultScreen(display);
        const Window root = RootWindow(display, screen);
        XWindowAttributes attributes = {};
        if (XGetWindowAttributes(display, root, &attributes) && attributes.width > 0 &&
            attributes.height > 0) {
            // Grab through a temporary PIXMAP (an in-process copy onto a
            // pixmap via XCopyArea, then XGetImage on that pixmap).
            // Composited roots - XWayland/WSLg being the canonical case -
            // refuse a direct root XGetImage with BadMatch, while an
            // own-pixmap grab is the same depth and always plain
            // memory-backed. This is the own-library path whose benchmark
            // lives in docs/capture-notes.md.
            const Pixmap snapshot =
                XCreatePixmap(display, root, attributes.width, attributes.height,
                              attributes.depth);
            const GC graphics_context = XCreateGC(display, snapshot, 0, nullptr);
            if (snapshot != None && graphics_context != nullptr) {
                XCopyArea(display, root, snapshot, graphics_context, 0, 0, attributes.width,
                          attributes.height, 0, 0);
                XSync(display, False); // flush + swallow the async CopyArea errors
                image = XGetImage(display, snapshot, 0, 0, attributes.width, attributes.height,
                                  AllPlanes, ZPixmap);
            }
            if (graphics_context != nullptr) {
                XFreeGC(display, graphics_context);
            }
            if (snapshot != None) {
                XFreePixmap(display, snapshot);
            }
        }
    }
    XSetErrorHandler(previous_error_handler);
    if (image == nullptr) {
        XCloseDisplay(display);
        throw SyscallError(
            "execute_failed",
            "the X11 snapshot grab failed (bad geometry, BadMatch on the copy, or a refused "
            "GetImage)");
    }
    const Frame frame = frame_from_ximage(image);
    XDestroyImage(image);
    XCloseDisplay(display);

    Outcome::Deliverable deliverable;
    deliverable.name = "screenshot.png";
    deliverable.bytes = encode_png_bgr(frame);
    return deliverable;
}

} // namespace

Outcome::Deliverable X11ScreenCapture::grab() { return grab_root_x11(); }

std::shared_ptr<IScreenCapture> make_screen_capture() {
    return std::make_shared<X11ScreenCapture>();
}

#endif

} // namespace tether::syscalls
