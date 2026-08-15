#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace smp {

struct ScreenPoint {
    int x{};
    int y{};
};

// Inclusive desktop/screen coordinates. Win32 RECT values must be converted
// explicitly because their right and bottom members are exclusive.
struct ScreenRect {
    int left{};
    int top{};
    int right{-1};
    int bottom{-1};

    [[nodiscard]] bool valid() const noexcept {
        return right >= left && bottom >= top;
    }
    [[nodiscard]] int width() const noexcept {
        return valid() ? right - left + 1 : 0;
    }
    [[nodiscard]] int height() const noexcept {
        return valid() ? bottom - top + 1 : 0;
    }
    [[nodiscard]] bool contains(ScreenPoint point) const noexcept {
        return valid() && point.x >= left && point.x <= right && point.y >= top && point.y <= bottom;
    }
    bool operator==(const ScreenRect&) const noexcept = default;
};

struct NormalizedScreenRect {
    double left{};
    double top{};
    double right{};
    double bottom{};

    [[nodiscard]] bool valid() const noexcept {
        return left >= 0.0 && top >= 0.0 && right <= 1.0 && bottom <= 1.0 && right > left && bottom > top;
    }
    bool operator==(const NormalizedScreenRect&) const noexcept = default;
};

enum class MinimapMode {
    Automatic,
    CalibratedOverride,
};

[[nodiscard]] const char* minimapModeName(MinimapMode mode) noexcept;

struct Config {
    std::wstring starcraftProcess{L"StarCraft.exe"};
    int controlGroupDoubleTapMs{300};
    std::vector<std::uint16_t> locationHotkeys{0x71, 0x72, 0x73}; // F2-F4

    ScreenRect gameArea{};
    ScreenRect viewport{};
    ScreenRect minimap{};
    ScreenRect commandCard{};
    bool autoScreenRegions{true};
    MinimapMode originalAspectMinimapMode{MinimapMode::Automatic};
    MinimapMode widescreenMinimapMode{MinimapMode::Automatic};
    std::optional<NormalizedScreenRect> calibratedMinimap;
    std::optional<NormalizedScreenRect> widescreenCalibratedMinimap;
    std::uint16_t calibrationCaptureKey{0x78}; // F9

    int edgeMarginPx{5};
    int edgeMinimumDwellMs{20};
    int flushIntervalMs{1000};

    static Config loadOrCreate(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
    void useOriginalAspectAutomaticMinimap() noexcept;
    void useWidescreenAutomaticMinimap() noexcept;
    void useCalibratedMinimapOverride(NormalizedScreenRect calibration);
    void useWidescreenCalibratedMinimapOverride(NormalizedScreenRect calibration);
};

std::uint16_t keyNameToVirtualKey(const std::string& value);
std::string virtualKeyToName(std::uint16_t key);

} // namespace smp
