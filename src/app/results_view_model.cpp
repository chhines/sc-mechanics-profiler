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

constexpr std::array<MacroAccessStyle, 4> accessStyles{
    MacroAccessStyle::ControlGroupOnly,
    MacroAccessStyle::LocationHotkeyClick,
    MacroAccessStyle::ControlGroupCenterClick,
    MacroAccessStyle::Mixed,
};

constexpr std::array<ArmySelectionMethod, 7> selectionMethods{
    ArmySelectionMethod::DirectClick,
    ArmySelectionMethod::BoxSelect,
    ArmySelectionMethod::CtrlClickType,
    ArmySelectionMethod::DoubleClickType,
    ArmySelectionMethod::ShiftClickModify,
    ArmySelectionMethod::ShiftBoxModify,
    ArmySelectionMethod::CtrlShiftClickType,
};

constexpr const char* activeTimeTooltip =
    "Captured game time while StarCraft was active in the foreground. Time while StarCraft was not foreground, such as while alt-tabbed, is excluded.";
constexpr const char* transitionsPerMinuteTooltip =
    "Detected camera-navigation transitions divided by active game minutes. The total includes control-group jumps, location-hotkey jumps, minimap jumps, and qualifying edge-pan episodes.";
constexpr const char* controlGroupJumpsTooltip =
    "Detected camera jumps caused by double-tapping a unit control group. Re-centering the same control-group camera context is not counted as another navigation transition.";
constexpr const char* locationHotkeyJumpsTooltip =
    "Detected camera jumps caused by recalling a location hotkey. Recalling the location already treated as the current camera context is a recenter rather than another navigation transition.";
constexpr const char* minimapJumpsTooltip =
    "Left-clicks detected inside the calibrated minimap region and interpreted as minimap camera jumps.";
constexpr const char* edgePansTooltip =
    "Continuous qualifying edge-scroll episodes. One continuous period of edge scrolling counts as one pan rather than one event per mouse movement.";

constexpr const char* gamesAnalyzedTooltip =
    "Completed games in the current session for which this replay-based macro analysis was available. This can be lower than the total number of completed session games.";
constexpr const char* macroCyclesTooltip =
    "Detected bursts of production macro. A production visit is one occasion where you access a production building and make at least one detected production attempt. One macro cycle can contain multiple production visits when nearby visits are treated as one continuous macro pass.";
constexpr const char* averageMacroDurationTooltip =
    "Mean macro-cycle execution time. A cycle is timed from the beginning of access to the first production building through the first production attempt in the final production visit of that cycle.";
constexpr const char* bestMacroDurationTooltip =
    "Fastest detected macro-cycle execution time, measured from the beginning of access to the first production building through the first production attempt in the final production visit.";
constexpr const char* slowestMacroDurationTooltip =
    "Slowest detected macro-cycle execution time, measured from the beginning of access to the first production building through the first production attempt in the final production visit.";
constexpr const char* productionVisitsTooltip =
    "Number of detected production visits. One production visit is one occasion where you access a production building and make at least one detected production attempt. This is not the number of units produced. The profiler keeps an internal identity for which production building each visit belongs to, but that internal production-context identity is not a separate mechanical action or reported statistic.";

constexpr const char* assignmentsTooltip =
    "Qualifying army control-group assignments made with Ctrl+number. Production-building groups, scouting-unit groups, and uncertain edits are excluded from this headline count.";
constexpr const char* additionsTooltip =
    "Qualifying army control-group additions made with Shift+number. Production-building groups, scouting-unit groups, and uncertain edits are excluded from this headline count.";
constexpr const char* editsPerMinuteTooltip =
    "Qualifying army control-group assignments plus additions, divided by active game minutes.";

constexpr const char* detectedScoutingUnitsTooltip =
    "Detected scout-unit identities. An early singleton worker is confirmed as a scout when replay-attributed commands for that same unit tag move onto the opponent's side of the map relative to the actual occupied starting locations.";
constexpr const char* scoutingActivityTooltip =
    "Observed scouting span from the qualifying worker control-group assignment through the confirmed return-home command after its final enemy-side excursion. If the worker never returns home, the final replay-attributed command is used instead. This is not the unit's death time.";
constexpr const char* scoutingSelectionsCommandsTooltip =
    "Selections are recalls of the scout's original control group while that binding remains intact. Commands are replay-attributed right-click commands issued by the same scout unit tag during the scouting episode; command attribution does not depend on the control group remaining selected.";
