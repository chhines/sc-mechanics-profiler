#pragma once

#include "analysis/macro_gap.h"
#include "app/game_analysis_visualization_model.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace smp::analysis_insights {

inline constexpr double macroGapTenSecondBoundary = 10.0;
inline constexpr double macroGapTwentySecondBoundary = 20.0;

enum class MacroGapBand {
    UnderTenSeconds,
    TenToTwentySeconds,
    OverTwentySeconds,
};

struct MacroGapInterval {
    double startSeconds{};
    double endSeconds{};
    double durationSeconds{};
};

struct MacroGapSummary {
    std::optional<double> cyclesPerMinute;
    std::vector<MacroGapInterval> gaps;
    std::optional<double> medianSeconds;
    std::optional<double> p90Seconds;
    std::optional<double> longestSeconds;
    std::size_t overTenSeconds{};
    std::size_t overTwentySeconds{};
    std::array<std::size_t, 5> histogram{};
};

[[nodiscard]] inline MacroGapBand macroGapBand(double durationSeconds) noexcept {
    if (durationSeconds > macroGapTwentySecondBoundary)
        return MacroGapBand::OverTwentySeconds;
    if (durationSeconds >= macroGapTenSecondBoundary)
        return MacroGapBand::TenToTwentySeconds;
    return MacroGapBand::UnderTenSeconds;
}

[[nodiscard]] inline std::size_t
macroGapHistogramBucket(double durationSeconds) noexcept {
    if (durationSeconds < 5.0)
        return 0;
    if (durationSeconds < 10.0)
        return 1;
    if (durationSeconds < 15.0)
        return 2;
    if (durationSeconds <= macroGapTwentySecondBoundary)
        return 3;
    return 4;
}

[[nodiscard]] inline MacroGapSummary macroGapSummary(
    const std::vector<TimelineMacroCycle>& cycles, double activeDurationMs) {
    MacroGapSummary summary;
    if (!cycles.empty() && activeDurationMs > 0.0) {
        summary.cyclesPerMinute =
            static_cast<double>(cycles.size()) * 60000.0 / activeDurationMs;
    }
    if (cycles.size() < 2)
        return summary;

    const auto observations = macroGapObservations(cycles);
    summary.gaps.reserve(observations.size());
    std::vector<double> durations;
    durations.reserve(observations.size());
    for (const auto& observation : observations) {
        const double startSeconds = observation.previousCycleEndMs / 1000.0;
        const double durationSeconds = observation.durationMs / 1000.0;
        summary.gaps.push_back(
            {startSeconds, startSeconds + durationSeconds, durationSeconds});
        durations.push_back(durationSeconds);
        ++summary.histogram[macroGapHistogramBucket(durationSeconds)];
        if (durationSeconds > macroGapTenSecondBoundary)
            ++summary.overTenSeconds;
        if (durationSeconds > macroGapTwentySecondBoundary)
            ++summary.overTwentySeconds;
    }

    summary.medianSeconds = interpolatedPercentile(durations, 0.50);
    summary.p90Seconds = interpolatedPercentile(durations, 0.90);
    summary.longestSeconds =
        *std::max_element(durations.begin(), durations.end());
    return summary;
}

} // namespace smp::analysis_insights
