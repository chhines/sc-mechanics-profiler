#pragma once

#include "app/gui_preferences.h"
#include "cli/automatic_session_stats.h"
#include "util/json.h"

#include <array>
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

enum class SessionTrendGroup {
    Macro,
    ArmyManagement,
    Multitasking,
};

enum class TrendMetric {
    NavigationRate,
    WorkerMacroDuration,
    ArmyMacroDuration,
    ControlGroupRate,
    ArmyCommandsRate,
    MedianArmyCommandGap,
    P90ArmyCommandGap,
    LongestArmyCommandGap,
    AbilitiesRate,
    AverageMultitaskingDensity,
    PeakMultitaskingDensity,
};

inline constexpr std::array<TrendMetric, 11> trendMetrics{
    TrendMetric::WorkerMacroDuration,
    TrendMetric::ArmyMacroDuration,
    TrendMetric::ControlGroupRate,
    TrendMetric::ArmyCommandsRate,
    TrendMetric::MedianArmyCommandGap,
    TrendMetric::P90ArmyCommandGap,
    TrendMetric::LongestArmyCommandGap,
    TrendMetric::AbilitiesRate,
    TrendMetric::NavigationRate,
    TrendMetric::AverageMultitaskingDensity,
    TrendMetric::PeakMultitaskingDensity,
};

enum class WorkerArmyTrendMetric {
    CyclesPerMinute,
    MedianMacroGap,
    P90MacroGap,
    LongestMacroGap,
    GapsOver10SecondsPerGame,
    GapsOver20SecondsPerGame,
};

inline constexpr std::array<WorkerArmyTrendMetric, 6>
    workerArmyTrendMetrics{
        WorkerArmyTrendMetric::CyclesPerMinute,
        WorkerArmyTrendMetric::MedianMacroGap,
        WorkerArmyTrendMetric::P90MacroGap,
        WorkerArmyTrendMetric::LongestMacroGap,
        WorkerArmyTrendMetric::GapsOver10SecondsPerGame,
        WorkerArmyTrendMetric::GapsOver20SecondsPerGame,
    };

[[nodiscard]] inline SessionTrendGroup trendMetricGroup(
    TrendMetric metric) noexcept {
    switch (metric) {
    case TrendMetric::WorkerMacroDuration:
    case TrendMetric::ArmyMacroDuration:
        return SessionTrendGroup::Macro;
    case TrendMetric::ControlGroupRate:
    case TrendMetric::ArmyCommandsRate:
    case TrendMetric::MedianArmyCommandGap:
    case TrendMetric::P90ArmyCommandGap:
    case TrendMetric::LongestArmyCommandGap:
    case TrendMetric::AbilitiesRate:
        return SessionTrendGroup::ArmyManagement;
    case TrendMetric::NavigationRate:
    case TrendMetric::AverageMultitaskingDensity:
    case TrendMetric::PeakMultitaskingDensity:
        return SessionTrendGroup::Multitasking;
    }
    return SessionTrendGroup::Macro;
}

[[nodiscard]] inline bool trendMetricVisible(
    const SessionReportVisibility& visibility, TrendMetric metric) noexcept {
    switch (metric) {
    case TrendMetric::WorkerMacroDuration:
        return visibility.workerMacroDuration;
    case TrendMetric::ArmyMacroDuration:
        return visibility.armyMacroDuration;
    case TrendMetric::ControlGroupRate:
        return visibility.armyControlGroupManagement;
    case TrendMetric::ArmyCommandsRate:
    case TrendMetric::MedianArmyCommandGap:
    case TrendMetric::P90ArmyCommandGap:
    case TrendMetric::LongestArmyCommandGap:
        return visibility.armyCommandActivity;
    case TrendMetric::AbilitiesRate:
        return visibility.abilityActivity;
    case TrendMetric::NavigationRate:
        return visibility.navigationTransitionRate;
    case TrendMetric::AverageMultitaskingDensity:
    case TrendMetric::PeakMultitaskingDensity:
        return visibility.multitasking;
    }
    return false;
}