constexpr const char* scoutingLastCommandedTooltip =
    "Active-game timestamp of the command that ends the observed scouting episode: a confirmed return-home command after the final enemy-side excursion, or otherwise the scout unit's final attributable command.";
constexpr const char* scoutingSelectionsTooltip =
    "Total recalls of the original control groups used for detected scout units while those bindings remained intact. These recalls are descriptive and do not control command attribution.";
constexpr const char* scoutingCommandsTooltip =
    "Total replay-attributed right-click commands issued by detected scout-unit tags through the end of their scouting episodes.";
constexpr const char* averageScoutingActivityTooltip =
    "Average observed scouting span from the qualifying worker assignment through the episode-ending return-home command, or otherwise through the final attributable command.";
constexpr const char* longestScoutingActivityTooltip =
    "Longest observed scouting span among detected scout units, ending at a confirmed return-home command or otherwise at the unit's final attributable command.";

const char* macroAccessStyleLabel(MacroAccessStyle style) noexcept {
    switch (style) {
    case MacroAccessStyle::ControlGroupOnly: return "Control Group Only";
    case MacroAccessStyle::LocationHotkeyClick: return "Location Hotkey Click";
    case MacroAccessStyle::ControlGroupCenterClick: return "Control Group Center Click";
    case MacroAccessStyle::Mixed: return "Mixed";
    case MacroAccessStyle::Other: return "Other";
    }
    return "Other";
}

const char* macroAccessStyleTooltip(MacroAccessStyle style) noexcept {
    switch (style) {
    case MacroAccessStyle::ControlGroupOnly:
        return "All production visits in this macro cycle were accessed directly through production control groups, without using a recognized camera-anchor-plus-click technique.";
    case MacroAccessStyle::LocationHotkeyClick:
        return "A location hotkey moved the camera to the production area, then production buildings were selected by direct click or box selection.";
    case MacroAccessStyle::ControlGroupCenterClick:
        return "A production control group was double-tapped to center the camera, then production buildings were selected from that view.";
    case MacroAccessStyle::Mixed:
        return "The macro cycle used more than one recognized production-access technique.";
    case MacroAccessStyle::Other:
        return "";
    }
    return "";
}

const char* selectionMethodLabel(ArmySelectionMethod method) noexcept {
    switch (method) {
    case ArmySelectionMethod::DirectClick: return "Direct Click";
    case ArmySelectionMethod::BoxSelect: return "Box Select";
    case ArmySelectionMethod::CtrlClickType: return "Ctrl-Click Type";
    case ArmySelectionMethod::DoubleClickType: return "Double-Click Type";
    case ArmySelectionMethod::ShiftClickModify: return "Shift-Click Modify";
    case ArmySelectionMethod::ShiftBoxModify: return "Shift-Box Modify";
    case ArmySelectionMethod::CtrlShiftClickType: return "Ctrl+Shift-Click Type";
    case ArmySelectionMethod::ExistingSelection: return "Existing Selection";
    case ArmySelectionMethod::Other: return "Other";
    }
    return "Other";
}

const char* selectionMethodTooltip(ArmySelectionMethod method) noexcept {
    switch (method) {
    case ArmySelectionMethod::DirectClick:
        return "A normal left-click formed the selection immediately before the control-group edit.";
    case ArmySelectionMethod::BoxSelect:
        return "A drag box formed the selection immediately before the control-group edit.";
    case ArmySelectionMethod::CtrlClickType:
        return "Ctrl-click selected units of the clicked unit type immediately before the control-group edit.";
    case ArmySelectionMethod::DoubleClickType:
        return "A double-click selected units of the clicked unit type immediately before the control-group edit.";
    case ArmySelectionMethod::ShiftClickModify:
        return "You already had units selected, then held Shift and clicked an individual unit to modify that selection before the control-group edit. Depending on selection state, this can add or remove that unit.";
    case ArmySelectionMethod::ShiftBoxModify:
        return "You already had units selected, then held Shift and drag-boxed units to modify or expand that selection before the control-group edit.";
    case ArmySelectionMethod::CtrlShiftClickType:
        return "Ctrl+Shift-click modified the existing selection by unit type immediately before the control-group edit.";
    case ArmySelectionMethod::ExistingSelection:
    case ArmySelectionMethod::Other:
        return "";
    }
    return "";
}

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

