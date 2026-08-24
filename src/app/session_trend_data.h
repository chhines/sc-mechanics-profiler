#pragma once

#include "app/session_kpi.h"
#include "cli/automatic_session_stats.h"
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

struct SessionTrendXAxisLimits {
    double minimum;
    double maximum;
};

[[nodiscard]] inline SessionTrendStats sessionTrendStats(
    const AutomaticSessionStats& source) {
    SessionTrendStats stats;
    stats.games = source.games;
    if (source.activeSeconds > 0.0) {
        stats.navigationTransitionsPerMinute =
            source.navigationTransitionsPerMinute();
    }
    stats.workerMacroAverageMs = source.workerMacro.averageDurationMs();
    stats.armyMacroAverageMs = source.armyMacro.averageDurationMs();
    stats.workerMacroCyclesPerMinute = source.workerMacro.cyclesPerMinute();
    stats.armyMacroCyclesPerMinute = source.armyMacro.cyclesPerMinute();
    stats.workerMacroMedianGapMs = source.workerMacro.medianGapMs();
    stats.workerMacroP90GapMs = source.workerMacro.p90GapMs();
    stats.workerMacroLongestGapMs = source.workerMacro.longestGapMs();
    stats.workerMacroGapsOver10SecondsPerGame =
        source.workerMacro.gapsOverPerGame(10000.0);
    stats.workerMacroGapsOver20SecondsPerGame =
        source.workerMacro.gapsOverPerGame(20000.0);
    stats.armyMacroMedianGapMs = source.armyMacro.medianGapMs();
    stats.armyMacroP90GapMs = source.armyMacro.p90GapMs();
    stats.armyMacroLongestGapMs = source.armyMacro.longestGapMs();
    stats.armyMacroGapsOver10SecondsPerGame =
        source.armyMacro.gapsOverPerGame(10000.0);
    stats.armyMacroGapsOver20SecondsPerGame =
        source.armyMacro.gapsOverPerGame(20000.0);
    if (source.armyControlGroupGamesAnalyzed > 0 &&
        source.armyControlGroups.activeDurationSeconds > 0.0) {
        stats.armyControlGroupEditsPerMinute =
            source.armyControlGroups.editsPerMinute();
    }
    stats.armyCommandsPerMinute = source.armyCommands.commandsPerMinute();
    stats.medianArmyCommandGapMs = source.armyCommands.medianGapMs();
    stats.p90ArmyCommandGapMs = source.armyCommands.p90GapMs();
    stats.longestArmyCommandGapMs = source.armyCommands.longestGapMs();
    stats.abilitiesPerMinute = source.abilityActivity.abilitiesPerMinute();
    stats.averageMechanicTypesPerActiveWindow =
        source.multitasking.averageActiveDiversity();
    stats.peakMechanicTypesPerWindow = source.multitasking.peak();
    return stats;
}

[[nodiscard]] inline std::vector<double>
sessionTrendTickValues(std::size_t sessionCount) {
    std::vector<double> ticks;
    ticks.reserve(sessionCount);
    for (std::size_t index = 0; index < sessionCount; ++index)
        ticks.push_back(static_cast<double>(index + 1));
    return ticks;
}

[[nodiscard]] inline SessionTrendXAxisLimits
sessionTrendXAxisLimits(std::size_t sessionCount) noexcept {
    return {1.0, sessionCount > 1 ? static_cast<double>(sessionCount) : 2.0};
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
