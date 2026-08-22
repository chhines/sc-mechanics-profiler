#pragma once

#include "app/game_analysis_visualization_model.h"

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

} // namespace smp::analysis_insights
