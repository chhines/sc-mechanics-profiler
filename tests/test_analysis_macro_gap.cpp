#include "test_framework.h"

#include "app/analysis_macro_gap.h"

#include <vector>

namespace {

smp::TimelineMacroCycle cycle(double startSeconds, double endSeconds) {
    smp::TimelineMacroCycle result;
    result.startActiveMs = startSeconds * 1000.0;
    result.endActiveMs = endSeconds * 1000.0;
    return result;
}

} // namespace

TEST_CASE("macro gaps use the previous cycle end and next cycle start") {
    const std::vector<smp::TimelineMacroCycle> cycles{
        cycle(0.0, 4.0), cycle(12.0, 15.0), cycle(40.0, 43.0)};

    const auto summary =
        smp::analysis_insights::macroGapSummary(cycles, 60000.0);

    REQUIRE(summary.gaps.size() == 2);
    REQUIRE_NEAR(summary.gaps[0].startSeconds, 4.0, 0.001);
    REQUIRE_NEAR(summary.gaps[0].endSeconds, 12.0, 0.001);
    REQUIRE_NEAR(summary.gaps[0].durationSeconds, 8.0, 0.001);
    REQUIRE_NEAR(summary.gaps[1].startSeconds, 15.0, 0.001);
    REQUIRE_NEAR(summary.gaps[1].endSeconds, 40.0, 0.001);
    REQUIRE_NEAR(summary.gaps[1].durationSeconds, 25.0, 0.001);
}

TEST_CASE("macro gap summary does not fabricate first or final gaps") {
    const auto oneCycle = smp::analysis_insights::macroGapSummary(
        {cycle(10.0, 12.0)}, 60000.0);
    REQUIRE(oneCycle.gaps.empty());
    REQUIRE(!oneCycle.medianSeconds.has_value());
    REQUIRE(!oneCycle.p90Seconds.has_value());
    REQUIRE(!oneCycle.longestSeconds.has_value());
    REQUIRE(oneCycle.cyclesPerMinute.has_value());
    REQUIRE_NEAR(*oneCycle.cyclesPerMinute, 1.0, 0.001);

    const auto noCycles =
        smp::analysis_insights::macroGapSummary({}, 60000.0);
    REQUIRE(noCycles.gaps.empty());
    REQUIRE(!noCycles.cyclesPerMinute.has_value());
}

TEST_CASE("macro gap KPIs and fixed histogram use end-to-start lengths") {
    const std::vector<smp::TimelineMacroCycle> cycles{
        cycle(0.0, 1.0),   cycle(3.0, 4.0),
        cycle(11.0, 12.0), cycle(24.0, 25.0),
        cycle(42.0, 43.0), cycle(64.0, 65.0)};

    const auto summary =
        smp::analysis_insights::macroGapSummary(cycles, 120000.0);

    REQUIRE(summary.cyclesPerMinute.has_value());
    REQUIRE_NEAR(*summary.cyclesPerMinute, 3.0, 0.001);
    REQUIRE_NEAR(*summary.medianSeconds, 12.0, 0.001);
    REQUIRE_NEAR(*summary.p90Seconds, 19.4, 0.001);
    REQUIRE_NEAR(*summary.longestSeconds, 21.0, 0.001);
    REQUIRE(summary.overTenSeconds == 3);
    REQUIRE(summary.overTwentySeconds == 1);
    REQUIRE(summary.histogram[0] == 1);
    REQUIRE(summary.histogram[1] == 1);
    REQUIRE(summary.histogram[2] == 1);
    REQUIRE(summary.histogram[3] == 1);
    REQUIRE(summary.histogram[4] == 1);
}

TEST_CASE("macro gap duration bands preserve the ten and twenty second boundaries") {
    using smp::analysis_insights::MacroGapBand;
    using smp::analysis_insights::macroGapBand;
    using smp::analysis_insights::macroGapHistogramBucket;

    REQUIRE(macroGapBand(9.999) == MacroGapBand::UnderTenSeconds);
    REQUIRE(macroGapBand(10.0) == MacroGapBand::TenToTwentySeconds);
    REQUIRE(macroGapBand(20.0) == MacroGapBand::TenToTwentySeconds);
    REQUIRE(macroGapBand(20.001) == MacroGapBand::OverTwentySeconds);
    REQUIRE(macroGapHistogramBucket(10.0) == 2);
    REQUIRE(macroGapHistogramBucket(20.0) == 3);
    REQUIRE(macroGapHistogramBucket(20.001) == 4);
}
