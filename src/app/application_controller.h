#pragma once

#include "cli/commands.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace smp {

enum class ApplicationMode {
    None,
    Automatic,
    Debug,
    Calibration,
};

struct ApplicationSnapshot {
    ApplicationMode mode{ApplicationMode::None};
    ProfilerActivity activity{ProfilerActivity::Idle};
    bool workerRunning{};
    std::string detail{"Profiler is idle"};
    std::string error;
    std::vector<std::string> diagnostics;
    std::optional<json::Value> latestGame;
    std::filesystem::path latestGamePath;
    AutomaticSessionStats currentSession;
};

class ApplicationController {
  public:
    using StateChanged = std::function<void()>;

    explicit ApplicationController(std::filesystem::path workingDirectory);
    ~ApplicationController();
    ApplicationController(const ApplicationController&) = delete;
    ApplicationController& operator=(const ApplicationController&) = delete;

    void setStateChanged(StateChanged callback);
    [[nodiscard]] ApplicationSnapshot snapshot() const;
    [[nodiscard]] bool startAutomatic(Config config,
                                      ReportGroupVisibility reportVisibility);
    [[nodiscard]] bool startDebug(Config config);
    [[nodiscard]] bool startCalibration(Config config,
                                        const std::filesystem::path& configPath);
    void stopCurrent() noexcept;
    void reapFinished();
    void shutdown() noexcept;

  private:
    [[nodiscard]] bool prepareStart(ApplicationMode mode, ProfilerActivity activity,
                                    std::string detail);
    ProfilerCallbacks callbacks();
    void setStatus(ProfilerActivity activity, std::string detail);
    void addDiagnostic(std::string line);
    void completeGame(const json::Value& summary, const std::filesystem::path& path,
                      const AutomaticSessionStats& session);
    void updateSession(const AutomaticSessionStats& session);
    void finishWorker(std::string error = {});
    void notifyChanged() const;
    void loadLatestGame() noexcept;

    std::filesystem::path workingDirectory_;
    mutable std::mutex mutex_;
    ApplicationSnapshot state_;
    StateChanged stateChanged_;
    std::thread worker_;
    std::atomic<bool> workerRunning_{false};
    std::atomic<bool> calibrationRequested_{false};
};

[[nodiscard]] const char* applicationModeName(ApplicationMode mode) noexcept;
[[nodiscard]] const char* profilerActivityName(ProfilerActivity activity) noexcept;

} // namespace smp
