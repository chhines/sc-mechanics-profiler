#include "cli/automatic_recording.h"

#include <stdexcept>
#include <utility>

namespace smp {

AutomaticFinalizationWorker::AutomaticFinalizationWorker(Handler handler, Diagnostic diagnostic)
    : handler_(std::move(handler)), diagnostic_(std::move(diagnostic)),
      thread_([this]() { run(); }) {}

AutomaticFinalizationWorker::~AutomaticFinalizationWorker() { stopAndDrain(); }

void AutomaticFinalizationWorker::log(std::string message) noexcept {
    try {
        if (diagnostic_) diagnostic_(std::move(message));
    } catch (...) {
        // A diagnostic consumer cannot kill the worker or discard accepted jobs.
    }
}

void AutomaticFinalizationWorker::enqueue(AutomaticFinalizationJob job) {
    const auto generation = job.generation;
    std::size_t depth;
    {
        std::scoped_lock lock(mutex_);
        if (!accepting_) throw std::logic_error("Finalization worker is closed");
        jobs_.push_back(std::move(job));
        depth = jobs_.size();
    }
    log("FINALIZATION_QUEUED generation=" + std::to_string(generation) +
        " queue_depth=" + std::to_string(depth));
    if (depth > 1)
        log("FINALIZATION_BACKLOG queue_depth=" + std::to_string(depth));
    ready_.notify_one();
}

void AutomaticFinalizationWorker::stopAndDrain() {
    {
        std::scoped_lock lock(mutex_);
        accepting_ = false;
    }
    ready_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void AutomaticFinalizationWorker::run() {
    for (;;) {
        AutomaticFinalizationJob job;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this]() { return !jobs_.empty() || !accepting_; });
            if (jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        const auto generation = job.generation;
        log("FINALIZATION_STARTED generation=" + std::to_string(generation));
        try {
            handler_(std::move(job));
            log("FINALIZATION_COMPLETED generation=" + std::to_string(generation));
        } catch (const std::exception& error) {
            log("FINALIZATION_FAILED generation=" + std::to_string(generation) + " reason=" + error.what());
        } catch (...) {
            log("FINALIZATION_FAILED generation=" + std::to_string(generation) + " reason=unknown");
        }
    }
}

bool AutomaticCaptureCoordinator::tryStart(
    const ReplayMetadata& baseline, const std::function<void(std::uint64_t)>& launchRecorder) {
    if (!lifecycle_.tryStart(baseline)) return false;
    const auto generation = ++nextGeneration_;
    activeGeneration_.store(generation);
    launchRecorder(generation);
    return true;
}

bool AutomaticCaptureCoordinator::tryFinish(
    std::uint64_t generation, const ReplayMetadata& replay,
    const StopRecorder& stopRecorder, AutomaticFinalizationWorker& worker,
    const std::function<void()>& rearmDetector,
    const std::function<PinnedReplaySource()>& pinReplay) {
    if (generation != activeGeneration() || !lifecycle_.tryStop(replay)) return false;
    // Pin immediately on acceptance, before recorder joining or a queued finalizer
    // can delay consumption. This opens only; all settling/copying stays in the worker.
    auto replaySource = pinReplay ? pinReplay() : PinnedReplaySource{};
    auto recording = stopRecorder(); // Joins the old recorder before any new capture can start.
    activeGeneration_.store(0);
    worker.enqueue({generation, std::move(recording), replay, false, std::move(replaySource)});
    rearmDetector(); // Never waits for any work in the finalizer.
    return true;
}

void AutomaticCaptureCoordinator::abort(const StopRecorder& stopRecorder,
                                         AutomaticFinalizationWorker& worker) {
    lifecycle_.forceStop();
    const auto generation = activeGeneration();
    auto recording = stopRecorder();
    activeGeneration_.store(0);
    if (generation != 0) worker.enqueue({generation, std::move(recording), {}, true});
}

} // namespace smp
