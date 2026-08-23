#pragma once

#include "analysis/analyzer.h"
#include "analysis/army_command.h"
#include "analysis/army_control_group.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smp {

// These are heuristic tuning parameters, not claims about StarCraft's command semantics.
inline constexpr double productionVisitWindowMs = 750.0;
inline constexpr double productionBurstContinuationGapMs = 500.0;
inline constexpr double productionAccessNavigationWindowMs = 2000.0;
inline constexpr double replaySelectionMatchWindowMs = 300.0;
inline constexpr double replayProductionMatchWindowMs = 650.0;
inline constexpr double workerMacroMergeGapMs = 2000.0;
inline constexpr double workerMacroMaximumDurationMs = 8000.0;
inline constexpr double armyMacroMergeGapMs = 2000.0;
inline constexpr double armyMacroMaximumDurationMs = 10000.0;
inline constexpr double qpcActivePauseToleranceMs = 25.0;
inline constexpr int minimumProductionVisits = 2;
inline constexpr int minimumProductionPresses = 3;

struct StarCraftHotkey {
    std::string command;
    std::string boundKey;
    std::uint16_t virtualKey{};
};

struct MacroHotkeyProfile {
    bool available{};
    std::string unavailableReason;
    std::string source{"CSettings.json"};
    std::optional<bool> customHotkeysEnabled;
    std::vector<StarCraftHotkey> parsedBindings;
    std::vector<StarCraftHotkey> productionCommands;

    [[nodiscard]] std::vector<std::string> compatibleProductionCommands(std::uint16_t virtualKey) const;
};

struct LikelyProductionGroup {
    int group{};
    std::vector<std::uint16_t> observedProductionKeys;
};

enum class MacroProductType : std::uint8_t {
    Worker,
    Army,
    Unknown
};

// Legacy combined access categories retained for the existing summary. New telemetry
// should use ProductionSelectionAccess and ProductionCameraAccess independently.
enum class ProductionAccessMethod : std::uint8_t {
    ControlGroup,
    LocationHotkeyClick,
    MinimapClick,
    ScreenClick
};

enum class ProductionSelectionAccess : std::uint8_t {
    ControlGroup,
    DirectClick,
    BoxSelect,
    Other
};

enum class ProductionCameraAccess : std::uint8_t {
    None,
    LocationHotkey,
    ControlGroupDoubleTap,
    Minimap,
    EdgeScroll,
    Other
};

enum class ProductionCameraAnchorKind : std::uint8_t {
    None,
    LocationHotkey,
    ControlGroup,
    Minimap,
    EdgeScroll,
    Other
};

enum class MacroAccessStyle : std::uint8_t {
    ControlGroupOnly,
    LocationHotkeyClick,
    ControlGroupCenterClick,
    Mixed,
    Other
};

inline constexpr std::size_t macroAccessStyleCount = 5;

enum class ProductionContextKind : std::uint8_t {
    ReplaySelection,
    ControlGroup,
    LocationHotkey,
    Unknown
};

struct ProductionContextId {
    ProductionContextKind kind{ProductionContextKind::Unknown};
    std::vector<std::uint32_t> unitTags;
    int controlGroup{-1};
    int locationHotkey{-1};
    std::uint32_t assignmentGeneration{};
};

struct ProductionVisit {
    MacroProductType productType{MacroProductType::Unknown};
    ProductionAccessMethod accessMethod{ProductionAccessMethod::ScreenClick};
    ProductionSelectionAccess selectionAccess{ProductionSelectionAccess::Other};
    ProductionCameraAccess cameraAccess{ProductionCameraAccess::None};
    ProductionCameraAnchorKind cameraAnchorKind{ProductionCameraAnchorKind::None};
    std::uint64_t cameraEpisodeId{};
    int cameraAnchorId{-1};
    std::uint64_t cameraAnchorTimestampTicks{};
    double startActiveMs{};
    std::uint64_t startTimestampTicks{};
    double contextActiveMs{};
    std::uint64_t contextTimestampTicks{};
    double firstProductionActiveMs{};
    std::uint64_t firstProductionTimestampTicks{};
    double endActiveMs{};
    std::uint64_t endTimestampTicks{};
    double accessLatencyMs{};
    double productionLatencyMs{};
    double executionDurationMs{};
    double productionBurstSpanMs{};
    double durationMs{};
    int controlGroup{-1};
    int locationHotkey{-1};
    int physicalProductionPresses{};
    std::vector<std::uint16_t> physicalProductionKeys;
    int replayProductionCommands{};
    std::vector<std::string> producedUnits;
    bool replayConfirmed{};
    ProductionContextId productionContext;
};

