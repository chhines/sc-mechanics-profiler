#pragma once

#include "analysis/analyzer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace smp {

inline constexpr double armySelectionAttributionWindowMs = 2000.0;
inline constexpr double armyDoubleClickThresholdMs = 500.0;
inline constexpr int armySelectionDragThresholdPixels = 4;
inline constexpr int armyDoubleClickDistancePixels = 4;
inline constexpr double scoutingUnitCutoffMs = 120000.0;
inline constexpr double scoutingUnitTravelProgressThreshold = 0.5;
inline constexpr double scoutingHomeRadiusSpawnFraction = 0.15;
inline constexpr double scoutingHomeRadiusMinPixels = 320.0;
inline constexpr double scoutingHomeRadiusMaxPixels = 640.0;
inline constexpr double scoutingPhysicalCommandMatchWindowMs = 500.0;

struct ArmyControlGroupDetectionConfig {
    double attributionWindowMs{armySelectionAttributionWindowMs};
    double doubleClickThresholdMs{armyDoubleClickThresholdMs};
    int dragThresholdPixels{armySelectionDragThresholdPixels};
    int doubleClickDistancePixels{armyDoubleClickDistancePixels};
};

enum class ArmyControlGroupOperation : std::uint8_t {
    Assign,
    Add
};

enum class ArmySelectionMethod : std::uint8_t {
    DirectClick,
    BoxSelect,
    CtrlClickType,
    DoubleClickType,
    ShiftClickModify,
    ShiftBoxModify,
    CtrlShiftClickType,
    ExistingSelection,
    Other
};

inline constexpr std::size_t armySelectionMethodCount = 9;

enum class ArmyControlGroupBindingConfidence : std::uint8_t {
    PhysicalOnly,
    ReplayConfirmed,
    Ambiguous
};

enum class ArmyControlGroupScope : std::uint8_t {
    Army,
    ProductionBuilding,
    // Early singleton worker confirmed by replay-attributed commands that move
    // that same unit tag onto the opponent's side of the map.
    ScoutingUnit,
    Uncertain
};

struct ArmyControlGroupEdit {
    std::uint64_t operationQpc{};
    double operationActiveMs{};
    int group{-1};
    ArmyControlGroupOperation operation{ArmyControlGroupOperation::Assign};
    ArmySelectionMethod selectionMethod{ArmySelectionMethod::Other};
    std::uint64_t selectionStartQpc{};
    std::uint64_t selectionCompleteQpc{};
    std::optional<double> selectionDurationMs;
    std::optional<double> selectionToOperationMs;
    std::optional<double> totalExecutionMs;
    int selectedUnitCount{};
    std::vector<std::string> selectedUnitTypes;
    std::vector<std::uint32_t> selectedUnitTags;
    bool replayConfirmed{};
    ArmyControlGroupBindingConfidence bindingConfidence{
        ArmyControlGroupBindingConfidence::PhysicalOnly};
    ArmyControlGroupScope scope{ArmyControlGroupScope::Uncertain};
};

struct ArmyControlGroupMethodStatistics {
    std::size_t editCount{};
    std::optional<double> averageSelectionToOperationMs;
    std::optional<double> medianSelectionToOperationMs;
    std::optional<double> bestSelectionToOperationMs;
    std::optional<double> p25SelectionToOperationMs;
    std::optional<double> p75SelectionToOperationMs;
    std::optional<double> p90SelectionToOperationMs;
    std::optional<double> averageSelectionDurationMs;
    std::optional<double> averageTotalExecutionMs;
};

struct ArmyControlGroupPerGroupStatistics {
    std::size_t assignments{};
    std::size_t additions{};
};

struct ScoutingUnitActivity {
    int group{-1};
    std::uint32_t assignmentGeneration{};
    std::uint64_t assignedQpc{};
    double assignedActiveMs{};
    std::optional<std::uint64_t> firstSelectionQpc;
    std::optional<std::uint64_t> lastSelectionQpc;
    std::optional<double> firstSelectionActiveMs;
    std::optional<double> lastSelectionActiveMs;
    std::optional<std::uint64_t> firstCommandQpc;
    std::optional<std::uint64_t> lastCommandQpc;
    std::optional<double> firstCommandActiveMs;
    std::optional<double> lastCommandActiveMs;
    std::size_t selectionCount{};
    std::size_t commandCount{};
    std::optional<double> assignmentToLastSelectionMs;
    std::optional<double> assignmentToLastCommandMs;
    std::optional<double> scoutingActivityDurationMs;
    std::optional<double> firstToLastCommandMs;
};

