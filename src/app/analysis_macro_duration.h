#pragma once

#include "app/game_analysis_visualization_model.h"

#include <array>
#include <cstddef>
#include <vector>

namespace smp::analysis_insights {

inline constexpr std::array<const char*, 5> macroDurationBucketLabels{
    "0-1 s", "1-2 s", "2-3 s", "3-4 s", "4+ s"};

[[nodiscard]] inline std::array<std::size_t, 5> macroDurationBins(
    const std::vector<TimelineMacroCycle>& cycles) noexcept {
    std::array<std::size_t, 5> bins{};
    for (const auto& cycle : cycles) {
        const double seconds = cycle.durationMs / 1000.0;
        std::size_t index = 4;
        if (seconds < 1.0)
            index = 0;
        else if (seconds < 2.0)
            index = 1;
        else if (seconds < 3.0)
            index = 2;
        else if (seconds < 4.0)
            index = 3;
        ++bins[index];
    }
    return bins;
}

} // namespace smp::analysis_insights
