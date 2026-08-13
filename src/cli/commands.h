#pragma once

#include "app/gui_preferences.h"
#include "cli/automatic_session_stats.h"
#include "config/config.h"
#include "util/json.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace smp {

enum class ProfilerActivity {
    Idle,
    WaitingForGame,
    WaitingForStarCraft,
    Recording,
    Paused,
    AnalyzingReplay,
    Calibrating,
    Error,
};

struct ProfilerCallbacks {
    std::function<void(ProfilerActivity, std::string)> statusChanged;
    std::function<void(std::string)> diagnostic;
    std::function<void(const json::Value&, const std::filesystem::path&,
                       const AutomaticSessionStats&)>
        gameCompleted;
    std::function<void(const AutomaticSessionStats&)> sessionUpdated;
};

int runCommand(const std::vector<std::string>& arguments, const std::filesystem::path& workingDirectory);
void printUsage();
int runAutomaticProfiler(const std::filesystem::path& workingDirectory, Config config,
                          const ProfilerCallbacks& callbacks,
                          const ReportGroupVisibility& reportVisibility);
int runDebugProfiler(const std::filesystem::path& workingDirectory, Config config,
                     const ProfilerCallbacks& callbacks);
void requestAutomaticProfilerStop() noexcept;
void requestRecordingProfilerStop() noexcept;

} // namespace smp
