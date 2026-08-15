#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>

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

struct StarcraftDisplayModeRefresh {
    StarcraftDisplayMode mode{StarcraftDisplayMode::Unknown};
    bool changed{};
    bool checked{};
};

class StarcraftDisplayModeReader {
  public:
    explicit StarcraftDisplayModeReader(
        std::filesystem::path path,
        std::chrono::milliseconds checkInterval = std::chrono::milliseconds(500));

    [[nodiscard]] StarcraftDisplayModeRefresh refreshNow() noexcept;
    [[nodiscard]] StarcraftDisplayModeRefresh refreshIfDue(
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) noexcept;
    [[nodiscard]] StarcraftDisplayMode mode() const noexcept {
        return mode_;
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    struct FileState {
        bool exists{};
        std::filesystem::file_time_type writeTime{};
        std::uintmax_t size{};

        bool operator==(const FileState&) const noexcept = default;
    };

    [[nodiscard]] StarcraftDisplayModeRefresh refreshFromDisk() noexcept;
    [[nodiscard]] static FileState readFileState(
        const std::filesystem::path& path) noexcept;

    std::filesystem::path path_;
    std::chrono::milliseconds checkInterval_{};
    std::chrono::steady_clock::time_point nextCheck_{};
    bool hasCheckDeadline_{};
    std::optional<FileState> fileState_;
    StarcraftDisplayMode mode_{StarcraftDisplayMode::Unknown};
};

} // namespace smp
