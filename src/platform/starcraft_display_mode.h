#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace smp {

enum class StarcraftDisplayMode : std::uint8_t {
    OriginalAspect,
    Widescreen,
    Unknown,
};

[[nodiscard]] const char* starcraftDisplayModeName(
    StarcraftDisplayMode mode) noexcept;
[[nodiscard]] std::filesystem::path defaultStarcraftSettingsPath() noexcept;
[[nodiscard]] StarcraftDisplayMode readStarcraftDisplayMode(
    const std::filesystem::path& path) noexcept;

class StarcraftDisplayModeWatcher {
  public:
    explicit StarcraftDisplayModeWatcher(
        std::filesystem::path path,
        std::chrono::milliseconds debounceInterval =
            std::chrono::milliseconds(75));
    ~StarcraftDisplayModeWatcher();
    StarcraftDisplayModeWatcher(const StarcraftDisplayModeWatcher&) = delete;
    StarcraftDisplayModeWatcher& operator=(const StarcraftDisplayModeWatcher&) = delete;

    bool start();
    void stop() noexcept;
    [[nodiscard]] StarcraftDisplayMode mode() const noexcept {
        return mode_.load(std::memory_order_acquire);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }

  private:
    void run() noexcept;
    bool waitForDebounce() const noexcept;
    void updateFromDiskRetainingValidMode() noexcept;

    std::filesystem::path path_;
    std::chrono::milliseconds debounceInterval_{};
    std::atomic<StarcraftDisplayMode> mode_{StarcraftDisplayMode::Unknown};
    void* directoryHandle_{};
    void* stopEvent_{};
    void* changeEvent_{};
    std::thread thread_;
    mutable std::mutex startupMutex_;
    std::condition_variable startupReady_;
    bool startupComplete_{};
    bool startupSucceeded_{};
    std::string error_;
};

} // namespace smp
