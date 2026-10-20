// lib/syscalls/image_png.hpp - the one PNG encode/decode helper the capture
// and camera families share (task 6.5/6.6: opencv4 imgcodecs is the
// dependency; cv::imencode does the work, this wrapper owns the Frame layout
// and the strict check that a produced/produced-and-consumed buffer is a
// real decodable image, not just bytes).
#pragma once

#include "errors.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tether::syscalls {

// A raw BGR frame (the layout cv::VideoCapture produces; X11/GDI byte order
// is normalized into it).
struct Frame {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> bgr; // width * height * 3 bytes
};

// Encodes `frame` as a complete PNG file. A zero-size or unsized frame
// throws "invalid_args"; an encode failure (implausible) "execute_failed".
std::vector<unsigned char> encode_png_bgr(const Frame& frame);

// Decodes a complete PNG back into a Frame (used by tests to prove a
// produced image is a decodable file with real pixels). A decode failure
// throws "invalid_args" (the input was not an image file).
Frame decode_png(const std::vector<unsigned char>& png);

// The PNG file signature check used before decode attempts trace-deep
// errors; cheap and exact.
bool looks_like_png(const std::vector<unsigned char>& bytes);

} // namespace tether::syscalls
