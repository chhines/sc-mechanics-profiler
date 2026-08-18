#pragma once

#include "analysis/analyzer.h"
#include "storage/session.h"
#include "util/json.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace smp {

struct VisualizationTrackStatus {
    bool available{};
    std::string reason;
};

struct TimelineNavigationEvent {
    double activeMs{};
    CameraNavigationType type{CameraNavigationType::ControlGroupJump};
    int id{-1};
    int cursorX{};
    int cursorY{};
    double durationMs{};
    EdgeDirection edgeDirection{EdgeDirection::None};
    int startCursorX{};
    int startCursorY{};
};

struct TimelineMacroCycle {
    std::string productType;
    double startActiveMs{};
    double executionEndActiveMs{};
    double endActiveMs{};
    double durationMs{};
    double fullSpanMs{};
    std::size_t visitCount{};
    std::string accessStyle;
};

struct TimelineProductionVisit {
    std::size_t sourceIndex{};
    std::string productType;
    double startActiveMs{};
    double contextActiveMs{};
    double firstProductionActiveMs{};
    double endActiveMs{};
    std::string selectionAccess;
    std::string cameraAccess;
    std::optional<int> controlGroup;
    std::optional<int> locationHotkey;
    std::string productionContext;
    std::vector<std::string> producedUnits;
    int physicalProductionPresses{};
    bool replayConfirmed{};
    double accessLatencyMs{};
    double productionLatencyMs{};
    double executionDurationMs{};
};

struct TimelineControlGroupEdit {
    double operationActiveMs{};
    int group{-1};
    std::string operation;
    std::string selectionMethod;
    std::optional<double> selectionToOperationMs;
    std::optional<double> selectionDurationMs;
    std::optional<double> totalExecutionMs;
    std::string scope;
    bool replayConfirmed{};
    std::string bindingConfidence;
};

struct TimelineScoutingActivity {
    int group{-1};
    std::uint32_t assignmentGeneration{};
    std::uint32_t unitTag{};
    double assignedActiveMs{};
    std::optional<double> lastCommandActiveMs;
    std::optional<double> activityDurationMs;
    std::vector<double> commandActiveMs;
    std::optional<double> longestCommandGapMs;
    std::size_t selectionCount{};
    std::size_t commandCount{};
    bool outcomeAvailable{};
    bool returnedHome{};
    bool resumedAfterTemporaryReturn{};
};

struct MacroAccessStyleDurationGroup {
    std::string accessStyle;
    std::vector<double> durationMs;
    std::optional<double> medianMs;
    std::optional<double> p25Ms;
    std::optional<double> p75Ms;
    std::optional<double> p90Ms;
};

struct GameAnalysisVisualizationModel {
    std::string sessionId;
    std::filesystem::path navPath;
    std::filesystem::path jsonPath;
    bool navLoaded{};
    bool jsonLoaded{};
    double activeDurationMs{};

    VisualizationTrackStatus navigationStatus;
    VisualizationTrackStatus workerMacroStatus;
    VisualizationTrackStatus armyMacroStatus;
    VisualizationTrackStatus productionVisitStatus;
    VisualizationTrackStatus controlGroupEditStatus;
    VisualizationTrackStatus scoutingStatus;

    std::vector<TimelineNavigationEvent> navigationEvents;
    std::vector<TimelineMacroCycle> workerMacroCycles;
    std::vector<TimelineMacroCycle> armyMacroCycles;
    std::vector<TimelineProductionVisit> productionVisits;
    std::vector<TimelineControlGroupEdit> armyControlGroupEdits;
    std::vector<TimelineScoutingActivity> scoutingActivities;
    bool scoutingOutcomeDataAvailable{};
    std::size_t scoutingCandidateCount{};
    std::size_t unconfirmedScoutingCandidateCount{};
    std::vector<MacroAccessStyleDurationGroup> workerAccessStyleDurations;
    std::vector<MacroAccessStyleDurationGroup> armyAccessStyleDurations;
};

[[nodiscard]] GameAnalysisVisualizationModel buildGameAnalysisVisualizationModel(const NavSession* nav,
                                                                                 const json::Value* derivedJson);

[[nodiscard]] GameAnalysisVisualizationModel loadGameAnalysisVisualizationModel(
    const std::filesystem::path& selectedResultPath);

[[nodiscard]] bool shouldReloadAnalysisModel(
    const GameAnalysisVisualizationModel& current,
    const GameAnalysisVisualizationModel& requested) noexcept;

} // namespace smp
