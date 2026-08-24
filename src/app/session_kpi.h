#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace smp {

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

enum class SessionKpiGroup {
    Navigation,
    WorkerMacro,
    ArmyMacro,
    ArmyManagement,
    Multitasking,
    Count,
};

struct SessionKpiGroupDefinition {
    SessionKpiGroup group;
    const char* id;
    const char* title;
};

inline constexpr std::array<SessionKpiGroupDefinition, 5>
    sessionKpiGroupDefinitions{{
        {SessionKpiGroup::Navigation, "session_navigation", "Navigation"},
        {SessionKpiGroup::WorkerMacro, "session_worker_macro",
         "Worker Macro"},
        {SessionKpiGroup::ArmyMacro, "session_army_macro", "Army Macro"},
        {SessionKpiGroup::ArmyManagement, "session_army_management",
         "Army Management"},
        {SessionKpiGroup::Multitasking, "session_multitasking",
         "Multitasking"},
    }};

static_assert(sessionKpiGroupDefinitions.size() ==
              static_cast<std::size_t>(SessionKpiGroup::Count));
static_assert([] {
    for (std::size_t index = 0; index < sessionKpiGroupDefinitions.size();
         ++index) {
        if (static_cast<std::size_t>(
                sessionKpiGroupDefinitions[index].group) != index) {
            return false;
        }
    }
    return true;
}());

enum class SessionKpi {
    NavigationTransitionsPerMinute,
    WorkerMacroAverageDuration,
    WorkerMacroCyclesPerMinute,
    WorkerMacroMedianGap,
    WorkerMacroP90Gap,
    WorkerMacroLongestGap,
    WorkerMacroGapsOver10PerGame,
    WorkerMacroGapsOver20PerGame,
    ArmyMacroAverageDuration,
    ArmyMacroCyclesPerMinute,
    ArmyMacroMedianGap,
    ArmyMacroP90Gap,
    ArmyMacroLongestGap,
    ArmyMacroGapsOver10PerGame,
    ArmyMacroGapsOver20PerGame,
    ArmyControlGroupEditsPerMinute,
    ArmyCommandsPerMinute,
    MedianArmyCommandGap,
    P90ArmyCommandGap,
    LongestArmyCommandGap,
    AbilitiesPerMinute,
    AverageMultitaskingDensity,
    PeakMultitaskingDensity,
    Count,
};

inline constexpr std::size_t sessionKpiCount =
    static_cast<std::size_t>(SessionKpi::Count);

enum class SessionKpiUnit {
    Scalar,
    Seconds,
};

struct SessionKpiDefinition {
    SessionKpi kpi;
    const char* preferenceKey;
    const char* title;
    const char* settingsLabel;
    SessionKpiGroup group;
    SessionKpiUnit unit;
    bool visibleByDefault;
    int colorIndex;
    std::optional<double> SessionTrendStats::*value;
    double scale;
};

