#pragma once

#include "config/config.h"
#include "platform/minimap_viewport_detector.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace smp {

struct ReplayMetadata {
    bool exists{};
    std::uint64_t writeTimeUtc{};
    std::uint64_t size{};
};

bool operator==(const ReplayMetadata& first, const ReplayMetadata& second) noexcept;
bool replayMetadataChanged(const ReplayMetadata& baseline, const ReplayMetadata& current) noexcept;
ReplayMetadata readReplayMetadata(const std::filesystem::path& path) noexcept;
std::filesystem::path defaultLastReplayPath();
std::string formatReplayWriteTimeUtc(const ReplayMetadata& metadata);

enum class AutomaticRecordingState {
    Idle,
    Recording,
};

class AutomaticLifecycleState {
  public:
    bool tryStart(const ReplayMetadata& baseline);
    bool tryStop(const ReplayMetadata& current);
    bool forceStop();
    [[nodiscard]] AutomaticRecordingState state() const;
    [[nodiscard]] std::optional<ReplayMetadata> baseline() const;

  private:
    mutable std::mutex mutex_;
    AutomaticRecordingState state_{AutomaticRecordingState::Idle};
    std::optional<ReplayMetadata> baseline_;
};

class LastReplayWatcher {
  public:
    using ChangeCallback = std::function<void(const ReplayMetadata&)>;

    LastReplayWatcher() = default;
    ~LastReplayWatcher();
    LastReplayWatcher(const LastReplayWatcher&) = delete;
    LastReplayWatcher& operator=(const LastReplayWatcher&) = delete;

    bool start(const std::filesystem::path& path, ReplayMetadata baseline, ChangeCallback callback);
    void stop();
    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }

  private:
    void run();

    std::filesystem::path path_;
    ReplayMetadata baseline_;
    ChangeCallback callback_;
    void* directoryHandle_{};
    void* stopEvent_{};
    void* changeEvent_{};
    std::thread thread_;
    std::string error_;
};

class MinimapStartMonitor {
  public:
    using StartCallback = std::function<void()>;

    MinimapStartMonitor(std::wstring executableName, MinimapMode minimapMode,
                        std::optional<NormalizedScreenRect> calibratedMinimap,
                        bool diagnosticsEnabled = true);
    ~MinimapStartMonitor();
    MinimapStartMonitor(const MinimapStartMonitor&) = delete;
    MinimapStartMonitor& operator=(const MinimapStartMonitor&) = delete;

    bool start(StartCallback callback,
               MinimapDetectorState initialState = MinimapDetectorState::WaitForAppearance);
    void stop();

  private:
    void run();

    std::wstring executableName_;
    MinimapMode minimapMode_{MinimapMode::Automatic};
    std::optional<NormalizedScreenRect> calibratedMinimap_;
    bool diagnosticsEnabled_{};
    StartCallback callback_;
    MinimapDetectorState initialState_{MinimapDetectorState::WaitForAppearance};
    void* stopEvent_{};
    std::thread thread_;
};

} // namespace smp
