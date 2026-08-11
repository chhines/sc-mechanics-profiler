#pragma once

#include "analysis/analyzer.h"

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
inline constexpr double productionAccessNavigationWindowMs = 2000.0;
inline constexpr double replaySelectionMatchWindowMs = 300.0;
inline constexpr double replayProductionMatchWindowMs = 650.0;
inline constexpr double workerMacroMergeGapMs = 2500.0;
inline constexpr double workerMacroMaximumDurationMs = 8000.0;
inline constexpr double armyMacroMergeGapMs = 2500.0;
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

enum class ProductionAccessMethod : std::uint8_t {
    ControlGroup,
    LocationHotkeyClick,
    MinimapClick,
    ScreenClick
};

struct ProductionVisit {
    MacroProductType productType{MacroProductType::Unknown};
    ProductionAccessMethod accessMethod{ProductionAccessMethod::ScreenClick};
    double startActiveMs{};
    double endActiveMs{};
    double durationMs{};
    std::uint64_t startTimestampTicks{};
    std::uint64_t endTimestampTicks{};
    double contextActiveMs{};
    std::uint64_t contextTimestampTicks{};
    int controlGroup{-1};
    int locationHotkey{-1};
    int physicalProductionPresses{};
    std::vector<std::uint16_t> physicalProductionKeys;
    int replayProductionCommands{};
    std::vector<std::string> producedUnits;
    bool replayConfirmed{};
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
    std::string parser;
};

struct ProductionAnalysis {
    bool visitsAvailable{};
    std::string visitsUnavailableReason;
    std::vector<LikelyProductionGroup> likelyProductionGroups;
    std::vector<ProductionVisit> productionVisits;
    ProductMacroCycleAnalysis workerMacroCycles;
    ProductMacroCycleAnalysis armyMacroCycles;
    ReplayCorrelationDiagnostics replayCorrelation;
};

[[nodiscard]] const char* macroProductTypeName(MacroProductType type) noexcept;
[[nodiscard]] const char* productionAccessMethodName(ProductionAccessMethod method) noexcept;
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
                      std::uint64_t qpcFrequency);

[[nodiscard]] ProductionAnalysis analyzeProductionVisits(const AnalysisResult& result,
                                                          const MacroHotkeyProfile& hotkeys,
                                                          std::uint64_t qpcFrequency);

} // namespace smp