inline constexpr std::array<SessionKpiDefinition, sessionKpiCount>
    sessionKpiDefinitions{{
        {SessionKpi::NavigationTransitionsPerMinute,
         "navigation_transitions_per_minute",
         "Navigation transitions / minute",
         "Navigation transitions / minute", SessionKpiGroup::Navigation,
         SessionKpiUnit::Scalar, true, 0,
         &SessionTrendStats::navigationTransitionsPerMinute, 1.0},
        {SessionKpi::WorkerMacroAverageDuration,
         "worker_macro_average_duration",
         "Average worker macro duration", "Average macro duration",
         SessionKpiGroup::WorkerMacro, SessionKpiUnit::Seconds, true, 1,
         &SessionTrendStats::workerMacroAverageMs, 0.001},
        {SessionKpi::WorkerMacroCyclesPerMinute,
         "worker_macro_cycles_per_minute", "Worker macro cycles / minute",
         "Macro cycles / minute", SessionKpiGroup::WorkerMacro,
         SessionKpiUnit::Scalar, true, 1,
         &SessionTrendStats::workerMacroCyclesPerMinute, 1.0},
        {SessionKpi::WorkerMacroMedianGap, "worker_macro_median_gap",
         "Worker median Macro Gap", "Median Macro Gap",
         SessionKpiGroup::WorkerMacro, SessionKpiUnit::Seconds, true, 1,
         &SessionTrendStats::workerMacroMedianGapMs, 0.001},
        {SessionKpi::WorkerMacroP90Gap, "worker_macro_p90_gap",
         "Worker P90 Macro Gap", "P90 Macro Gap",
         SessionKpiGroup::WorkerMacro, SessionKpiUnit::Seconds, false, 1,
         &SessionTrendStats::workerMacroP90GapMs, 0.001},
        {SessionKpi::WorkerMacroLongestGap, "worker_macro_longest_gap",
         "Worker longest Macro Gap", "Longest Macro Gap",
         SessionKpiGroup::WorkerMacro, SessionKpiUnit::Seconds, false, 1,
         &SessionTrendStats::workerMacroLongestGapMs, 0.001},
        {SessionKpi::WorkerMacroGapsOver10PerGame,
         "worker_macro_gaps_over_10_seconds_per_game",
         "Worker Macro gaps >10 s / game", "Macro gaps >10 s / game",
         SessionKpiGroup::WorkerMacro, SessionKpiUnit::Scalar, false, 1,
         &SessionTrendStats::workerMacroGapsOver10SecondsPerGame, 1.0},
        {SessionKpi::WorkerMacroGapsOver20PerGame,
         "worker_macro_gaps_over_20_seconds_per_game",
         "Worker Macro gaps >20 s / game", "Macro gaps >20 s / game",
         SessionKpiGroup::WorkerMacro, SessionKpiUnit::Scalar, false, 1,
         &SessionTrendStats::workerMacroGapsOver20SecondsPerGame, 1.0},
        {SessionKpi::ArmyMacroAverageDuration,
         "army_macro_average_duration", "Average army macro duration",
         "Average macro duration", SessionKpiGroup::ArmyMacro,
         SessionKpiUnit::Seconds, true, 2,
         &SessionTrendStats::armyMacroAverageMs, 0.001},
        {SessionKpi::ArmyMacroCyclesPerMinute,
         "army_macro_cycles_per_minute", "Army macro cycles / minute",
         "Macro cycles / minute", SessionKpiGroup::ArmyMacro,
         SessionKpiUnit::Scalar, true, 2,
         &SessionTrendStats::armyMacroCyclesPerMinute, 1.0},
        {SessionKpi::ArmyMacroMedianGap, "army_macro_median_gap",
         "Army median Macro Gap", "Median Macro Gap",
         SessionKpiGroup::ArmyMacro, SessionKpiUnit::Seconds, true, 2,
         &SessionTrendStats::armyMacroMedianGapMs, 0.001},
        {SessionKpi::ArmyMacroP90Gap, "army_macro_p90_gap",
         "Army P90 Macro Gap", "P90 Macro Gap",
         SessionKpiGroup::ArmyMacro, SessionKpiUnit::Seconds, false, 2,
         &SessionTrendStats::armyMacroP90GapMs, 0.001},
        {SessionKpi::ArmyMacroLongestGap, "army_macro_longest_gap",
         "Army longest Macro Gap", "Longest Macro Gap",
         SessionKpiGroup::ArmyMacro, SessionKpiUnit::Seconds, false, 2,
         &SessionTrendStats::armyMacroLongestGapMs, 0.001},
        {SessionKpi::ArmyMacroGapsOver10PerGame,
         "army_macro_gaps_over_10_seconds_per_game",
         "Army Macro gaps >10 s / game", "Macro gaps >10 s / game",
         SessionKpiGroup::ArmyMacro, SessionKpiUnit::Scalar, false, 2,
         &SessionTrendStats::armyMacroGapsOver10SecondsPerGame, 1.0},
        {SessionKpi::ArmyMacroGapsOver20PerGame,
         "army_macro_gaps_over_20_seconds_per_game",
         "Army Macro gaps >20 s / game", "Macro gaps >20 s / game",
         SessionKpiGroup::ArmyMacro, SessionKpiUnit::Scalar, false, 2,
         &SessionTrendStats::armyMacroGapsOver20SecondsPerGame, 1.0},
        {SessionKpi::ArmyControlGroupEditsPerMinute,
         "army_control_group_edits_per_minute",
         "Army control-group edits / minute",
         "Control-group edits / minute", SessionKpiGroup::ArmyManagement,
         SessionKpiUnit::Scalar, true, 3,
         &SessionTrendStats::armyControlGroupEditsPerMinute, 1.0},
        {SessionKpi::ArmyCommandsPerMinute, "army_commands_per_minute",
         "Army commands / minute", "Army commands / minute",
         SessionKpiGroup::ArmyManagement, SessionKpiUnit::Scalar, true, 4,
         &SessionTrendStats::armyCommandsPerMinute, 1.0},
        {SessionKpi::MedianArmyCommandGap, "median_army_command_gap",
         "Median Army command gap", "Median Army command gap",
         SessionKpiGroup::ArmyManagement, SessionKpiUnit::Seconds, true, 5,
         &SessionTrendStats::medianArmyCommandGapMs, 0.001},
        {SessionKpi::P90ArmyCommandGap, "p90_army_command_gap",
         "P90 Army command gap", "P90 Army command gap",
         SessionKpiGroup::ArmyManagement, SessionKpiUnit::Seconds, false, 6,
         &SessionTrendStats::p90ArmyCommandGapMs, 0.001},
        {SessionKpi::LongestArmyCommandGap, "longest_army_command_gap",
         "Longest Army command gap", "Longest Army command gap",
         SessionKpiGroup::ArmyManagement, SessionKpiUnit::Seconds, false, 7,
         &SessionTrendStats::longestArmyCommandGapMs, 0.001},
        {SessionKpi::AbilitiesPerMinute, "abilities_per_minute",
         "Abilities / minute", "Abilities / minute",
         SessionKpiGroup::ArmyManagement, SessionKpiUnit::Scalar, false, 8,
         &SessionTrendStats::abilitiesPerMinute, 1.0},
        {SessionKpi::AverageMultitaskingDensity,
         "average_multitasking_density",
         "Average mechanic types / active 5-second window",
         "Average mechanic types / active 5-second window",
         SessionKpiGroup::Multitasking, SessionKpiUnit::Scalar, true, 9,
         &SessionTrendStats::averageMechanicTypesPerActiveWindow, 1.0},
        {SessionKpi::PeakMultitaskingDensity, "peak_multitasking_density",
         "Peak mechanic types in one 5-second window",
         "Peak mechanic types in one 5-second window",
         SessionKpiGroup::Multitasking, SessionKpiUnit::Scalar, false, 10,
         &SessionTrendStats::peakMechanicTypesPerWindow, 1.0},
    }};

