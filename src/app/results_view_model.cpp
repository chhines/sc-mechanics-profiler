#include "app/results_view_model.h"

#include "analysis/army_control_group.h"
#include "analysis/production_visit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace smp {
namespace {

constexpr std::array<MacroAccessStyle, macroAccessStyleCount> accessStyles{
    MacroAccessStyle::ControlGroupOnly,
    MacroAccessStyle::LocationHotkeyClick,
    MacroAccessStyle::ControlGroupCenterClick,
    MacroAccessStyle::Mixed,
    MacroAccessStyle::Other,
};

constexpr std::array<ArmySelectionMethod, armySelectionMethodCount> selectionMethods{
    ArmySelectionMethod::DirectClick,
    ArmySelectionMethod::BoxSelect,
    ArmySelectionMethod::CtrlClickType,
    ArmySelectionMethod::DoubleClickType,
    ArmySelectionMethod::ShiftClickModify,
    ArmySelectionMethod::ShiftBoxModify,
    ArmySelectionMethod::CtrlShiftClickType,
    ArmySelectionMethod::ExistingSelection,
    ArmySelectionMethod::Other,
};

std::string fixed(double value, int precision = 1) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(precision) << value;
    return output.str();
}

std::string integer(std::int64_t value) {
    return std::to_string(value);
}

std::string seconds(const json::Value& milliseconds, int precision = 2) {
    return milliseconds.isNumber() ? fixed(milliseconds.asNumber() / 1000.0, precision) + " s"
                                   : "N/A";
}

std::string optionalSeconds(const std::optional<double>& milliseconds,
                            int precision = 2) {
    return milliseconds ? fixed(*milliseconds / 1000.0, precision) + " s" : "N/A";
}

std::string activeTime(double milliseconds) {
    const auto totalSeconds = static_cast<long long>(std::llround(milliseconds / 1000.0));
    std::ostringstream output;
    output << totalSeconds / 60 << ':' << std::setw(2) << std::setfill('0')
           << totalSeconds % 60;
    return output.str();
}

ResultsSection unavailable(std::string id, std::string title,
                           const std::string& reason) {
    return {std::move(id), std::move(title), {{"Status", "Unavailable: " + reason}}};
}

void addGameMacro(ResultsViewModel& model, const json::Value& macro,
                  std::string id, std::string title) {
    if (!macro["available"].asBool(false)) {
        model.sections.push_back(unavailable(std::move(id), std::move(title),
                                             macro["reason"].asString("Replay correlation unavailable")));
        return;
    }
    ResultsSection section{std::move(id), std::move(title), {}};
    section.metrics.push_back({"Cycles", integer(macro["count"].asInt())});
    section.metrics.push_back({"Average duration", seconds(macro["average_duration_ms"])});
    section.metrics.push_back({"Best duration", seconds(macro["best_duration_ms"])});
    section.metrics.push_back({"Slowest duration", seconds(macro["slowest_duration_ms"])});
    section.metrics.push_back({"Production visits", integer(macro["production_visit_count"].asInt())});
    model.sections.push_back(std::move(section));
}

void addGameAccessStyles(ResultsViewModel& model, const json::Value& macro,
                         std::string id, std::string title) {
    if (!macro["available"].asBool(false))
        return;
    ResultsSection section{std::move(id), std::move(title), {}};
    for (const auto style : accessStyles) {
        const auto& stats = macro["macro_access_styles"][macroAccessStyleName(style)];
        const double percentage = stats["percentage"].asNumber();
        if (stats["cycle_count"].asInt() == 0)
            continue;
        section.metrics.push_back(
            {macroAccessStyleName(style),
             fixed(percentage, 1) + "%  |  median " +
                 seconds(stats["median_duration_ms"])});
    }
    if (section.metrics.empty())
        section.metrics.push_back({"Cycles", "No classified cycles"});
    model.sections.push_back(std::move(section));
}

void addGameArmyControlGroups(ResultsViewModel& model, const json::Value& army) {
    if (!army["available"].asBool(false)) {
        model.sections.push_back(unavailable(
            "army_control_groups", "Army Control-Group Management",
            army["reason"].asString("Replay correlation unavailable")));
        return;
    }
    ResultsSection totals{"army_control_groups", "Army Control-Group Management", {}};
    totals.metrics.push_back({"Assignments", integer(army["assignments"].asInt())});
    totals.metrics.push_back({"Additions", integer(army["additions"].asInt())});
    totals.metrics.push_back({"Edits / minute", fixed(army["total_group_edits_per_minute"].asNumber(), 1)});
    totals.metrics.push_back({"Scouting edits excluded",
                              integer(army["excluded_scouting_unit_edits"].asInt())});
    totals.metrics.push_back({"Production groups excluded",
                              integer(army["excluded_production_building_edits"].asInt())});
    model.sections.push_back(std::move(totals));

    const auto addMethods = [&](const char* key, std::string id, std::string title) {
        ResultsSection methods{std::move(id), std::move(title), {}};
        for (const auto method : selectionMethods) {
            const auto& stats = army[key][armySelectionMethodName(method)];
            if (stats["edit_count"].asInt() == 0)
                continue;
            const double percentage = stats["percentage"].asNumber();
            std::string value = fixed(percentage, 1) + "%";
            if (stats["average_selection_to_operation_ms"].isNumber())
                value += "  |  avg " +
                         fixed(stats["average_selection_to_operation_ms"].asNumber(), 0) + " ms";
            methods.metrics.push_back({armySelectionMethodLabel(method), std::move(value)});
        }
        if (!methods.metrics.empty())
            model.sections.push_back(std::move(methods));
    };
    addMethods("assignment_methods", "army_assignment_methods", "Assignment Selection Methods");
    addMethods("addition_methods", "army_addition_methods", "Addition Selection Methods");
}

