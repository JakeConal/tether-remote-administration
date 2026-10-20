// lib/syscalls/jobs.hpp - the shared managed background-job lifecycle
// (remote-commands delta: "background jobs have a managed lifecycle"; task
// 6.6 binds it as SHARED so the 6.7 keylogger family plugs into the same
// registry, one named slot per job family).
//
// Shape:
//  - `start(slot, worker)` seats a worker on its own thread or reports the
//    slot occupied (duplicate start is refused by the caller with the
//    structured already-running result - the incumbent untouched);
//  - `stop(slot)` signals, joins, and hands back the deliverable the worker
//    published ("job stop releases and delivers the accumulated capture");
//  - a stop on an empty slot reports not-running (same translation rule).
//
// The session stays responsive while a job runs: the worker owns its capture
// loop and never takes the session's locks; deliverables cross threads
// through the handle's mutex.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tether::syscalls {

// A job result: the name the file result reports and its complete bytes.
// `ready` is false when the job produced no deliverable (the caller refuses
// with a structured failure instead of an apparently-successful empty one).
struct JobDeliverable {
    bool ready = false;
    std::string name;
    std::vector<unsigned char> bytes;
};

// The control handle one worker thread sees. Workers publish their
// accumulated result through `publish` (thread-safe, only the bounded,
// latest state is kept) and check `stop_requested()` between steps; the
// registry drives both ends.
class JobHandle {
public:
    void request_stop();                 // wind-down signal (idempotent)
    bool stop_requested() const;

    void publish(JobDeliverable deliverable);        // overwrite-and-latest
    std::optional<JobDeliverable> latest() const;    // empty when nothing published

    // Marks a worker-side failure (the deliverable stays absent; the reason
    // surfaces through the registry's stop outcome).
    void fail(const std::string& reason);
    std::optional<std::string> failure() const;

private:
    friend class BackgroundJobRegistry;

    std::atomic<bool> requested_{false};

    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    std::optional<JobDeliverable> deliverable_;
    std::optional<std::string> failure_;
};

class BackgroundJobRegistry {
public:
    // `Worker` runs on its OWN thread for the job's lifetime; it must check
    // the handle's stop flag between steps and must not outlive its handle.
    using Worker = std::function<void(JobHandle&)>;

    BackgroundJobRegistry() = default;
    ~BackgroundJobRegistry(); // joins every still-running job

    BackgroundJobRegistry(const BackgroundJobRegistry&) = delete;
    BackgroundJobRegistry& operator=(const BackgroundJobRegistry&) = delete;

    // Spawns the worker for `slot`. Returns false WITHOUT touching the
    // incumbent when the slot is already running (the duplicate-start
    // refusal; the caller reports the structured error). Returns true when
    // the job is now running in the background.
    bool start(const std::string& slot, Worker worker);

    struct StopOutcome {
        bool was_running = false;
        std::optional<JobDeliverable> deliverable; // the accumulated result
        std::optional<std::string> failure;        // set when the worker failed
    };

    // Signals + joins the slot's job (if any). An empty slot yields an
    // unfulfilled outcome (`was_running == false` - the caller's
    // not-running refusal). Otherwise the worker's latest publish rides
    // `deliverable`, a worker-side failure surfaces as `failure`.
    StopOutcome stop(const std::string& slot);

    bool running(const std::string& slot) const;

    // Named slots; the camera family claims "camera" (task 6.6) and the
    // 6.7 keylogger plugs into "input" without registry changes.
    static constexpr const char* kCameraSlot = "camera";
    static constexpr const char* kInputSlot = "input";
private:
    struct RunningJob {
        std::shared_ptr<JobHandle> handle;
        std::thread thread;
    };

    std::map<std::string, std::shared_ptr<RunningJob>> jobs_; // guarded by mutex_
    mutable std::mutex mutex_;
};

// The agent-process-wide registry instance: the camera family registers here
// today and the 6.7 keylogger family plugs into the SAME instance with its
// own slot (the shared-lifecycle binding of task 6.6). Tests create their
// own instances instead of touching this.
std::shared_ptr<BackgroundJobRegistry> shared_job_registry();

} // namespace tether::syscalls