[[nodiscard]] inline constexpr bool sessionKpiDefinitionsAreIndexed()
    noexcept {
    for (std::size_t index = 0; index < sessionKpiDefinitions.size(); ++index) {
        if (static_cast<std::size_t>(sessionKpiDefinitions[index].kpi) !=
            index) {
            return false;
        }
    }
    return true;
}

static_assert(sessionKpiDefinitionsAreIndexed());

struct SessionKpiPairDefinition {
    SessionKpi worker;
    SessionKpi army;
    const char* title;
};

inline constexpr std::array<SessionKpiPairDefinition, 6>
    sessionKpiPairDefinitions{{
        {SessionKpi::WorkerMacroCyclesPerMinute,
         SessionKpi::ArmyMacroCyclesPerMinute, "Macro cycles / minute"},
        {SessionKpi::WorkerMacroMedianGap,
         SessionKpi::ArmyMacroMedianGap, "Median Macro Gap"},
        {SessionKpi::WorkerMacroP90Gap, SessionKpi::ArmyMacroP90Gap,
         "P90 Macro Gap"},
        {SessionKpi::WorkerMacroLongestGap,
         SessionKpi::ArmyMacroLongestGap, "Longest Macro Gap"},
        {SessionKpi::WorkerMacroGapsOver10PerGame,
         SessionKpi::ArmyMacroGapsOver10PerGame,
         "Macro gaps >10 s / game"},
        {SessionKpi::WorkerMacroGapsOver20PerGame,
         SessionKpi::ArmyMacroGapsOver20PerGame,
         "Macro gaps >20 s / game"},
    }};

