// lib/syscalls/camera.cpp - the camera family implementation (task 6.6):
// the production frame source is cv::VideoCapture (opencv4 videoio), frames
// are PNG-encoded through image_png (shared with the capture family), and
// the job lifecycle is the SHARED registry mechanics (jobs.hpp/cpp) driving
// one dedicated worker thread. The worker loop is deliberately simple:
// open the source, then read -> encode -> publish (the accumulated latest
// frame) -> pace, stopping on the registry's signal; any source failure is
// recorded on the handle and surfaces through stop_job's structured result
// ("execute_failed") - never a wedged session, never a silent success.
#include "camera.hpp"

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace tether::syscalls {

namespace {

// Production pacing: one frame per this interval. stop() requests the
// stop flag before the sleep is re-entered, so the join bound is one
// read+encode (~tens of ms), never the full sleep.
constexpr std::chrono::milliseconds kFrameInterval{250};

// The single deliverable name the camera job publishes.
constexpr const char* kDeliverableName = "webcam.png";

JobDeliverable ready_deliverable(std::vector<unsigned char> png_bytes) {
    JobDeliverable deliverable;
    deliverable.ready = true;
    deliverable.name = kDeliverableName;
    deliverable.bytes = std::move(png_bytes);
    return deliverable;
}

// Publishes one encoded PNG frame through the job handle (the accumulated
// latest-state state the registry hands back at stop).
void encode_publish(JobHandle& handle, const Frame& frame) {
    handle.publish(ready_deliverable(encode_png_bgr(frame)));
}

} // namespace

// ---- the production frame source (opencv videoio) ---------------------------

std::shared_ptr<IFrameSource> make_video_capture_source(const int device_index) {
    class VideoCaptureSource final : public IFrameSource {
    public:
        explicit VideoCaptureSource(const int device_index) : device_index_(device_index) {}

        bool open() override {
            // CAP_ANY: the platform default backend (DirectShow/MSMF on
            // Windows, V4L2 on Linux) - no hint, no codec dependency.
            capture_.open(device_index_, cv::CAP_ANY);
            return capture_.isOpened();
        }

        bool read(Frame& out) override {
            cv::Mat frame;
            if (!capture_.read(frame)) {
                return false; // device disconnected / no more frames
            }
            if (frame.depth() != CV_8U || (frame.channels() != 1 && frame.channels() != 3 &&
                                           frame.channels() != 4)) {
                // Something the standard device backends never send: fail
                // LOUDLY with a classified SyscallError (the registry's
                // worker wrapper records `what()` on the job handle) rather
                // than pretending the frame was captured.
                throw SyscallError("execute_failed",
                                   "the camera source returned an unsupported frame type (" +
                                       std::to_string(frame.depth()) + "/" +
                                       std::to_string(frame.channels()) + "ch)");
            }
            out = *normalize_to_bgr(frame);
            return true;
        }

        void close() override {
            if (capture_.isOpened()) {
                capture_.release();
            }
        }

    private:
        // Normalizes any videoio frame type the standard device backends
        // produce (8UC1/8UC3/8UC4/16U) into the BGR-8U Frame layout. Plain
        // loops (no imgproc dependency): width*height work only.
        static std::optional<Frame> normalize_to_bgr(const cv::Mat& input) {
            if (input.empty() || input.cols <= 0 || input.rows <= 0) {
                return std::nullopt;
            }
            Frame frame;
            frame.width = input.cols;
            frame.height = input.rows;
            frame.bgr.resize(static_cast<std::size_t>(frame.width) * frame.height * 3);
            auto* dst = frame.bgr.data();
            const int channels = input.channels();
            for (int y = 0; y < input.rows; ++y) {
                const auto* src = input.ptr<unsigned char>(y);
                std::size_t offset = static_cast<std::size_t>(y) * frame.width * 3;
                for (int x = 0; x < frame.width; ++x) {
                    const auto* pixel = src + static_cast<std::size_t>(x) * channels;
                    if (channels >= 3) {
                        // BGR on OpenCV device backends; alpha dropped when 4.
                        dst[offset]     = pixel[0];
                        dst[offset + 1] = pixel[1];
                        dst[offset + 2] = pixel[2];
                    } else {
                        // Grayscale: replicate into the three planes.
                        dst[offset] = dst[offset + 1] = dst[offset + 2] = pixel[0];
                    }
                    offset += 3;
                }
            }
            return frame;
        }

        int device_index_;
        cv::VideoCapture capture_;
    };
    return std::make_shared<VideoCaptureSource>(device_index);
}

// ---- the family implementation over the shared registry ---------------------

void VideoCameraCapture::start_job() {
    // The raw worker: owns its source; publishes the ENCODED latest frame
    // every cycle so stop_job hands out an always-decodable deliverable.
    auto worker = [factory = factory_, waiter = waiter_](JobHandle& handle) {
        std::shared_ptr<IFrameSource> source = factory();
        if (source == nullptr || !source->open()) {
            handle.fail("the camera device could not be opened");
            return;
        }
        Frame frame;
        for (;;) {
            if (handle.stop_requested()) {
                break;
            }
            if (!source->read(frame)) {
                handle.fail("the camera source stopped delivering frames");
                break;
            }
            encode_publish(handle, frame);
            if (handle.stop_requested()) {
                break;
            }
            waiter(); // pace; the registry flags stop inside this window
        }
        source->close();
    };
    if (!registry_->start(BackgroundJobRegistry::kCameraSlot, std::move(worker))) {
        // Duplicate start: the already-running job is untouched (registry
        // refuses without disturbing the incumbent).
        throw SyscallError(
            "already_running",
            "start_webcam while a camera job is already running; the running job kept its slot");
    }
}

Outcome::Deliverable as_outcome_deliverable(const JobDeliverable& deliverable) {
    Outcome::Deliverable result;
    result.name = deliverable.name;
    result.bytes = deliverable.bytes;
    return result;
}

Outcome::Deliverable VideoCameraCapture::stop_job() {
    const BackgroundJobRegistry::StopOutcome outcome =
        registry_->stop(BackgroundJobRegistry::kCameraSlot);
    if (!outcome.was_running) {
        throw SyscallError(
            "not_running",
            "stop_webcam arrived with no camera job running; nothing was executed");
    }
    if (outcome.failure.has_value()) {
        throw SyscallError("execute_failed", "the camera job failed: " + *outcome.failure);
    }
    if (!outcome.deliverable.has_value() || !outcome.deliverable->ready) {
        throw SyscallError(
            "execute_failed",
            "the camera job produced no frames before stopping; failed loudly instead of "
            "apparently succeeding");
    }
    return as_outcome_deliverable(*outcome.deliverable);
}

// ---- production factories ----------------------------------------------------

std::shared_ptr<ICameraCapture> make_camera_capture() {
    return make_camera_capture_with_registry(shared_job_registry());
}

std::shared_ptr<ICameraCapture> make_camera_capture_with_registry(
    std::shared_ptr<BackgroundJobRegistry> registry) {
    return std::make_shared<VideoCameraCapture>(
        []() { return make_video_capture_source(0); }, std::move(registry),
        []() { std::this_thread::sleep_for(kFrameInterval); });
}

} // namespace tether::syscalls
