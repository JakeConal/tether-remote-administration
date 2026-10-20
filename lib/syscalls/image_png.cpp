// lib/syscalls/image_png.cpp - encode/decode behind opencv4's imgcodecs
// (cv::imencode / cv::imdecode). Include paths come through the opencv
// CMake target wiring done in lib/syscalls/CMakeLists.txt.
#include "image_png.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cerrno>
#include <string>

namespace tether::syscalls {

bool looks_like_png(const std::vector<unsigned char>& bytes) {
    // The 8-byte PNG signature: 0x89 'P' 'N' 'G' 0x0D 0x0A 0x1A 0x0A.
    if (bytes.size() < 8) {
        return false;
    }
    static const unsigned char kPngSignature[8] = {0x89u, 0x50u, 0x4Eu, 0x47u,
                                                   0x0Du, 0x0Au, 0x1Au, 0x0Au};
    for (int i = 0; i < 8; ++i) {
        if (bytes[i] != kPngSignature[i]) {
            return false;
        }
    }
    return true;
}

std::vector<unsigned char> encode_png_bgr(const Frame& frame) {
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.bgr.size() != static_cast<std::size_t>(frame.width) * frame.height * 3) {
        throw SyscallError("invalid_args",
                           "cannot encode a " + std::to_string(frame.width) + "x" +
                               std::to_string(frame.height) + " frame of " +
                               std::to_string(frame.bgr.size()) + " BGR bytes");
    }
    const cv::Mat image(frame.height, frame.width, CV_8UC3,
                        // Mat does not own: it wraps the caller's bytes for
                        // the encode call only (no copy, no retained ref).
                        const_cast<unsigned char*>(frame.bgr.data()),
                        static_cast<std::size_t>(frame.width) * 3);
    std::vector<unsigned char> png;
    const std::vector<int> none;
    if (!cv::imencode(".png", image, png, none)) {
        throw SyscallError("execute_failed", "PNG encoding failed for the captured frame");
    }
    return png;
}

Frame decode_png(const std::vector<unsigned char>& png) {
    if (!looks_like_png(png)) {
        throw SyscallError("invalid_args", "the bytes are not a PNG file");
    }
    const cv::Mat decoded = cv::imdecode(png, cv::IMREAD_COLOR);
    if (decoded.empty() || decoded.cols <= 0 || decoded.rows <= 0 ||
        decoded.type() != CV_8UC3) {
        // "empty" is the only mode imdecode reports a bad file with.
        throw SyscallError("invalid_args", "the bytes are not a decodable image file");
    }
    Frame frame;
    frame.width = decoded.cols;
    frame.height = decoded.rows;
    frame.bgr.assign(decoded.data, decoded.data + decoded.total() * 3);
    return frame;
}

} // namespace tether::syscalls
