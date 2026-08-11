#pragma once

#include "analysis/analyzer.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smp {

// These are heuristic tuning parameters, not claims about StarCraft's command semantics.
inline constexpr double productionVisitWindowMs = 750.0;
inline constexpr double macroCycleMergeGapMs = 1500.0;
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

struct MacroCycle {
    double startActiveMs{};
    double endActiveMs{};
    double durationMs{};
    std::uint64_t startTimestampTicks{};
    std::uint64_t endTimestampTicks{};
    int productionPresses{};
    int productionVisits{};
    std::vector<int> controlGroups;
};

struct MacroCycleAnalysis {
    bool available{true};
    std::string unavailableReason;
    std::vector<LikelyProductionGroup> likelyProductionGroups;
    std::vector<MacroCycle> cycles;
    std::optional<double> averageDurationMs;
    std::optional<double> bestDurationMs;
    std::optional<double> slowestDurationMs;
};

[[nodiscard]] bool isOrdinaryProductionCommandIdentifier(std::string_view command);
[[nodiscard]] MacroHotkeyProfile parseStarCraftHotkeyProfile(const std::string& settingsJson) noexcept;
[[nodiscard]] MacroHotkeyProfile loadStarCraftHotkeyProfile(const std::filesystem::path& settingsPath) noexcept;
[[nodiscard]] MacroHotkeyProfile loadStarCraftHotkeyProfile() noexcept;

[[nodiscard]] std::vector<LikelyProductionGroup>
inferLikelyProductionGroups(const std::vector<MechanicalInputEvent>& events,
                            const MacroHotkeyProfile& hotkeys, std::uint64_t qpcFrequency);

// Cycle grouping is deliberately separate from the current heuristic inference so a future replay-backed
// classifier can provide likely production groups without changing the physical-execution cycle model.
[[nodiscard]] MacroCycleAnalysis
detectMacroCyclesForLikelyProductionGroups(const std::vector<MechanicalInputEvent>& events,
                                           const MacroHotkeyProfile& hotkeys, std::uint64_t qpcFrequency,
                                           const std::vector<LikelyProductionGroup>& likelyGroups);

[[nodiscard]] MacroCycleAnalysis summarizeMacroCycles(std::vector<MacroCycle> cycles);
[[nodiscard]] MacroCycleAnalysis analyzeMacroCycles(const AnalysisResult& result,
                                                    const MacroHotkeyProfile& hotkeys,
                                                    std::uint64_t qpcFrequency);

} // namespace smp