[[nodiscard]] inline constexpr bool sessionKpiUsesPairedPlot(
    SessionKpi kpi) noexcept {
    for (const auto& pair : sessionKpiPairDefinitions) {
        if (pair.worker == kpi || pair.army == kpi)
            return true;
    }
    return false;
}

[[nodiscard]] inline constexpr std::size_t sessionKpiIndex(
    SessionKpi kpi) noexcept {
    return static_cast<std::size_t>(kpi);
}

[[nodiscard]] inline constexpr const SessionKpiDefinition&
sessionKpiDefinition(SessionKpi kpi) noexcept {
    return sessionKpiDefinitions[sessionKpiIndex(kpi)];
}

[[nodiscard]] inline constexpr const char* sessionKpiGroupTitle(
    SessionKpiGroup group) noexcept {
    return sessionKpiGroupDefinitions[static_cast<std::size_t>(group)].title;
}

[[nodiscard]] inline constexpr const char* sessionKpiGroupId(
    SessionKpiGroup group) noexcept {
    return sessionKpiGroupDefinitions[static_cast<std::size_t>(group)].id;
}

[[nodiscard]] inline std::optional<double> sessionKpiValue(
    const SessionTrendStats& stats, SessionKpi kpi) {
    const auto& definition = sessionKpiDefinition(kpi);
    const auto& value = stats.*(definition.value);
    return value ? std::optional<double>(*value * definition.scale)
                 : std::nullopt;
}

[[nodiscard]] inline constexpr bool sessionKpiUsesSeconds(
    SessionKpi kpi) noexcept {
    return sessionKpiDefinition(kpi).unit == SessionKpiUnit::Seconds;
}

struct SessionReportVisibility {
    std::array<bool, sessionKpiCount> kpis{};

    SessionReportVisibility() noexcept { restoreDefaults(); }

    void selectAll() noexcept { kpis.fill(true); }
    void clearAll() noexcept { kpis.fill(false); }
    void restoreDefaults() noexcept {
        for (const auto& definition : sessionKpiDefinitions)
            set(definition.kpi, definition.visibleByDefault);
    }

    [[nodiscard]] bool visible(SessionKpi kpi) const noexcept {
        return kpis[sessionKpiIndex(kpi)];
    }
    void set(SessionKpi kpi, bool visible) noexcept {
        kpis[sessionKpiIndex(kpi)] = visible;
    }

    bool operator==(const SessionReportVisibility&) const noexcept = default;
};

[[nodiscard]] inline bool hasVisibleSessionKpi(
    const SessionReportVisibility& visibility,
    SessionKpiGroup group) noexcept {
    for (const auto& definition : sessionKpiDefinitions) {
        if (definition.group == group && visibility.visible(definition.kpi))
            return true;
    }
    return false;
}

template <typename Function>
inline void forEachVisibleSessionKpi(
    const SessionReportVisibility& visibility, SessionKpiGroup group,
    Function&& function) {
    for (const auto& definition : sessionKpiDefinitions) {
        if (definition.group == group && visibility.visible(definition.kpi))
            function(definition);
    }
}

} // namespace smp
