#include "test_framework.h"

#include "app/analysis_scouting.h"

#include <optional>
#include <vector>

namespace {

smp::TimelineScoutingActivity activityWithDuration(
    std::optional<double> durationMs) {
    smp::TimelineScoutingActivity activity;
    activity.activityDurationMs = durationMs;
    return activity;
}

} // namespace

TEST_CASE("total scouting time sums available activity durations") {
    const std::vector<smp::TimelineScoutingActivity> activities{
        activityWithDuration(42500.0), activityWithDuration(std::nullopt),
        activityWithDuration(18000.0)};

    const auto total =
        smp::analysis_insights::totalScoutingActivityDurationMs(activities);

    REQUIRE(total.has_value());
    REQUIRE_NEAR(*total, 60500.0, 0.001);
}

TEST_CASE("total scouting time is unavailable without usable durations") {
    const std::vector<smp::TimelineScoutingActivity> activities{
        activityWithDuration(std::nullopt),
        activityWithDuration(std::nullopt)};

    REQUIRE(!smp::analysis_insights::totalScoutingActivityDurationMs(activities)
                 .has_value());
    REQUIRE(!smp::analysis_insights::totalScoutingActivityDurationMs({})
                 .has_value());
}

TEST_CASE("zero-duration scouting activity remains available") {
    const std::vector<smp::TimelineScoutingActivity> activities{
        activityWithDuration(0.0), activityWithDuration(std::nullopt)};

    const auto total =
        smp::analysis_insights::totalScoutingActivityDurationMs(activities);

    REQUIRE(total.has_value());
    REQUIRE_NEAR(*total, 0.0, 0.001);
}