double reportedPercentage(std::uint64_t count, std::uint64_t reportedTotal) noexcept {
    return reportedTotal > 0
               ? static_cast<double>(count) * 100.0 /
                     static_cast<double>(reportedTotal)
               : 0.0;
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
    section.metrics.push_back({"Cycles", integer(macro["count"].asInt()),
                               macroCyclesTooltip});
    section.metrics.push_back({"Average duration", seconds(macro["average_duration_ms"]),
                               averageMacroDurationTooltip});
    section.metrics.push_back({"Best duration", seconds(macro["best_duration_ms"]),
                               bestMacroDurationTooltip});
    section.metrics.push_back({"Slowest duration", seconds(macro["slowest_duration_ms"]),
                               slowestMacroDurationTooltip});
    section.metrics.push_back({"Production visits", integer(macro["production_visit_count"].asInt()),
                               productionVisitsTooltip});
    model.sections.push_back(std::move(section));
}

void addGameAccessStyles(ResultsViewModel& model, const json::Value& macro,
                         std::string id, std::string title) {
    if (!macro["available"].asBool(false))
        return;
    ResultsSection section{std::move(id), std::move(title), {}};
    std::uint64_t reportedCycles = 0;
    for (const auto style : accessStyles) {
        const auto& stats = macro["macro_access_styles"][macroAccessStyleName(style)];
        reportedCycles += static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, stats["cycle_count"].asInt()));
    }
    for (const auto style : accessStyles) {
        const auto& stats = macro["macro_access_styles"][macroAccessStyleName(style)];
        const auto cycleCount = static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, stats["cycle_count"].asInt()));
        if (cycleCount == 0)
            continue;
        section.metrics.push_back(
            {macroAccessStyleLabel(style),
             fixed(reportedPercentage(cycleCount, reportedCycles), 1) +
                 "%  |  median " + seconds(stats["median_duration_ms"]),
             macroAccessStyleTooltip(style)});
    }
    if (!section.metrics.empty())
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
    totals.metrics.push_back({"Assignments", integer(army["assignments"].asInt()),
                              assignmentsTooltip});
    totals.metrics.push_back({"Additions", integer(army["additions"].asInt()),
                              additionsTooltip});
    totals.metrics.push_back({"Edits / minute", fixed(army["total_group_edits_per_minute"].asNumber(), 1),
                              editsPerMinuteTooltip});
    model.sections.push_back(std::move(totals));

    const auto addMethods = [&](const char* key, std::string id, std::string title) {
        ResultsSection methods{std::move(id), std::move(title), {}};
        std::uint64_t reportedEdits = 0;
        for (const auto method : selectionMethods) {
            reportedEdits += static_cast<std::uint64_t>(std::max<std::int64_t>(
                0, army[key][armySelectionMethodName(method)]["edit_count"].asInt()));
        }
        for (const auto method : selectionMethods) {
            const auto& stats = army[key][armySelectionMethodName(method)];
            const auto editCount = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, stats["edit_count"].asInt()));
            if (editCount == 0)
                continue;
            std::string value = fixed(reportedPercentage(editCount, reportedEdits), 1) + "%";
            if (stats["average_selection_to_operation_ms"].isNumber())
                value += "  |  avg " +
                         fixed(stats["average_selection_to_operation_ms"].asNumber(), 0) + " ms";
            methods.metrics.push_back({selectionMethodLabel(method), std::move(value),
                                       selectionMethodTooltip(method)});
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
    section.metrics.push_back({"Detected scouting units",
                               integer(static_cast<int>(activities.size())),
                               detectedScoutingUnitsTooltip});
    for (const auto& activity : activities) {
        const std::string prefix = "Group " + std::to_string(activity["group"].asInt()) +
                                   " generation " +
                                   std::to_string(activity["assignment_generation"].asInt());
        section.metrics.push_back(
            {prefix + " activity", seconds(activity["activity_duration_ms"], 1),
             scoutingActivityTooltip});
        section.metrics.push_back(
            {prefix + " selections / commands",
             integer(activity["selection_count"].asInt()) + " / " +
                 integer(activity["command_count"].asInt()),
             scoutingSelectionsCommandsTooltip});
        section.metrics.push_back(
            {prefix + " last commanded",
             activity["last_command_active_ms"].isNumber()
                 ? activeTime(activity["last_command_active_ms"].asNumber())
                 : "N/A",
             scoutingLastCommandedTooltip});
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
    section.metrics.push_back({"Games analyzed", integer(macro.gamesAnalyzed),
                               gamesAnalyzedTooltip});
    section.metrics.push_back({"Cycles", integer(macro.cycles), macroCyclesTooltip});
    section.metrics.push_back({"Average duration", optionalSeconds(macro.averageDurationMs()),
                               averageMacroDurationTooltip});
    section.metrics.push_back({"Best duration", optionalSeconds(macro.bestDurationMs),
                               bestMacroDurationTooltip});
    section.metrics.push_back({"Slowest duration", optionalSeconds(macro.slowestDurationMs),
                               slowestMacroDurationTooltip});
    section.metrics.push_back({"Production visits", integer(macro.productionVisits),
                               productionVisitsTooltip});
    model.sections.push_back(std::move(section));
}

