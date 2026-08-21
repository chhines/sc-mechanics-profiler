#include "test_framework.h"

#include "app/analysis_timeline_layout.h"

namespace {

using smp::AnalysisTimelineTrack;
using smp::AnalysisTimelineTrackVisibility;
using smp::buildAnalysisTimelineTrackLayout;

constexpr AnalysisTimelineTrackVisibility allTracks{true, true, true, true};

void requireRow(const smp::AnalysisTimelineTrackRow& row,
                AnalysisTimelineTrack expectedTrack, double expectedY) {
    REQUIRE(row.track == expectedTrack);
    REQUIRE(row.y == expectedY);
}

} // namespace

TEST_CASE("analysis timeline lays out all tracks contiguously in display order") {
    const auto rows = buildAnalysisTimelineTrackLayout(allTracks, allTracks);

    REQUIRE(rows.size() == 4);
    requireRow(rows[0], AnalysisTimelineTrack::WorkerMacro, 3.0);
    requireRow(rows[1], AnalysisTimelineTrack::ArmyMacro, 2.0);
    requireRow(rows[2], AnalysisTimelineTrack::ControlGroupEdits, 1.0);
    requireRow(rows[3], AnalysisTimelineTrack::Scouting, 0.0);
}

TEST_CASE("analysis timeline packs remaining tracks when worker is hidden") {
    const AnalysisTimelineTrackVisibility enabled{false, true, true, true};
    const auto rows = buildAnalysisTimelineTrackLayout(enabled, allTracks);

    REQUIRE(rows.size() == 3);
    requireRow(rows[0], AnalysisTimelineTrack::ArmyMacro, 2.0);
    requireRow(rows[1], AnalysisTimelineTrack::ControlGroupEdits, 1.0);
    requireRow(rows[2], AnalysisTimelineTrack::Scouting, 0.0);
}

TEST_CASE("analysis timeline leaves no gap when a middle track is hidden") {
    const AnalysisTimelineTrackVisibility enabled{true, false, true, true};
    const auto rows = buildAnalysisTimelineTrackLayout(enabled, allTracks);

    REQUIRE(rows.size() == 3);
    requireRow(rows[0], AnalysisTimelineTrack::WorkerMacro, 2.0);
    requireRow(rows[1], AnalysisTimelineTrack::ControlGroupEdits, 1.0);
    requireRow(rows[2], AnalysisTimelineTrack::Scouting, 0.0);
}

TEST_CASE("analysis timeline assigns zero to its only visible row") {
    const AnalysisTimelineTrackVisibility enabled{false, false, true, false};
    const auto rows = buildAnalysisTimelineTrackLayout(enabled, allTracks);

    REQUIRE(rows.size() == 1);
    requireRow(rows[0], AnalysisTimelineTrack::ControlGroupEdits, 0.0);
}

TEST_CASE("analysis timeline layout is empty when no tracks are visible") {
    const AnalysisTimelineTrackVisibility none{};

    REQUIRE(buildAnalysisTimelineTrackLayout(none, allTracks).empty());
    REQUIRE(buildAnalysisTimelineTrackLayout(allTracks, none).empty());
}
