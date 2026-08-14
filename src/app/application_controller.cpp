#include "app/application_controller.h"

#include "cli/calibration.h"
#include "storage/session.h"

#include <utility>

namespace smp {

const char* applicationModeName(ApplicationMode mode) noexcept {
    switch (mode) {
    case ApplicationMode::None: return "Idle";
    case ApplicationMode::Automatic: return "Automatic detector";
    case ApplicationMode::Debug: return "Live detection test";
    case ApplicationMode::Calibration: return "Minimap calibration";
    }
    return "Idle";
}

const char* profilerActivityName(ProfilerActivity activity) noexcept {
    switch (activity) {
    case ProfilerActivity::Idle: return "Idle";
    case ProfilerActivity::WaitingForGame: return "Waiting for game";
    case ProfilerActivity::WaitingForStarCraft: return "Waiting for StarCraft";
    case ProfilerActivity::Recording: return "Recording";
    case ProfilerActivity::Paused: return "Paused";
    case ProfilerActivity::AnalyzingReplay: return "Analyzing replay";
    case ProfilerActivity::Calibrating: return "Calibrating";
    case ProfilerActivity::Error: return "Error";
    }
    return "Idle";
}

ApplicationController::ApplicationController(std::filesystem::path workingDirectory)
    : workingDirectory_(std::move(workingDirectory)) {
    loadLatestGame();
}

ApplicationController::~ApplicationController() {
    shutdown();
}

void ApplicationController::setStateChanged(StateChanged callback) {
    std::scoped_lock lock(mutex_);
    stateChanged_ = std::move(callback);
}

void ApplicationController::setReportVisibility(ReportGroupVisibility visibility) {
    std::scoped_lock lock(mutex_);
    reportVisibility_ = visibility;
}

ReportGroupVisibility ApplicationController::reportVisibility() const {
    std::scoped_lock lock(mutex_);
    return reportVisibility_;
}

ApplicationSnapshot ApplicationController::snapshot() const {
    std::scoped_lock lock(mutex_);
    auto snapshot = state_;
    snapshot.workerRunning = workerRunning_.load(std::memory_order_acquire);
    return snapshot;
}

bool ApplicationController::prepareStart(ApplicationMode mode,
                                         ProfilerActivity activity,
                                         std::string detail) {
    reapFinished();
    bool started = false;
    {
        std::scoped_lock lock(mutex_);
        if (!worker_.joinable()) {
            state_.mode = mode;
            state_.activity = activity;
            state_.detail = std::move(detail);
            state_.error.clear();
            if (mode == ApplicationMode::Debug)
                state_.diagnostics.clear();
            workerRunning_.store(true, std::memory_order_release);
            started = true;
        }
    }
    if (started)
        notifyChanged();
    return started;
}

bool ApplicationController::startAutomatic(Config config) {
    if (!prepareStart(ApplicationMode::Automatic, ProfilerActivity::WaitingForGame,
                      "Starting automatic detector"))
        return false;
    const auto runCallbacks = callbacks();
    worker_ = std::thread([this, config = std::move(config), runCallbacks]() mutable {
        try {
            (void)runAutomaticProfiler(workingDirectory_, std::move(config), runCallbacks,
                                       [this]() { return reportVisibility(); });
            finishWorker();
        } catch (const std::exception& error) {
            finishWorker(error.what());
        } catch (...) {
            finishWorker("Automatic detector failed");
        }
    });
    return true;
}

bool ApplicationController::startDebug(Config config) {
    if (!prepareStart(ApplicationMode::Debug, ProfilerActivity::WaitingForStarCraft,
                      "Starting live detection test"))
        return false;
    const auto runCallbacks = callbacks();
    worker_ = std::thread([this, config = std::move(config), runCallbacks]() mutable {
        try {
            (void)runDebugProfiler(workingDirectory_, std::move(config), runCallbacks);
            finishWorker();
        } catch (const std::exception& error) {
            finishWorker(error.what());
        } catch (...) {
            finishWorker("Live detection test failed");
        }
    });
    return true;
}

bool ApplicationController::startCalibration(Config config,
                                             const std::filesystem::path& configPath) {
    if (!prepareStart(ApplicationMode::Calibration, ProfilerActivity::Calibrating,
                      "Waiting for StarCraft to calibrate the minimap override"))
        return false;
    calibrationRequested_.store(true, std::memory_order_release);
    worker_ = std::thread([this, config = std::move(config), configPath]() mutable {
        try {
            const int result = runCalibration(
                config, configPath,
                [this](std::string progress) {
                    setStatus(ProfilerActivity::Calibrating, std::move(progress));
                },
                &calibrationRequested_);
            finishWorker(result == 0 ? std::string{}
                                     : "Minimap override calibration cancelled");
        } catch (const std::exception& error) {
            finishWorker(error.what());
        } catch (...) {
            finishWorker("Minimap override calibration failed");
        }
    });
    return true;
}

void ApplicationController::stopCurrent() noexcept {
    ApplicationMode mode;
    {
        std::scoped_lock lock(mutex_);
        mode = state_.mode;
        if (!workerRunning_.load(std::memory_order_acquire))
            return;
        state_.detail = "Stopping cleanly";
    }
    if (mode == ApplicationMode::Automatic)
        requestAutomaticProfilerStop();
    else if (mode == ApplicationMode::Debug)
        requestRecordingProfilerStop();
    else if (mode == ApplicationMode::Calibration)
        calibrationRequested_.store(false, std::memory_order_release);
    notifyChanged();
}

void ApplicationController::reapFinished() {
    if (workerRunning_.load(std::memory_order_acquire) || !worker_.joinable())
        return;
    worker_.join();
}

void ApplicationController::shutdown() noexcept {
    stopCurrent();
    if (worker_.joinable())
        worker_.join();
    workerRunning_.store(false, std::memory_order_release);
}

ProfilerCallbacks ApplicationController::callbacks() {
    ProfilerCallbacks result;
    result.statusChanged = [this](ProfilerActivity activity, std::string detail) {
        setStatus(activity, std::move(detail));
    };
    result.diagnostic = [this](std::string line) { addDiagnostic(std::move(line)); };
    result.gameCompleted = [this](const json::Value& summary,
                                  const std::filesystem::path& path,
                                  const AutomaticSessionStats& session) {
        completeGame(summary, path, session);
    };
    result.sessionUpdated =
        [this](const AutomaticSessionStats& session) { updateSession(session); };
    return result;
}

void ApplicationController::setStatus(ProfilerActivity activity, std::string detail) {
    {
        std::scoped_lock lock(mutex_);
        state_.activity = activity;
        state_.detail = std::move(detail);
    }
    notifyChanged();
}

void ApplicationController::addDiagnostic(std::string line) {
    {
        std::scoped_lock lock(mutex_);
        constexpr std::size_t maximumDiagnosticLines = 200;
        if (state_.diagnostics.size() >= maximumDiagnosticLines)
            state_.diagnostics.erase(state_.diagnostics.begin());
        state_.diagnostics.push_back(std::move(line));
    }
    notifyChanged();
}

void ApplicationController::completeGame(const json::Value& summary,
                                         const std::filesystem::path& path,
                                         const AutomaticSessionStats& session) {
    {
        std::scoped_lock lock(mutex_);
        state_.latestGame = summary;
        state_.latestGamePath = path;
        state_.currentSession = session;
    }
    notifyChanged();
}

void ApplicationController::updateSession(const AutomaticSessionStats& session) {
    {
        std::scoped_lock lock(mutex_);
        state_.currentSession = session;
    }
    notifyChanged();
}

void ApplicationController::finishWorker(std::string error) {
    {
        std::scoped_lock lock(mutex_);
        state_.mode = ApplicationMode::None;
        state_.activity = error.empty() ? ProfilerActivity::Idle : ProfilerActivity::Error;
        state_.detail = error.empty() ? "Profiler is idle" : error;
        state_.error = std::move(error);
        workerRunning_.store(false, std::memory_order_release);
        calibrationRequested_.store(false, std::memory_order_release);
    }
    notifyChanged();
}

void ApplicationController::notifyChanged() const {
    StateChanged callback;
    {
        std::scoped_lock lock(mutex_);
        callback = stateChanged_;
    }
    if (callback)
        callback();
}

void ApplicationController::loadLatestGame() noexcept {
    try {
        const auto sessions = listNavSessions(workingDirectory_ / "sessions");
        if (sessions.empty())
            return;
        const auto navPath = sessions.back();
        auto jsonPath = navPath;
        jsonPath.replace_extension(".json");
        json::Value summary;
        if (std::filesystem::is_regular_file(jsonPath)) {
            summary = json::parseFile(jsonPath);
            state_.latestGamePath = jsonPath;
        } else {
            const auto session = readNavSession(navPath);
            summary = analysisToJson(session.analysis, session.sessionId);
            state_.latestGamePath = navPath;
        }
        state_.latestGame = std::move(summary);
    } catch (...) {
        // A malformed historical result must never prevent GUI startup.
    }
}

} // namespace smp
