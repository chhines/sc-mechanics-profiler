#include "test_framework.h"

#include "app/analysis_navigation_rate.h"

namespace {

smp::TimelineNavigationEvent navigationEvent(double activeMs) {
    smp::TimelineNavigationEvent event;
    event.activeMs = activeMs;
    return event;
}

} // namespace

TEST_CASE("navigation rates retain full thirty-second bucket boundaries") {
    smp::GameAnalysisVisualizationModel model;
    model.activeDurationMs = 60000.0;
    model.navigationEvents = {
        navigationEvent(1000.0), navigationEvent(29999.0),
        navigationEvent(30000.0)};

    const auto series = smp::analysis_insights::navigationRates(model);

    REQUIRE(series.buckets.size() == 2);
    REQUIRE_NEAR(series.buckets[0].startSeconds, 0.0, 0.001);
    REQUIRE_NEAR(series.buckets[0].endSeconds, 30.0, 0.001);
    REQUIRE_NEAR(series.buckets[0].ratePerMinute, 4.0, 0.001);
    REQUIRE_NEAR(series.buckets[1].startSeconds, 30.0, 0.001);
    REQUIRE_NEAR(series.buckets[1].endSeconds, 60.0, 0.001);
    REQUIRE_NEAR(series.buckets[1].ratePerMinute, 2.0, 0.001);
}

TEST_CASE("navigation rates end the final partial bucket at game end") {
    smp::GameAnalysisVisualizationModel model;
    model.activeDurationMs = 65000.0;
    model.navigationEvents = {
        navigationEvent(1000.0), navigationEvent(31000.0),
        navigationEvent(62000.0), navigationEvent(64000.0)};

    const auto series = smp::analysis_insights::navigationRates(model);

    REQUIRE(series.buckets.size() == 3);
    REQUIRE_NEAR(series.buckets[2].startSeconds, 60.0, 0.001);
    REQUIRE_NEAR(series.buckets[2].endSeconds, 65.0, 0.001);
    REQUIRE_NEAR(series.buckets[2].ratePerMinute, 4.0, 0.001);
}

TEST_CASE("navigation bucket lookup uses containment and exact boundaries") {
    smp::GameAnalysisVisualizationModel model;
    model.activeDurationMs = 65000.0;
    const auto series = smp::analysis_insights::navigationRates(model);

    const auto* first =
        smp::analysis_insights::navigationBucketAt(series, 29.999);
    const auto* second =
        smp::analysis_insights::navigationBucketAt(series, 30.0);
    const auto* final =
        smp::analysis_insights::navigationBucketAt(series, 65.0);

    REQUIRE(first == &series.buckets[0]);
    REQUIRE(second == &series.buckets[1]);
    REQUIRE(final == &series.buckets[2]);
    REQUIRE(smp::analysis_insights::navigationBucketAt(series, 65.001) ==
            nullptr);
}
