#pragma once

#include "analysis/multitasking_density.h"
#include "app/game_analysis_visualization_model.h"

#include <cstddef>

namespace smp::analysis_insights {

[[nodiscard]] inline MultitaskingWindowSummary multitaskingWindows(
    const GameAnalysisVisualizationModel& model) {
    MultitaskingActivityTimestamps activity;
    for (const auto& event : model.navigationEvents)
        activity.activeMs[static_cast<std::size_t>(
            MultitaskingMechanicClass::Camera)].push_back(event.activeMs);
    for (const auto& cycle : model.workerMacroCycles)
        activity.activeMs[static_cast<std::size_t>(
            MultitaskingMechanicClass::WorkerMacro)]
            .push_back(cycle.startActiveMs);
    for (const auto& cycle : model.armyMacroCycles)
        activity.activeMs[static_cast<std::size_t>(
            MultitaskingMechanicClass::ArmyMacro)]
            .push_back(cycle.startActiveMs);
    for (const auto& edit : model.armyControlGroupEdits)
        activity.activeMs[static_cast<std::size_t>(
            MultitaskingMechanicClass::ControlGroupEdit)]
            .push_back(edit.operationActiveMs);
    for (const auto& scout : model.scoutingActivities) {
        for (const double command : scout.commandActiveMs)
            activity.activeMs[static_cast<std::size_t>(
                MultitaskingMechanicClass::ScoutCommand)]
                .push_back(command);
    }
    return summarizeMultitaskingWindows(model.activeDurationMs, activity);
}

} // namespace smp::analysis_insights
