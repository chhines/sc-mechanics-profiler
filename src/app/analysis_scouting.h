#pragma once

#include "app/game_analysis_visualization_model.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace smp::analysis_insights {

[[nodiscard]] inline std::optional<double> totalScoutingActivityDurationMs(
    const std::vector<TimelineScoutingActivity>& activities) {
    std::optional<double> totalMs;
    for (const auto& activity : activities) {
        if (activity.activityDurationMs)
            totalMs = totalMs.value_or(0.0) + *activity.activityDurationMs;
    }
    return totalMs;
}

struct ScoutingOutcomeCounts {
    std::size_t returnedHome{};
    std::size_t noObservedReturn{};
    std::size_t resumedAfterTemporaryReturn{};
};

[[nodiscard]] inline std::optional<ScoutingOutcomeCounts>
scoutingOutcomeCounts(
    bool outcomeDataAvailable,
    const std::vector<TimelineScoutingActivity>& activities) noexcept {
    if (!outcomeDataAvailable)
        return std::nullopt;

    ScoutingOutcomeCounts counts;
    for (const auto& activity : activities) {
        if (!activity.outcomeAvailable)
            continue;
        if (activity.returnedHome)
            ++counts.returnedHome;
        else
            ++counts.noObservedReturn;
        if (activity.resumedAfterTemporaryReturn)
            ++counts.resumedAfterTemporaryReturn;
    }
    return counts;
}

} // namespace smp::analysis_insights
