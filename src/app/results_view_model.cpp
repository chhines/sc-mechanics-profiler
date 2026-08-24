#include "app/results_view_model.h"

#include "analysis/army_control_group.h"
#include "analysis/production_visit.h"
#include "app/analysis_ability_activity.h"
#include "app/analysis_macro_duration.h"
#include "app/analysis_macro_gap.h"
#include "app/analysis_multitasking.h"
#include "app/analysis_navigation_rate.h"
#include "app/analysis_scouting.h"
#include "app/session_trend_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
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

std::string optionalValue(const std::optional<double>& value,
                          int precision = 2,
                          const char* suffix = "") {
    return value ? fixed(*value, precision) + suffix : "N/A";
}

std::string activeTime(double milliseconds) {
    const auto totalSeconds = static_cast<long long>(std::llround(milliseconds / 1000.0));
    std::ostringstream output;
    output << totalSeconds / 60 << ':' << std::setw(2) << std::setfill('0')
           << totalSeconds % 60;
    return output.str();
}

std::string activeTimeSeconds(double seconds) {
    const auto wholeSeconds =
        static_cast<long long>(std::max(0.0, seconds));
    std::ostringstream output;
    output << wholeSeconds / 60 << ':' << std::setw(2)
           << std::setfill('0') << wholeSeconds % 60;
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
                  const std::vector<TimelineMacroCycle>& cycles,
                  double activeDurationMs, std::string id,
                  std::string title) {
    if (!macro["available"].asBool(false)) {
        model.sections.push_back(unavailable(std::move(id), std::move(title),
                                             macro["reason"].asString("Replay correlation unavailable")));
        return;
    }
    ResultsSection section{std::move(id), std::move(title), {}};
    section.metrics.push_back({"Cycles", integer(macro["count"].asInt()),
                               macroCyclesTooltip});
    const auto summary =
        analysis_insights::macroGapSummary(cycles, activeDurationMs);
    section.metrics.push_back(
        {"Cycles / minute", optionalValue(summary.cyclesPerMinute, 2)});
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

void addGameMacroGaps(ResultsViewModel& model,
                      const VisualizationTrackStatus& status,
                      const std::vector<TimelineMacroCycle>& cycles,
                      double activeDurationMs, std::string id,
                      std::string title) {
    if (!status.available) {
        model.sections.push_back(
            unavailable(std::move(id), std::move(title), status.reason));
        return;
    }
    const auto summary =
        analysis_insights::macroGapSummary(cycles, activeDurationMs);
    ResultsSection section{std::move(id), std::move(title), {}};
    section.metrics.push_back(
        {"Cycles / min", optionalValue(summary.cyclesPerMinute, 2)});
    section.metrics.push_back(
        {"Median gap", optionalValue(summary.medianSeconds, 2, " s")});
    section.metrics.push_back(
        {"P90 gap", optionalValue(summary.p90Seconds, 2, " s")});
    section.metrics.push_back(
        {"Longest gap", optionalValue(summary.longestSeconds, 2, " s")});
    const bool hasGaps = !summary.gaps.empty();
    section.metrics.push_back(
        {"Gaps >10 s",
         hasGaps ? integer(summary.overTenSeconds) : "N/A"});
    section.metrics.push_back(
        {"Gaps >20 s",
         hasGaps ? integer(summary.overTwentySeconds) : "N/A"});
    model.sections.push_back(std::move(section));
}

void addGameMacroDurationDistribution(
    ResultsViewModel& model, const VisualizationTrackStatus& status,
    const std::vector<TimelineMacroCycle>& cycles, std::string id,
    std::string title) {
    if (!status.available) {
        model.sections.push_back(
            unavailable(std::move(id), std::move(title), status.reason));
        return;
    }
    ResultsSection section{std::move(id), std::move(title), {}};
    const auto bins = analysis_insights::macroDurationBins(cycles);
    for (std::size_t index = 0; index < bins.size(); ++index) {
        section.metrics.push_back(
            {analysis_insights::macroDurationBucketLabels[index],
             integer(bins[index])});
    }
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
             integer(cycleCount) + " cycles  |  " +
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
    totals.metrics.push_back(
        {"Edits / minute",
         army["total_group_edits_per_minute"].isNumber()
             ? fixed(army["total_group_edits_per_minute"].asNumber(), 1)
             : "N/A",
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
            std::string value = integer(editCount) + " edits  |  " +
                                fixed(reportedPercentage(editCount,
                                                         reportedEdits),
                                      1) +
                                "%";
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

void addGameArmyCommands(ResultsViewModel& model,
                         const GameAnalysisVisualizationModel& game) {
    if (!game.armyCommandStatus.available) {
        model.sections.push_back(unavailable(
            "army_command_activity", "Army Command Activity",
            game.armyCommandStatus.reason));
        return;
    }
    ResultsSection section{"army_command_activity", "Army Command Activity",
                           {}};
    section.metrics.push_back(
        {"Commands / min", optionalValue(game.armyCommandsPerMinute, 1)});
    section.metrics.push_back(
        {"Median command gap",
         optionalSeconds(game.medianArmyCommandGapMs)});
    section.metrics.push_back(
        {"P90 command gap", optionalSeconds(game.p90ArmyCommandGapMs)});
    section.metrics.push_back(
        {"Longest command gap",
         optionalSeconds(game.longestArmyCommandGapMs)});
    model.sections.push_back(std::move(section));
}

void addGameAbilityActivity(ResultsViewModel& model,
                            const GameAnalysisVisualizationModel& game) {
    if (!game.abilityActivityStatus.available) {
        model.sections.push_back(unavailable(
            "ability_activity", "Ability Activity",
            game.abilityActivityStatus.reason));
        return;
    }
    const bool hasActivity =
        analysis_insights::hasAbilityActivityForDisplay(game.totalAbilityUses);
    ResultsSection section{"ability_activity", "Ability Activity", {}};
    section.metrics.push_back(
        {"Abilities / min",
         hasActivity ? optionalValue(game.abilitiesPerMinute, 1) : "N/A"});
    section.metrics.push_back(
        {"Total abilities",
         hasActivity ? std::to_string(*game.totalAbilityUses) : "N/A"});
    model.sections.push_back(std::move(section));

    if (game.abilityActivityBreakdown.empty())
        return;
    ResultsSection breakdown{"ability_activity_breakdown",
                             "Ability Activity Breakdown", {}};
    for (const auto& ability : game.abilityActivityBreakdown) {
        breakdown.metrics.push_back(
            {ability.ability,
             std::to_string(ability.uses) + " uses  |  " +
                 optionalValue(ability.usesPerMinute, 1, " / min")});
    }
    model.sections.push_back(std::move(breakdown));
}

void addGameNavigationBuckets(ResultsViewModel& model,
                              const GameAnalysisVisualizationModel& game) {
    if (!game.navigationStatus.available)
        return;
    const auto series = analysis_insights::navigationRates(game);
    if (series.buckets.empty())
        return;
    ResultsSection section{"navigation_transition_buckets",
                           "Navigation Transition Rate - 30-second Buckets",
                           {}};
    for (const auto& bucket : series.buckets) {
        section.metrics.push_back(
            {activeTimeSeconds(bucket.startSeconds) + "-" +
                 activeTimeSeconds(bucket.endSeconds),
             fixed(bucket.ratePerMinute, 1) + " / min"});
    }
    model.sections.push_back(std::move(section));
}

void addGameMultitasking(ResultsViewModel& model,
                         const GameAnalysisVisualizationModel& game) {
    if (game.activeDurationMs <= 0.0) {
        model.sections.push_back(unavailable(
            "multitasking", "Multitasking Heatmap",
            "No active game duration is available"));
        return;
    }
    const auto windows = analysis_insights::multitaskingWindows(game);
    ResultsSection summary{"multitasking", "Multitasking Heatmap", {}};
    summary.metrics.push_back(
        {"Average mechanic types / active 5-second window",
         optionalValue(windows.averageActiveDiversity(), 2)});
    summary.metrics.push_back(
        {"Peak mechanic types in one 5-second window",
         integer(windows.peakDiversity)});
    model.sections.push_back(std::move(summary));

    constexpr std::array<const char*, 5> labels{
        "Camera", "Worker macro", "Army macro", "CG edit", "Scout command"};
    ResultsSection detail{"command_heatmap_windows",
                          "Command Heatmap - 5-second Windows", {}};
    for (std::size_t column = 0; column < windows.diversity.size(); ++column) {
        const double startSeconds =
            static_cast<double>(column) * multitaskingWindowDurationMs / 1000.0;
        const double endSeconds = std::min(
            game.activeDurationMs / 1000.0,
            startSeconds + multitaskingWindowDurationMs / 1000.0);
        std::string value;
        for (std::size_t row = 0; row < windows.counts.size(); ++row) {
            if (!value.empty())
                value += "  |  ";
            value += labels[row];
            value += ' ';
            value += std::to_string(windows.counts[row][column]);
        }
        detail.metrics.push_back(
            {activeTimeSeconds(startSeconds) + "-" +
                 activeTimeSeconds(endSeconds),
             std::move(value)});
    }
    model.sections.push_back(std::move(detail));
}

void addGameScouting(ResultsViewModel& model,
                     const GameAnalysisVisualizationModel& game) {
    if (!game.scoutingStatus.available) {
        model.sections.push_back(unavailable(
            "scouting_activity", "Scouting Activity",
            game.scoutingStatus.reason));
        return;
    }
    ResultsSection section{"scouting_activity", "Scouting Activity", {}};
    section.metrics.push_back({"Confirmed scouts",
                               integer(game.scoutingActivities.size()),
                               detectedScoutingUnitsTooltip});
    section.metrics.push_back(
        {"Total scouting time",
         optionalSeconds(analysis_insights::totalScoutingActivityDurationMs(
             game.scoutingActivities))});
    std::optional<double> longestGapMs;
    for (const auto& activity : game.scoutingActivities) {
        if (activity.longestCommandGapMs &&
            (!longestGapMs || *activity.longestCommandGapMs > *longestGapMs)) {
            longestGapMs = activity.longestCommandGapMs;
        }
    }
    section.metrics.push_back(
        {"Longest scout command gap", optionalSeconds(longestGapMs)});
    const auto outcomes = analysis_insights::scoutingOutcomeCounts(
        game.scoutingOutcomeDataAvailable, game.scoutingActivities);
    if (outcomes) {
        section.metrics.push_back(
            {"Returned home", integer(outcomes->returnedHome)});
        section.metrics.push_back(
            {"No observed return", integer(outcomes->noObservedReturn),
             "No observed return is not a death inference."});
        section.metrics.push_back(
            {"Resumed after temporary return",
             integer(outcomes->resumedAfterTemporaryReturn),
             "Supplemental observation that may overlap the final Returned home or No observed return outcome."});
        section.metrics.push_back(
            {"Outcome interpretation",
             "Resumed after temporary return may overlap the final outcome. "
             "No observed return is not a death inference."});
    } else {
        section.metrics.push_back(
            {"Observed scouting outcomes", "N/A",
             "Outcome details require a game analyzed with current scouting telemetry."});
    }
    for (const auto& activity : game.scoutingActivities) {
        const std::string prefix = "Group " + std::to_string(activity.group) +
                                   " generation " +
                                   std::to_string(activity.assignmentGeneration);
        section.metrics.push_back(
            {prefix + " activity",
             optionalSeconds(activity.activityDurationMs, 1),
             scoutingActivityTooltip});
        section.metrics.push_back(
            {prefix + " selections / commands",
             integer(activity.selectionCount) + " / " +
                 integer(activity.commandCount),
             scoutingSelectionsCommandsTooltip});
        section.metrics.push_back(
            {prefix + " last commanded",
             activity.lastCommandActiveMs
                 ? activeTime(*activity.lastCommandActiveMs)
                 : "N/A",
             scoutingLastCommandedTooltip});
    }
    model.sections.push_back(std::move(section));
}

void addSessionTrendGroup(ResultsViewModel& model,
                          const SessionTrendStats& stats,
                          const SessionReportVisibility& visibility,
                          SessionTrendGroup group, std::string id,
                          std::string title) {
    ResultsSection section{std::move(id), std::move(title), {}};
    for (const auto metric : trendMetrics) {
        if (trendMetricGroup(metric) != group ||
            !trendMetricVisible(visibility, metric)) {
            continue;
        }
        section.metrics.push_back(
            {metricTitle(metric),
             optionalValue(metricValue(stats, metric), 2,
                           metricUsesSeconds(metric) ? " s" : "")});
    }
    if (group == SessionTrendGroup::Macro &&
        visibility.macroCadenceGaps) {
        for (const auto metric : workerArmyTrendMetrics) {
            for (const bool worker : {true, false}) {
                section.metrics.push_back(
                    {workerArmyTrendMetricTitle(worker, metric),
                     optionalValue(
                         workerArmyTrendValue(stats, worker, metric), 2,
                         workerArmyTrendUsesSeconds(metric) ? " s" : "")});
            }
        }
    }
    if (!section.metrics.empty())
        model.sections.push_back(std::move(section));
}

} // namespace

bool ResultsViewModel::hasSection(const std::string& id) const noexcept {
    return std::any_of(sections.begin(), sections.end(),
                       [&](const auto& section) { return section.id == id; });
}

ResultsViewModel deriveGameResults(const json::Value& summary,
                                   const ReportGroupVisibility& visibility,
                                   const GameAnalysisVisualizationModel*
                                       visualization) {
    GameAnalysisVisualizationModel derivedVisualization;
    if (!visualization) {
        derivedVisualization =
            buildGameAnalysisVisualizationModel(nullptr, &summary);
        visualization = &derivedVisualization;
    }
    const auto& game = *visualization;
    ResultsViewModel model{"Latest Game", summary["session"]["id"].asString(), {}};
    if (visibility.navigationTransitionRate || visibility.cameraNavigation) {
        const auto& navigation = summary["camera_navigation"];
        const double total = navigation["total_transitions"].asNumber();
        ResultsSection section{"camera_navigation", "Camera Navigation", {}};
        if (visibility.navigationTransitionRate) {
            section.metrics.push_back({
                "Active time",
                activeTime(summary["session"]["active_duration_seconds"]
                               .asNumber() *
                           1000.0),
                activeTimeTooltip});
            section.metrics.push_back({
                "Transitions / minute",
                fixed(navigation["transitions_per_minute"].asNumber(), 1),
                transitionsPerMinuteTooltip});
        }
        if (visibility.cameraNavigation) {
            const auto addMethod = [&](const char* label, double count,
                                       const char* tooltip) {
                const double percentage =
                    total > 0.0 ? count * 100.0 / total : 0.0;
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
            addMethod("Minimap jumps",
                      navigation["minimap"]["transitions"].asNumber(),
                      minimapJumpsTooltip);
            addMethod("Edge pans",
                      navigation["edge_scroll"]["episodes"].asNumber(),
                      edgePansTooltip);
        }
        model.sections.push_back(std::move(section));
    }
    if (visibility.navigationTransitionRate)
        addGameNavigationBuckets(model, game);
    if (visibility.workerMacroCycles)
        addGameMacro(model, summary["worker_macro_cycles"],
                     game.workerMacroCycles, game.activeDurationMs,
                     "worker_macro", "Worker Macro Cycles");
    if (visibility.armyMacroCycles)
        addGameMacro(model, summary["army_macro_cycles"],
                     game.armyMacroCycles, game.activeDurationMs,
                     "army_macro", "Army Macro Cycles");
    if (visibility.macroGaps) {
        addGameMacroGaps(model, game.workerMacroStatus,
                         game.workerMacroCycles, game.activeDurationMs,
                         "worker_macro_gaps", "Worker Macro Gaps");
        addGameMacroGaps(model, game.armyMacroStatus,
                         game.armyMacroCycles, game.activeDurationMs,
                         "army_macro_gaps", "Army Macro Gaps");
    }
    if (visibility.macroDurationDistribution) {
        addGameMacroDurationDistribution(
            model, game.workerMacroStatus, game.workerMacroCycles,
            "worker_macro_duration_distribution",
            "Worker Macro Duration Distribution");
        addGameMacroDurationDistribution(
            model, game.armyMacroStatus, game.armyMacroCycles,
            "army_macro_duration_distribution",
            "Army Macro Duration Distribution");
    }
    if (visibility.macroAccessStyles) {
        addGameAccessStyles(model, summary["worker_macro_cycles"], "worker_access_styles", "Worker Macro Access Styles");
        addGameAccessStyles(model, summary["army_macro_cycles"], "army_access_styles", "Army Macro Access Styles");
    }
    if (visibility.armyControlGroupManagement)
        addGameArmyControlGroups(model, summary["army_control_group_management"]);
    if (visibility.armyCommandActivity)
        addGameArmyCommands(model, game);
    if (visibility.abilityActivity)
        addGameAbilityActivity(model, game);
    if (visibility.multitaskingDensity)
        addGameMultitasking(model, game);
    if (visibility.scoutingUnitActivity)
        addGameScouting(model, game);
    return model;
}

ResultsViewModel deriveSessionResults(const AutomaticSessionStats& stats,
                                      const SessionReportVisibility& visibility) {
    ResultsViewModel model{"Current Session", integer(stats.games) + " completed game(s)", {}};
    const auto trendStats = sessionTrendStats(stats);
    if (visibility.hasMacroSections())
        addSessionTrendGroup(model, trendStats, visibility,
                             SessionTrendGroup::Macro, "session_macro",
                             "Macro");
    if (visibility.hasArmyManagementSections())
        addSessionTrendGroup(
            model, trendStats, visibility,
            SessionTrendGroup::ArmyManagement, "session_army_management",
            "Army Management");
    if (visibility.hasMultitaskingSections())
        addSessionTrendGroup(model, trendStats, visibility,
                             SessionTrendGroup::Multitasking,
                             "session_multitasking", "Multitasking");
    return model;
}

} // namespace smp