struct ControlGroupProductionCandidate {
    ProductionVisit visit;
    std::size_t selectMechanicalEventIndex{};
    std::vector<std::size_t> productionMechanicalEventIndices;
    bool mouseContradiction{};
};

struct MacroCycle {
    MacroProductType productType{MacroProductType::Unknown};
    double startActiveMs{};
    double endActiveMs{};
    double durationMs{};
    std::uint64_t startTimestampTicks{};
    std::uint64_t endTimestampTicks{};
    std::vector<std::size_t> visitIndices;
    double executionEndActiveMs{};
    double fullSpanMs{};
    std::uint64_t executionEndTimestampTicks{};
    MacroAccessStyle macroAccessStyle{MacroAccessStyle::Other};
    std::size_t controlGroupVisitCount{};
    std::size_t directClickVisitCount{};
    std::size_t boxSelectVisitCount{};
    std::size_t cameraEpisodeCount{};
};

struct MacroAccessStyleStatistics {
    std::size_t cycleCount{};
    std::optional<double> averageDurationMs;
    std::optional<double> medianDurationMs;
    std::optional<double> bestDurationMs;
    std::optional<double> p25DurationMs;
    std::optional<double> p75DurationMs;
    std::optional<double> p90DurationMs;
};

struct AssignmentInterruptionSplit {
    std::size_t previousVisitIndex{};
    std::size_t nextVisitIndex{};
    MechanicalInputType interruptionType{MechanicalInputType::ControlGroupAssign};
    double interruptionActiveMs{};
    std::uint64_t interruptionTimestampTicks{};
};

struct ProductMacroCycleAnalysis {
    bool available{};
    std::string unavailableReason;
    MacroProductType productType{MacroProductType::Unknown};
    std::vector<MacroCycle> cycles;
    std::optional<double> averageDurationMs;
    std::optional<double> bestDurationMs;
    std::optional<double> slowestDurationMs;
    std::size_t productionVisitCount{};
    std::array<std::size_t, 4> accessMethodCounts{};
    std::array<MacroAccessStyleStatistics, macroAccessStyleCount> accessStyleStatistics{};
    std::size_t repeatedContextSplits{};
    std::vector<std::size_t> repeatedContextSplitVisitIndices;
    std::size_t assignmentInterruptionSplits{};
    std::vector<AssignmentInterruptionSplit> assignmentInterruptionSplitDetails;
};

struct ReplayCorrelationDiagnostics {
    bool available{};
    std::string unavailableReason;
    int playerId{-1};
    std::string playerName;
    double sequenceScore{};
    double runnerUpSequenceScore{};
    std::size_t matchedControlGroupEvents{};
    std::size_t timelineAnchors{};
    std::size_t matchedProductionVisits{};
    std::size_t unmatchedProductionVisits{};
    std::size_t replayCreatedControlGroupVisits{};
    std::size_t matchedClickVisits{};
    std::size_t matchedReplayProductionEvents{};
    std::size_t unmatchedReplayProductionEvents{};
    std::size_t extendedProductionVisits{};
    std::size_t extendedPhysicalProductionPresses{};
    std::string parser;
};

struct ProductionAnalysis {
    bool visitsAvailable{};
    std::string visitsUnavailableReason;
    std::vector<LikelyProductionGroup> likelyProductionGroups;
    std::vector<ProductionVisit> productionVisits;
    ProductMacroCycleAnalysis workerMacroCycles;
    ProductMacroCycleAnalysis armyMacroCycles;
    ArmyControlGroupAnalysis armyControlGroupManagement;
    ArmyCommandAnalysis armyCommandActivity;
    ReplayCorrelationDiagnostics replayCorrelation;
};