void addSessionAccessStyles(ResultsViewModel& model, const ProductMacroSessionStats& macro,
                            std::string id, std::string title) {
    if (macro.gamesAnalyzed == 0)
        return;
    ResultsSection section{std::move(id), std::move(title), {}};
    std::uint64_t reportedCycles = 0;
    for (const auto style : accessStyles)
        reportedCycles += macro.accessStyleStatistics(style).cycleCount;
    for (const auto style : accessStyles) {
        const auto stats = macro.accessStyleStatistics(style);
        if (stats.cycleCount == 0)
            continue;
        section.metrics.push_back({
            macroAccessStyleLabel(style),
            fixed(reportedPercentage(stats.cycleCount, reportedCycles), 1) +
                "%  |  median " + optionalSeconds(stats.medianDurationMs),
            macroAccessStyleTooltip(style)});
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
    section.metrics.push_back({"Assignments", integer(army.assignments), assignmentsTooltip});
    section.metrics.push_back({"Additions", integer(army.additions), additionsTooltip});
    section.metrics.push_back({"Edits / minute", fixed(army.editsPerMinute(), 1),
                               editsPerMinuteTooltip});
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
    section.metrics.push_back({"Detected scouting units",
                               integer(army.scoutingUnitActivities.size()),
                               detectedScoutingUnitsTooltip});
    section.metrics.push_back({"Selections", integer(selections), scoutingSelectionsTooltip});
    section.metrics.push_back({"Commands", integer(commands), scoutingCommandsTooltip});
    if (!durations.empty()) {
        const double average = std::accumulate(durations.begin(), durations.end(), 0.0) /
                               static_cast<double>(durations.size());
        section.metrics.push_back({"Average activity duration", optionalSeconds(average),
                                   averageScoutingActivityTooltip});
        section.metrics.push_back({"Longest activity duration",
                                   optionalSeconds(*std::max_element(durations.begin(), durations.end())),
                                   longestScoutingActivityTooltip});
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
        section.metrics.push_back({"Active time",
                                   activeTime(summary["session"]["active_duration_seconds"].asNumber() * 1000.0),
                                   activeTimeTooltip});
        section.metrics.push_back({"Transitions / minute",
                                   fixed(navigation["transitions_per_minute"].asNumber(), 1),
                                   transitionsPerMinuteTooltip});
        const auto addMethod = [&](const char* label, double count, const char* tooltip) {
            const double percentage = total > 0.0 ? count * 100.0 / total : 0.0;
            section.metrics.push_back({
                label,
                integer(static_cast<std::int64_t>(count)) + "  |  " +
                    fixed(percentage, 1) + "%",
                tooltip});
        };
        addMethod("Control-group jumps",
                  navigation["control_group"]["transitions"].asNumber(),
                  controlGroupJumpsTooltip);
        addMethod("Location-hotkey jumps",
                  navigation["location_hotkey"]["transitions"].asNumber(),
                  locationHotkeyJumpsTooltip);
        addMethod("Minimap jumps", navigation["minimap"]["transitions"].asNumber(),
                  minimapJumpsTooltip);
        addMethod("Edge pans", navigation["edge_scroll"]["episodes"].asNumber(),
                  edgePansTooltip);
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
        section.metrics.push_back({"Active time", activeTime(stats.activeSeconds * 1000.0),
                                   activeTimeTooltip});
        section.metrics.push_back({"Transitions / minute",
                                   fixed(stats.navigationTransitionsPerMinute(), 1),
                                   transitionsPerMinuteTooltip});
        const auto addMethod = [&](const char* label, std::uint64_t count,
                                   const char* tooltip) {
            const double percentage = stats.methodPercentage(count);
            section.metrics.push_back({label,
                                       integer(count) + "  |  " + fixed(percentage, 1) + "%",
                                       tooltip});
        };
        addMethod("Control-group jumps", stats.controlGroupJumps,
                  controlGroupJumpsTooltip);
        addMethod("Location-hotkey jumps", stats.locationHotkeyJumps,
                  locationHotkeyJumpsTooltip);
        addMethod("Minimap jumps", stats.minimapJumps, minimapJumpsTooltip);
        addMethod("Edge pans", stats.edgePans, edgePansTooltip);
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