void addGameScouting(ResultsViewModel& model, const json::Value& army) {
    const auto& activities = army["scouting_unit_activity"].asArray();
    if (activities.empty())
        return;
    ResultsSection section{"scouting_activity", "Scouting Unit Activity", {}};
    section.metrics.push_back({"Detected scouting groups", integer(static_cast<int>(activities.size()))});
    for (const auto& activity : activities) {
        const std::string prefix = "Group " + std::to_string(activity["group"].asInt()) +
                                   " generation " +
                                   std::to_string(activity["assignment_generation"].asInt());
        section.metrics.push_back(
            {prefix + " activity", seconds(activity["activity_duration_ms"], 1)});
        section.metrics.push_back(
            {prefix + " selections / commands",
             integer(activity["selection_count"].asInt()) + " / " +
             integer(activity["command_count"].asInt())});
        section.metrics.push_back(
            {prefix + " last commanded",
             activity["last_command_active_ms"].isNumber()
                 ? activeTime(activity["last_command_active_ms"].asNumber())
                 : "N/A"});
    }
    model.sections.push_back(std::move(section));
}

void addSessionMacro(ResultsViewModel& model, const ProductMacroSessionStats& macro,
                     std::string id, std::string title) {
    if (macro.gamesAnalyzed == 0) {
        model.sections.push_back(unavailable(std::move(id), std::move(title),
                                             "No replay-analyzed games in this session"));
        return;
    }
    ResultsSection section{std::move(id), std::move(title), {}};
    section.metrics.push_back({"Games analyzed", integer(macro.gamesAnalyzed)});
    section.metrics.push_back({"Cycles", integer(macro.cycles)});
    section.metrics.push_back({"Average duration", optionalSeconds(macro.averageDurationMs())});
    section.metrics.push_back({"Best duration", optionalSeconds(macro.bestDurationMs)});
    section.metrics.push_back({"Slowest duration", optionalSeconds(macro.slowestDurationMs)});
    section.metrics.push_back({"Production visits", integer(macro.productionVisits)});
    model.sections.push_back(std::move(section));
}

void addSessionAccessStyles(ResultsViewModel& model, const ProductMacroSessionStats& macro,
                            std::string id, std::string title) {
    if (macro.gamesAnalyzed == 0)
        return;
    ResultsSection section{std::move(id), std::move(title), {}};
    for (const auto style : accessStyles) {
        const auto stats = macro.accessStyleStatistics(style);
        if (stats.cycleCount == 0)
            continue;
        const double percentage = macro.accessStylePercentage(style);
        section.metrics.push_back({macroAccessStyleName(style),
                                   fixed(percentage, 1) + "%  |  median " +
                                   optionalSeconds(stats.medianDurationMs)});
    }
    if (!section.metrics.empty())
        model.sections.push_back(std::move(section));
}

void addSessionArmyControlGroups(ResultsViewModel& model,
                                 const ArmyControlGroupAnalysis& army) {
    if (!army.available) {
        model.sections.push_back(unavailable("army_control_groups",
                                             "Army Control-Group Management",
                                             army.unavailableReason));
        return;
    }
    ResultsSection section{"army_control_groups", "Army Control-Group Management", {}};
    section.metrics.push_back({"Assignments", integer(army.assignments)});
    section.metrics.push_back({"Additions", integer(army.additions)});
    section.metrics.push_back({"Edits / minute", fixed(army.editsPerMinute(), 1)});
    model.sections.push_back(std::move(section));
}

void addSessionScouting(ResultsViewModel& model,
                        const ArmyControlGroupAnalysis& army) {
    if (army.scoutingUnitActivities.empty())
        return;
    ResultsSection section{"scouting_activity", "Scouting Unit Activity", {}};
    std::size_t selections = 0;
    std::size_t commands = 0;
    std::vector<double> durations;
    for (const auto& activity : army.scoutingUnitActivities) {
        selections += activity.selectionCount;
        commands += activity.commandCount;
        if (activity.scoutingActivityDurationMs)
            durations.push_back(*activity.scoutingActivityDurationMs);
    }
    section.metrics.push_back({"Detected scouting groups", integer(army.scoutingUnitActivities.size())});
    section.metrics.push_back({"Selections", integer(selections)});
    section.metrics.push_back({"Commands", integer(commands)});
    if (!durations.empty()) {
        const double average = std::accumulate(durations.begin(), durations.end(), 0.0) /
                               static_cast<double>(durations.size());
        section.metrics.push_back({"Average activity duration", optionalSeconds(average)});
        section.metrics.push_back({"Longest activity duration",
                                   optionalSeconds(*std::max_element(durations.begin(), durations.end()))});
    }
    model.sections.push_back(std::move(section));
}

} // namespace