[[nodiscard]] const char* macroProductTypeName(MacroProductType type) noexcept;
[[nodiscard]] const char* productionAccessMethodName(ProductionAccessMethod method) noexcept;
[[nodiscard]] const char*
productionSelectionAccessName(ProductionSelectionAccess access) noexcept;
[[nodiscard]] const char* productionCameraAccessName(ProductionCameraAccess access) noexcept;
[[nodiscard]] const char*
productionCameraAnchorKindName(ProductionCameraAnchorKind kind) noexcept;
[[nodiscard]] const char* macroAccessStyleName(MacroAccessStyle style) noexcept;
[[nodiscard]] std::size_t macroAccessStyleIndex(MacroAccessStyle style) noexcept;
[[nodiscard]] const char* productionContextKindName(ProductionContextKind kind) noexcept;
[[nodiscard]] ProductionContextId
makeReplaySelectionProductionContext(std::vector<std::uint32_t> unitTags);
[[nodiscard]] ProductionContextId
makeControlGroupProductionContext(int controlGroup,
                                  std::uint32_t assignmentGeneration = 0) noexcept;
[[nodiscard]] ProductionContextId
makeLocationHotkeyProductionContext(int locationHotkey,
                                    std::uint32_t assignmentGeneration = 0) noexcept;
[[nodiscard]] bool knownProductionContext(const ProductionContextId& context) noexcept;
[[nodiscard]] bool sameProductionContext(const ProductionContextId& first,
                                         const ProductionContextId& second) noexcept;
void refreshProductionVisitTiming(ProductionVisit& visit,
                                  std::uint64_t qpcFrequency) noexcept;
void annotateProductionAccessTelemetry(std::vector<ProductionVisit>& visits,
                                       const AnalysisResult& result);
[[nodiscard]] MacroAccessStyleStatistics
summarizeMacroAccessStyleDurations(std::vector<double> durationsMs);
[[nodiscard]] double macroAccessStylePercentage(const ProductMacroCycleAnalysis& analysis,
                                                MacroAccessStyle style) noexcept;
[[nodiscard]] bool isOrdinaryProductionCommandIdentifier(std::string_view command);
[[nodiscard]] MacroHotkeyProfile parseStarCraftHotkeyProfile(const std::string& settingsJson) noexcept;
[[nodiscard]] MacroHotkeyProfile loadStarCraftHotkeyProfile(const std::filesystem::path& settingsPath) noexcept;
[[nodiscard]] MacroHotkeyProfile loadStarCraftHotkeyProfile() noexcept;

[[nodiscard]] std::vector<LikelyProductionGroup>
inferLikelyProductionGroups(const std::vector<MechanicalInputEvent>& events,
                            const MacroHotkeyProfile& hotkeys, std::uint64_t qpcFrequency);

[[nodiscard]] std::vector<ProductionVisit>
detectHeuristicProductionVisitsForLikelyGroups(const AnalysisResult& result,
                                               const MacroHotkeyProfile& hotkeys,
                                               std::uint64_t qpcFrequency,
                                               const std::vector<LikelyProductionGroup>& likelyGroups);

[[nodiscard]] std::vector<ControlGroupProductionCandidate>
detectControlGroupProductionCandidates(const AnalysisResult& result,
                                       const MacroHotkeyProfile& hotkeys,
                                       std::uint64_t qpcFrequency);

[[nodiscard]] ProductMacroCycleAnalysis
summarizeProductMacroCycles(MacroProductType productType, std::vector<MacroCycle> cycles,
                            const std::vector<ProductionVisit>& visits);

[[nodiscard]] ProductMacroCycleAnalysis
groupProductionVisits(const std::vector<ProductionVisit>& visits, MacroProductType productType,
                      const std::vector<MechanicalInputEvent>& mechanicalEvents,
                      std::uint64_t qpcFrequency);
[[nodiscard]] ProductMacroCycleAnalysis
groupProductionVisits(const std::vector<ProductionVisit>& visits, MacroProductType productType,
                      std::uint64_t qpcFrequency);

[[nodiscard]] ProductionAnalysis analyzeProductionVisits(const AnalysisResult& result,
                                                          const MacroHotkeyProfile& hotkeys,
                                                          std::uint64_t qpcFrequency);

} // namespace smp