// Replay-semantic command evidence for one unit tag. own/enemy spawn and target
// coordinates are map pixels. commandActiveMs is mapped onto captured active game
// time; physical QPC is matched later where possible.
struct ScoutingUnitCommandEvidence {
    std::size_t assignmentEditIndex{};
    std::uint32_t unitTag{};
    double ownSpawnX{};
    double ownSpawnY{};
    double enemySpawnX{};
    double enemySpawnY{};
    double targetX{};
    double targetY{};
    double commandActiveMs{};
};

// Legacy synthetic test seam retained so older unit fixtures can still exercise
// the pre-redesign classifier in isolation. Production replay correlation does
// not use this evidence type.
struct ScoutingUnitTravelEvidence {
    std::size_t assignmentEditIndex{};
    double startX{};
    double startY{};
    double mapCenterX{};
    double mapCenterY{};
    double targetX{};
    double targetY{};
};

struct ArmyControlGroupAnalysis {
    bool available{};
    std::string unavailableReason;
    double activeDurationSeconds{};
    std::vector<ArmyControlGroupEdit> edits;
    std::size_t assignments{};
    std::size_t additions{};
    std::size_t uncertainEdits{};
    std::size_t excludedProductionBuildingEdits{};
    std::size_t excludedScoutingUnitEdits{};
    std::vector<ScoutingUnitActivity> scoutingUnitActivities;
    std::vector<ScoutingUnitCommandEvidence> scoutingUnitCommandEvidence;
    std::array<ArmyControlGroupMethodStatistics, armySelectionMethodCount> assignmentMethods{};
    std::array<ArmyControlGroupMethodStatistics, armySelectionMethodCount> additionMethods{};
    std::array<ArmyControlGroupPerGroupStatistics, 10> byGroup{};

    [[nodiscard]] double assignmentsPerMinute() const noexcept;
    [[nodiscard]] double additionsPerMinute() const noexcept;
    [[nodiscard]] double editsPerMinute() const noexcept;
    [[nodiscard]] double assignPercentage() const noexcept;
    [[nodiscard]] double addPercentage() const noexcept;
};

[[nodiscard]] const char* armyControlGroupOperationName(ArmyControlGroupOperation operation) noexcept;
[[nodiscard]] const char* armySelectionMethodName(ArmySelectionMethod method) noexcept;
[[nodiscard]] const char* armySelectionMethodLabel(ArmySelectionMethod method) noexcept;
[[nodiscard]] std::size_t armySelectionMethodIndex(ArmySelectionMethod method) noexcept;
[[nodiscard]] const char* armyControlGroupBindingConfidenceName(
    ArmyControlGroupBindingConfidence confidence) noexcept;
[[nodiscard]] const char* armyControlGroupScopeName(ArmyControlGroupScope scope) noexcept;

[[nodiscard]] ArmyControlGroupAnalysis detectArmyControlGroupManagement(
    const AnalysisResult& result, std::uint64_t qpcFrequency,
    const ArmyControlGroupDetectionConfig& config = {});
void rebuildArmyControlGroupStatistics(ArmyControlGroupAnalysis& analysis);
void applyScoutingUnitClassification(
    ArmyControlGroupAnalysis& analysis,
    const std::vector<ScoutingUnitCommandEvidence>& commandEvidence = {});
void applyScoutingUnitClassification(
    ArmyControlGroupAnalysis& analysis,
    const std::vector<ScoutingUnitTravelEvidence>& travelEvidence);
void analyzeScoutingUnitActivity(ArmyControlGroupAnalysis& analysis,
                                 const AnalysisResult& result,
                                 std::uint64_t qpcFrequency);

} // namespace smp
