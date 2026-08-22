#pragma once

#include "app/game_analysis_visualization_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace smp::analysis_insights {

inline constexpr double macroGapWarningSeconds = 10.0;
inline constexpr double macroGapSevereSeconds = 20.0;

enum class MacroGapSeverity {
    Normal,
    Warning,
    Severe,
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

[[nodiscard]] inline MacroGapSeverity
macroGapSeverity(double durationSeconds) noexcept {
    if (durationSeconds > macroGapSevereSeconds)
        return MacroGapSeverity::Severe;
    if (durationSeconds >= macroGapWarningSeconds)
        return MacroGapSeverity::Warning;
    return MacroGapSeverity::Normal;
}

[[nodiscard]] inline std::size_t
macroGapHistogramBucket(double durationSeconds) noexcept {
    if (durationSeconds < 5.0)
        return 0;
    if (durationSeconds < 10.0)
        return 1;
    if (durationSeconds < 15.0)
        return 2;
    if (durationSeconds <= macroGapSevereSeconds)
        return 3;
    return 4;
}

[[nodiscard]] inline double macroGapPercentile(std::vector<double> values,
                                                double probability) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const double position =
        probability * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = std::min(lower + 1, values.size() - 1);
    const double fraction = position - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
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

    summary.gaps.reserve(cycles.size() - 1);
    std::vector<double> durations;
    durations.reserve(cycles.size() - 1);
    for (std::size_t index = 1; index < cycles.size(); ++index) {
        const double startSeconds =
            std::max(0.0, cycles[index - 1].endActiveMs / 1000.0);
        const double nextStartSeconds =
            std::max(0.0, cycles[index].startActiveMs / 1000.0);
        const double durationSeconds =
            std::max(0.0, nextStartSeconds - startSeconds);
        summary.gaps.push_back(
            {startSeconds, startSeconds + durationSeconds, durationSeconds});
        durations.push_back(durationSeconds);
        ++summary.histogram[macroGapHistogramBucket(durationSeconds)];
        if (durationSeconds > macroGapWarningSeconds)
            ++summary.overTenSeconds;
        if (durationSeconds > macroGapSevereSeconds)
            ++summary.overTwentySeconds;
    }

    summary.medianSeconds = macroGapPercentile(durations, 0.50);
    summary.p90Seconds = macroGapPercentile(durations, 0.90);
    summary.longestSeconds =
        *std::max_element(durations.begin(), durations.end());
    return summary;
}

} // namespace smp::analysis_insights
