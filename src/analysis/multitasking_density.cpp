#include "analysis/multitasking_density.h"

#include <algorithm>
#include <cmath>

namespace smp {

std::optional<double>
MultitaskingWindowSummary::averageActiveDiversity() const noexcept {
    if (activeWindowCount == 0)
        return std::nullopt;
    return static_cast<double>(totalDiversityAcrossActiveWindows) /
           static_cast<double>(activeWindowCount);
}

MultitaskingWindowSummary summarizeMultitaskingWindows(
    double activeDurationMs, const MultitaskingActivityTimestamps& activity) {
    MultitaskingWindowSummary result;
    const double durationMs = std::max(0.0, activeDurationMs);
    if (durationMs <= 0.0)
        return result;

    const auto windowCount = static_cast<std::size_t>(
        std::max(1.0, std::ceil(durationMs / multitaskingWindowDurationMs)));
    for (auto& row : result.counts)
        row.assign(windowCount, 0);
    result.diversity.assign(windowCount, 0);

    for (std::size_t row = 0; row < activity.activeMs.size(); ++row) {
        for (const double activeMs : activity.activeMs[row]) {
            if (!std::isfinite(activeMs) || activeMs < 0.0)
                continue;
            const auto index = std::min(
                windowCount - 1,
                static_cast<std::size_t>(activeMs /
                                         multitaskingWindowDurationMs));
            ++result.counts[row][index];
        }
    }

    for (std::size_t index = 0; index < windowCount; ++index) {
        int diversity = 0;
        for (const auto& row : result.counts) {
            if (row[index] > 0)
                ++diversity;
        }
        result.diversity[index] = diversity;
        result.peakDiversity = std::max(result.peakDiversity, diversity);
        if (diversity > 0) {
            ++result.activeWindowCount;
            result.totalDiversityAcrossActiveWindows +=
                static_cast<std::uint64_t>(diversity);
        }
    }
    return result;
}

} // namespace smp
