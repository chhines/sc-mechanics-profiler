#pragma once

#include "analysis/analyzer.h"
#include "analysis/production_visit.h"
#include "platform/automatic_lifecycle.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace smp {

struct RecordingSessionResult {
    AnalysisResult analysis;
    ProductionAnalysis production;
    MacroHotkeyProfile macroHotkeys;
    std::uint64_t qpcFrequency{};
    std::string sessionId;
    std::filesystem::path navPath;
    std::filesystem::path jsonPath;
    std::filesystem::path rawPath;
};

struct AutomaticFinalizationJob {
    std::uint64_t generation{};
    std::optional<RecordingSessionResult> recording;
    ReplayMetadata replayChange;
    bool aborted{};
};

// Owns every accepted job until it finishes. The handler and its session statistics
// have a single owner: this worker. Shutdown drains FIFO and joins, including on unwind.
class AutomaticFinalizationWorker {
  public:
    using Handler = std::function<void(AutomaticFinalizationJob)>;
    using Diagnostic = std::function<void(std::string)>;
    AutomaticFinalizationWorker(Handler handler, Diagnostic diagnostic = {});
    ~AutomaticFinalizationWorker();
    AutomaticFinalizationWorker(const AutomaticFinalizationWorker&) = delete;
    AutomaticFinalizationWorker& operator=(const AutomaticFinalizationWorker&) = delete;

    void enqueue(AutomaticFinalizationJob job);
    void stopAndDrain();

  private:
    void run();
    void log(std::string message) noexcept;
    Handler handler_;
    Diagnostic diagnostic_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<AutomaticFinalizationJob> jobs_;
    bool accepting_{true};
    std::thread thread_;
};

// Called only by the lifecycle/event thread; the worker may read activeGeneration
// for diagnostics, but cannot modify capture state, baselines or recorder ownership.
class AutomaticCaptureCoordinator {
  public:
    using StopRecorder = std::function<std::optional<RecordingSessionResult>()>;
    bool tryStart(const ReplayMetadata& baseline,
                  const std::function<void(std::uint64_t)>& launchRecorder);
    bool tryFinish(std::uint64_t generation, const ReplayMetadata& replay,
                   const StopRecorder& stopRecorder, AutomaticFinalizationWorker& worker,
                   const std::function<void()>& rearmDetector);
    void abort(const StopRecorder& stopRecorder, AutomaticFinalizationWorker& worker);
    [[nodiscard]] std::uint64_t activeGeneration() const { return activeGeneration_.load(); }
  private:
    AutomaticLifecycleState lifecycle_;
    std::uint64_t nextGeneration_{};
    std::atomic<std::uint64_t> activeGeneration_{};
};

} // namespace smp