[[nodiscard]] inline std::optional<double> metricValue(
    const SessionTrendStats& stats, TrendMetric metric) {
    switch (metric) {
    case TrendMetric::NavigationRate:
        return stats.navigationTransitionsPerMinute;
    case TrendMetric::WorkerMacroDuration:
        return stats.workerMacroAverageMs
                   ? std::optional<double>(*stats.workerMacroAverageMs / 1000.0)
                   : std::nullopt;
    case TrendMetric::ArmyMacroDuration:
        return stats.armyMacroAverageMs
                   ? std::optional<double>(*stats.armyMacroAverageMs / 1000.0)
                   : std::nullopt;
    case TrendMetric::ControlGroupRate:
        return stats.armyControlGroupEditsPerMinute;
    case TrendMetric::ArmyCommandsRate:
        return stats.armyCommandsPerMinute;
    case TrendMetric::MedianArmyCommandGap:
        return stats.medianArmyCommandGapMs
                   ? std::optional<double>(*stats.medianArmyCommandGapMs /
                                           1000.0)
                   : std::nullopt;
    case TrendMetric::P90ArmyCommandGap:
        return stats.p90ArmyCommandGapMs
                   ? std::optional<double>(*stats.p90ArmyCommandGapMs / 1000.0)
                   : std::nullopt;
    case TrendMetric::LongestArmyCommandGap:
        return stats.longestArmyCommandGapMs
                   ? std::optional<double>(*stats.longestArmyCommandGapMs /
                                           1000.0)
                   : std::nullopt;
    case TrendMetric::AbilitiesRate:
        return stats.abilitiesPerMinute;
    case TrendMetric::AverageMultitaskingDensity:
        return stats.averageMechanicTypesPerActiveWindow;
    case TrendMetric::PeakMultitaskingDensity:
        return stats.peakMechanicTypesPerWindow;
    }
    return std::nullopt;
}

[[nodiscard]] inline const char* metricTitle(TrendMetric metric) noexcept {
    switch (metric) {
    case TrendMetric::NavigationRate:
        return "Navigation transitions / minute";
    case TrendMetric::WorkerMacroDuration:
        return "Average worker macro duration";
    case TrendMetric::ArmyMacroDuration:
        return "Average army macro duration";
    case TrendMetric::ControlGroupRate:
        return "Army control-group edits / minute";
    case TrendMetric::ArmyCommandsRate:
        return "Army commands / minute";
    case TrendMetric::MedianArmyCommandGap:
        return "Median Army command gap";
    case TrendMetric::P90ArmyCommandGap:
        return "P90 Army command gap";
    case TrendMetric::LongestArmyCommandGap:
        return "Longest Army command gap";
    case TrendMetric::AbilitiesRate:
        return "Abilities / minute";
    case TrendMetric::AverageMultitaskingDensity:
        return "Average mechanic types / active 5-second window";
    case TrendMetric::PeakMultitaskingDensity:
        return "Peak mechanic types in one 5-second window";
    }
    return "Trend";
}

[[nodiscard]] inline bool metricUsesSeconds(TrendMetric metric) noexcept {
    return metric == TrendMetric::WorkerMacroDuration ||
           metric == TrendMetric::ArmyMacroDuration ||
           metric == TrendMetric::MedianArmyCommandGap ||
           metric == TrendMetric::P90ArmyCommandGap ||
           metric == TrendMetric::LongestArmyCommandGap;
}

[[nodiscard]] inline const char* metricYAxis(TrendMetric metric) noexcept {
    return metricUsesSeconds(metric) ? "Seconds" : metricTitle(metric);
}

[[nodiscard]] inline int metricColorIndex(TrendMetric metric) noexcept {
    switch (metric) {
    case TrendMetric::NavigationRate: return 0;
    case TrendMetric::WorkerMacroDuration: return 1;
    case TrendMetric::ArmyMacroDuration: return 2;
    case TrendMetric::ControlGroupRate: return 3;
    case TrendMetric::ArmyCommandsRate: return 4;
    case TrendMetric::MedianArmyCommandGap: return 5;
    case TrendMetric::P90ArmyCommandGap: return 6;
    case TrendMetric::LongestArmyCommandGap: return 7;
    case TrendMetric::AbilitiesRate: return 8;
    case TrendMetric::AverageMultitaskingDensity: return 9;
    case TrendMetric::PeakMultitaskingDensity: return 10;
    }
    return 0;
}