bool ResultsViewModel::hasSection(const std::string& id) const noexcept {
    return std::any_of(sections.begin(), sections.end(),
                       [&](const auto& section) { return section.id == id; });
}

ResultsViewModel deriveGameResults(const json::Value& summary,
                                   const ReportGroupVisibility& visibility) {
    ResultsViewModel model{"Latest Game", summary["session"]["id"].asString(), {}};
    if (visibility.cameraNavigation) {
        const auto& navigation = summary["camera_navigation"];
        const double total = navigation["total_transitions"].asNumber();
        ResultsSection section{"camera_navigation", "Camera Navigation", {}};
        section.metrics.push_back({"Active time", activeTime(summary["session"]["active_duration_seconds"].asNumber() * 1000.0)});
        section.metrics.push_back({"Transitions / minute", fixed(navigation["transitions_per_minute"].asNumber(), 1)});
        const auto addMethod = [&](const char* label, double count) {
            const double percentage = total > 0.0 ? count * 100.0 / total : 0.0;
            section.metrics.push_back({label, integer(static_cast<std::int64_t>(count)) + "  |  " + fixed(percentage, 1) + "%"});
        };
        addMethod("Control-group jumps", navigation["control_group"]["transitions"].asNumber());
        addMethod("Location-hotkey jumps", navigation["location_hotkey"]["transitions"].asNumber());
        addMethod("Minimap jumps", navigation["minimap"]["transitions"].asNumber());
        addMethod("Edge pans", navigation["edge_scroll"]["episodes"].asNumber());
        model.sections.push_back(std::move(section));
    }
    if (visibility.workerMacroCycles)
        addGameMacro(model, summary["worker_macro_cycles"], "worker_macro", "Worker Macro Cycles");
    if (visibility.armyMacroCycles)
        addGameMacro(model, summary["army_macro_cycles"], "army_macro", "Army Macro Cycles");
    if (visibility.macroAccessStyles) {
        addGameAccessStyles(model, summary["worker_macro_cycles"], "worker_access_styles", "Worker Macro Access Styles");
        addGameAccessStyles(model, summary["army_macro_cycles"], "army_access_styles", "Army Macro Access Styles");
    }
    if (visibility.armyControlGroupManagement)
        addGameArmyControlGroups(model, summary["army_control_group_management"]);
    if (visibility.scoutingUnitActivity)
        addGameScouting(model, summary["army_control_group_management"]);
    return model;
}

ResultsViewModel deriveSessionResults(const AutomaticSessionStats& stats,
                                      const ReportGroupVisibility& visibility) {
    ResultsViewModel model{"Current Session", integer(stats.games) + " completed game(s)", {}};
    if (visibility.cameraNavigation) {
        ResultsSection section{"camera_navigation", "Camera Navigation", {}};
        section.metrics.push_back({"Active time", activeTime(stats.activeSeconds * 1000.0)});
        section.metrics.push_back({"Transitions / minute", fixed(stats.navigationTransitionsPerMinute(), 1)});
        const auto addMethod = [&](const char* label, std::uint64_t count) {
            const double percentage = stats.methodPercentage(count);
            section.metrics.push_back({label, integer(count) + "  |  " + fixed(percentage, 1) + "%"});
        };
        addMethod("Control-group jumps", stats.controlGroupJumps);
        addMethod("Location-hotkey jumps", stats.locationHotkeyJumps);
        addMethod("Minimap jumps", stats.minimapJumps);
        addMethod("Edge pans", stats.edgePans);
        model.sections.push_back(std::move(section));
    }
    if (visibility.workerMacroCycles)
        addSessionMacro(model, stats.workerMacro, "worker_macro", "Worker Macro Cycles");
    if (visibility.armyMacroCycles)
        addSessionMacro(model, stats.armyMacro, "army_macro", "Army Macro Cycles");
    if (visibility.macroAccessStyles) {
        addSessionAccessStyles(model, stats.workerMacro, "worker_access_styles", "Worker Macro Access Styles");
        addSessionAccessStyles(model, stats.armyMacro, "army_access_styles", "Army Macro Access Styles");
    }
    if (visibility.armyControlGroupManagement)
        addSessionArmyControlGroups(model, stats.armyControlGroups);
    if (visibility.scoutingUnitActivity)
        addSessionScouting(model, stats.armyControlGroups);
    return model;
}

} // namespace smp
