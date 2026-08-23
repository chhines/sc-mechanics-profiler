#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace smp {

inline constexpr double multitaskingWindowDurationMs = 5000.0;
inline constexpr std::size_t multitaskingMechanicClassCount = 5;

enum class MultitaskingMechanicClass : std::size_t {
    Camera,
    WorkerMacro,
    ArmyMacro,
    ControlGroupEdit,
    ScoutCommand,
};

struct MultitaskingActivityTimestamps {
    std::array<std::vector<double>, multitaskingMechanicClassCount> activeMs;
};

struct MultitaskingWindowSummary {
    std::array<std::vector<int>, multitaskingMechanicClassCount> counts;
    std::vector<int> diversity;
    std::uint64_t totalDiversityAcrossActiveWindows{};
    std::uint64_t activeWindowCount{};
    int peakDiversity{};

    [[nodiscard]] std::optional<double> averageActiveDiversity() const noexcept;
};

[[nodiscard]] MultitaskingWindowSummary summarizeMultitaskingWindows(
    double activeDurationMs, const MultitaskingActivityTimestamps& activity);

} // namespace smp