[[nodiscard]] inline const char* workerArmyTrendTitle(
    WorkerArmyTrendMetric metric) noexcept {
    switch (metric) {
    case WorkerArmyTrendMetric::CyclesPerMinute:
        return "Macro cycles / minute";
    case WorkerArmyTrendMetric::MedianMacroGap:
        return "Median Macro Gap";
    case WorkerArmyTrendMetric::P90MacroGap:
        return "P90 Macro Gap";
    case WorkerArmyTrendMetric::LongestMacroGap:
        return "Longest Macro Gap";
    case WorkerArmyTrendMetric::GapsOver10SecondsPerGame:
        return "Macro gaps >10 s / game";
    case WorkerArmyTrendMetric::GapsOver20SecondsPerGame:
        return "Macro gaps >20 s / game";
    }
    return "Macro trend";
}

[[nodiscard]] inline const char* workerArmyTrendMetricTitle(
    bool worker, WorkerArmyTrendMetric metric) noexcept {
    switch (metric) {
    case WorkerArmyTrendMetric::CyclesPerMinute:
        return worker ? "Worker macro cycles / minute"
                      : "Army macro cycles / minute";
    case WorkerArmyTrendMetric::MedianMacroGap:
        return worker ? "Worker median Macro Gap" : "Army median Macro Gap";
    case WorkerArmyTrendMetric::P90MacroGap:
        return worker ? "Worker P90 Macro Gap" : "Army P90 Macro Gap";
    case WorkerArmyTrendMetric::LongestMacroGap:
        return worker ? "Worker longest Macro Gap"
                      : "Army longest Macro Gap";
    case WorkerArmyTrendMetric::GapsOver10SecondsPerGame:
        return worker ? "Worker Macro gaps >10 s / game"
                      : "Army Macro gaps >10 s / game";
    case WorkerArmyTrendMetric::GapsOver20SecondsPerGame:
        return worker ? "Worker Macro gaps >20 s / game"
                      : "Army Macro gaps >20 s / game";
    }
    return "Macro trend";
}

[[nodiscard]] inline bool workerArmyTrendUsesSeconds(
    WorkerArmyTrendMetric metric) noexcept {
    return metric == WorkerArmyTrendMetric::MedianMacroGap ||
           metric == WorkerArmyTrendMetric::P90MacroGap ||
           metric == WorkerArmyTrendMetric::LongestMacroGap;
}

[[nodiscard]] inline std::optional<double> workerArmyTrendValue(
    const SessionTrendStats& stats, bool worker,
    WorkerArmyTrendMetric metric) {
    std::optional<double> value;
    switch (metric) {
    case WorkerArmyTrendMetric::CyclesPerMinute:
        value = worker ? stats.workerMacroCyclesPerMinute
                       : stats.armyMacroCyclesPerMinute;
        break;
    case WorkerArmyTrendMetric::MedianMacroGap:
        value = worker ? stats.workerMacroMedianGapMs
                       : stats.armyMacroMedianGapMs;
        break;
    case WorkerArmyTrendMetric::P90MacroGap:
        value = worker ? stats.workerMacroP90GapMs
                       : stats.armyMacroP90GapMs;
        break;
    case WorkerArmyTrendMetric::LongestMacroGap:
        value = worker ? stats.workerMacroLongestGapMs
                       : stats.armyMacroLongestGapMs;
        break;
    case WorkerArmyTrendMetric::GapsOver10SecondsPerGame:
        value = worker ? stats.workerMacroGapsOver10SecondsPerGame
                       : stats.armyMacroGapsOver10SecondsPerGame;
        break;
    case WorkerArmyTrendMetric::GapsOver20SecondsPerGame:
        value = worker ? stats.workerMacroGapsOver20SecondsPerGame
                       : stats.armyMacroGapsOver20SecondsPerGame;
        break;
    }
    if (value && workerArmyTrendUsesSeconds(metric))
        return *value / 1000.0;
    return value;
}

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
