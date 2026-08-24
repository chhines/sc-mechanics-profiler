#pragma once

#include "app/gui_preferences.h"

#include <cstddef>
#include <vector>

namespace smp {

enum class AnalysisTimelineTrack {
    WorkerMacro,
    ArmyMacro,
    ControlGroupEdits,
    Scouting,
};

struct AnalysisTimelineTrackVisibility {
    bool workerMacro{};
    bool armyMacro{};
    bool controlGroupEdits{};
    bool scouting{};
};

struct AnalysisTimelineTrackRow {
    AnalysisTimelineTrack track;
    double y{};
};

[[nodiscard]] inline AnalysisTimelineTrackVisibility
configuredAnalysisTimelineTracks(
    const ReportGroupVisibility& visibility,
    const AnalysisTimelineTrackVisibility& selected) noexcept {
    return {
        visibility.workerMacroCycles && selected.workerMacro,
        visibility.armyMacroCycles && selected.armyMacro,
        visibility.armyControlGroupManagement && selected.controlGroupEdits,
        visibility.scoutingUnitActivity && selected.scouting,
    };
}

[[nodiscard]] inline std::vector<AnalysisTimelineTrackRow>
buildAnalysisTimelineTrackLayout(
    const AnalysisTimelineTrackVisibility& enabled,
    const AnalysisTimelineTrackVisibility& available) {
    std::vector<AnalysisTimelineTrackRow> rows;
    const auto addIfVisible = [&](AnalysisTimelineTrack track, bool isEnabled,
                                  bool isAvailable) {
        if (isEnabled && isAvailable)
            rows.push_back({track, 0.0});
    };

    addIfVisible(AnalysisTimelineTrack::WorkerMacro, enabled.workerMacro,
                 available.workerMacro);
    addIfVisible(AnalysisTimelineTrack::ArmyMacro, enabled.armyMacro,
                 available.armyMacro);
    addIfVisible(AnalysisTimelineTrack::ControlGroupEdits,
                 enabled.controlGroupEdits, available.controlGroupEdits);
    addIfVisible(AnalysisTimelineTrack::Scouting, enabled.scouting,
                 available.scouting);

    for (std::size_t index = 0; index < rows.size(); ++index)
        rows[index].y = static_cast<double>(rows.size() - index - 1);
    return rows;
}

} // namespace smp
