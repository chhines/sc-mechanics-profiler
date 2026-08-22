#pragma once

#include "app/game_analysis_visualization_model.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace smp::analysis_insights {

inline constexpr double navigationBucketMs = 30000.0;

struct NavigationRateBucket {
    double startSeconds{};
    double endSeconds{};
    double ratePerMinute{};
};

struct NavigationBucketSeries {
    std::vector<NavigationRateBucket> buckets;
};

[[nodiscard]] inline NavigationBucketSeries navigationRates(
    const GameAnalysisVisualizationModel& model) {
    NavigationBucketSeries series;
    const double durationMs = std::max(0.0, model.activeDurationMs);
    if (durationMs <= 0.0)
        return series;
    const auto bucketCount = static_cast<std::size_t>(
        std::max(1.0, std::ceil(durationMs / navigationBucketMs)));
    std::vector<int> counts(bucketCount, 0);
    for (const auto& event : model.navigationEvents) {
        const auto index = std::min(
            bucketCount - 1,
            static_cast<std::size_t>(std::max(0.0, event.activeMs) /
                                     navigationBucketMs));
        ++counts[index];
    }

    series.buckets.reserve(bucketCount);
    for (std::size_t index = 0; index < bucketCount; ++index) {
        const double startMs = static_cast<double>(index) * navigationBucketMs;
        const double endMs = std::min(durationMs, startMs + navigationBucketMs);
        // Every bucket is presented as a 30-second equivalent rate. The final
        // partial bucket uses the same denominator so a few closing seconds do
        // not create an artificial spike.
        series.buckets.push_back({startMs / 1000.0, endMs / 1000.0,
                                  static_cast<double>(counts[index]) * 2.0});
    }
    return series;
}

[[nodiscard]] inline const NavigationRateBucket* navigationBucketAt(
    const NavigationBucketSeries& series, double activeSeconds) noexcept {
    for (std::size_t index = 0; index < series.buckets.size(); ++index) {
        const auto& bucket = series.buckets[index];
        const bool isFinal = index + 1 == series.buckets.size();
        if (activeSeconds >= bucket.startSeconds &&
            (activeSeconds < bucket.endSeconds ||
             (isFinal && activeSeconds <= bucket.endSeconds))) {
            return &bucket;
        }
    }
    return nullptr;
}

} // namespace smp::analysis_insights
