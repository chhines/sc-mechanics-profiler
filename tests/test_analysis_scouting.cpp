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

smp::TimelineScoutingActivity scoutingOutcome(
    bool returnedHome, bool resumedAfterTemporaryReturn) {
    smp::TimelineScoutingActivity activity;
    activity.outcomeAvailable = true;
    activity.returnedHome = returnedHome;
    activity.resumedAfterTemporaryReturn = resumedAfterTemporaryReturn;
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

TEST_CASE("scouting outcome counts are available only with current telemetry") {
    const std::vector<smp::TimelineScoutingActivity> activities{
        scoutingOutcome(true, false), scoutingOutcome(false, false),
        scoutingOutcome(true, true)};

    const auto current =
        smp::analysis_insights::scoutingOutcomeCounts(true, activities);
    REQUIRE(current.has_value());
    REQUIRE(current->returnedHome == 2);
    REQUIRE(current->noObservedReturn == 1);
    REQUIRE(current->resumedAfterTemporaryReturn == 1);

    REQUIRE(!smp::analysis_insights::scoutingOutcomeCounts(false, activities)
                 .has_value());
}
