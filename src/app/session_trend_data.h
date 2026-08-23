#pragma once

#include "util/json.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace smp {

struct SessionMacroGapTrendValues {
    std::optional<double> workerMedianMs;
    std::optional<double> workerP90Ms;
    std::optional<double> armyMedianMs;
    std::optional<double> armyP90Ms;
};

struct SessionTrendStats {
    std::uint64_t games{};
    std::optional<double> navigationTransitionsPerMinute;
    std::optional<double> workerMacroAverageMs;
    std::optional<double> armyMacroAverageMs;
    std::optional<double> workerMacroCyclesPerMinute;
    std::optional<double> armyMacroCyclesPerMinute;
    std::optional<double> workerMacroMedianGapMs;
    std::optional<double> workerMacroP90GapMs;
    std::optional<double> workerMacroLongestGapMs;
    std::optional<double> workerMacroGapsOver10SecondsPerGame;
    std::optional<double> workerMacroGapsOver20SecondsPerGame;
    std::optional<double> armyMacroMedianGapMs;
    std::optional<double> armyMacroP90GapMs;
    std::optional<double> armyMacroLongestGapMs;
    std::optional<double> armyMacroGapsOver10SecondsPerGame;
    std::optional<double> armyMacroGapsOver20SecondsPerGame;
    std::optional<double> armyControlGroupEditsPerMinute;
    std::optional<double> armyCommandsPerMinute;
    std::optional<double> medianArmyCommandGapMs;
    std::optional<double> p90ArmyCommandGapMs;
    std::optional<double> longestArmyCommandGapMs;
    std::optional<double> abilitiesPerMinute;
    std::optional<double> averageMechanicTypesPerActiveWindow;
    std::optional<double> peakMechanicTypesPerWindow;
};

[[nodiscard]] inline std::vector<double>
sessionTrendTickValues(std::size_t sessionCount) {
    std::vector<double> ticks;
    ticks.reserve(sessionCount);
    for (std::size_t index = 0; index < sessionCount; ++index)
        ticks.push_back(static_cast<double>(index + 1));
    return ticks;
}

[[nodiscard]] inline std::optional<double>
sessionTrendNumber(const json::Value& value) {
    return value.isNumber() ? std::optional<double>(value.asNumber())
                            : std::nullopt;
}

[[nodiscard]] inline SessionMacroGapTrendValues
decodeSessionMacroGapTrendValues(const json::Value& stats) {
    return {
        sessionTrendNumber(stats["worker_macro"]["median_gap_ms"]),
        sessionTrendNumber(stats["worker_macro"]["p90_gap_ms"]),
        sessionTrendNumber(stats["army_macro"]["median_gap_ms"]),
        sessionTrendNumber(stats["army_macro"]["p90_gap_ms"]),
    };
}

[[nodiscard]] inline SessionTrendStats
decodeSessionTrendStats(const json::Value& value) {
    SessionTrendStats stats;
    stats.games = static_cast<std::uint64_t>(
        value["games"].asNumber() > 0.0 ? value["games"].asNumber() : 0.0);
    stats.navigationTransitionsPerMinute =
        sessionTrendNumber(value["navigation"]["transitions_per_minute"]);
    stats.workerMacroAverageMs =
        sessionTrendNumber(value["worker_macro"]["average_duration_ms"]);
    stats.armyMacroAverageMs =
        sessionTrendNumber(value["army_macro"]["average_duration_ms"]);
    stats.workerMacroCyclesPerMinute =
        sessionTrendNumber(value["worker_macro"]["cycles_per_minute"]);
    stats.armyMacroCyclesPerMinute =
        sessionTrendNumber(value["army_macro"]["cycles_per_minute"]);
    stats.workerMacroMedianGapMs =
        sessionTrendNumber(value["worker_macro"]["median_gap_ms"]);
    stats.workerMacroP90GapMs =
        sessionTrendNumber(value["worker_macro"]["p90_gap_ms"]);
    stats.workerMacroLongestGapMs =
        sessionTrendNumber(value["worker_macro"]["longest_gap_ms"]);
    stats.workerMacroGapsOver10SecondsPerGame = sessionTrendNumber(
        value["worker_macro"]["gaps_over_10s_per_game"]);
    stats.workerMacroGapsOver20SecondsPerGame = sessionTrendNumber(
        value["worker_macro"]["gaps_over_20s_per_game"]);
    stats.armyMacroMedianGapMs =
        sessionTrendNumber(value["army_macro"]["median_gap_ms"]);
    stats.armyMacroP90GapMs =
        sessionTrendNumber(value["army_macro"]["p90_gap_ms"]);
    stats.armyMacroLongestGapMs =
        sessionTrendNumber(value["army_macro"]["longest_gap_ms"]);
    stats.armyMacroGapsOver10SecondsPerGame = sessionTrendNumber(
        value["army_macro"]["gaps_over_10s_per_game"]);
    stats.armyMacroGapsOver20SecondsPerGame = sessionTrendNumber(
        value["army_macro"]["gaps_over_20s_per_game"]);
    stats.armyControlGroupEditsPerMinute = sessionTrendNumber(
        value["army_control_groups"]["edits_per_minute"]);
    stats.armyCommandsPerMinute =
        sessionTrendNumber(value["army_commands"]["commands_per_minute"]);
    stats.medianArmyCommandGapMs =
        sessionTrendNumber(value["army_commands"]["median_gap_ms"]);
    stats.p90ArmyCommandGapMs =
        sessionTrendNumber(value["army_commands"]["p90_gap_ms"]);
    stats.longestArmyCommandGapMs =
        sessionTrendNumber(value["army_commands"]["longest_gap_ms"]);
    stats.abilitiesPerMinute = sessionTrendNumber(
        value["ability_activity"]["abilities_per_minute"]);
    stats.averageMechanicTypesPerActiveWindow = sessionTrendNumber(
        value["multitasking"]["average_active_diversity"]);
    stats.peakMechanicTypesPerWindow =
        sessionTrendNumber(value["multitasking"]["peak_diversity"]);
    return stats;
}

} // namespace smp
