#include "cli/report.h"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>

namespace smp {
namespace {

std::optional<double> number(const json::Value& value) {
    return value.isNumber() ? std::optional<double>(value.asNumber()) : std::nullopt;
}

std::string format(const std::optional<double>& value, const char* suffix = "", int precision = 0) {
    if (!value)
        return "N/A";
    std::ostringstream output;
    output << std::fixed << std::setprecision(precision) << *value << suffix;
    return output.str();
}

void writeRow(std::ostream& output, const std::string& label, const std::string& value) {
    output << std::left << std::setw(40) << label << std::right << std::setw(12) << value << '\n';
}

void row(const std::string& label, const std::string& value) {
    writeRow(std::cout, label, value);
}

std::string duration(double seconds) {
    const auto total = static_cast<long long>(std::llround(seconds));
    std::ostringstream output;
    output << total / 60 << ':' << std::setw(2) << std::setfill('0') << total % 60;
    return output.str();
}

std::string macroDuration(const std::optional<double>& milliseconds) {
    return milliseconds ? format(*milliseconds / 1000.0, " s", 2) : "N/A";
}

double percentage(int count, int total) {
    return total > 0 ? static_cast<double>(count) * 100.0 / static_cast<double>(total) : 0.0;
}

std::optional<double> baselineMean(const std::vector<json::Value>& baselines, const char* section,
                                   const char* metric) {
    double total = 0.0;
    std::size_t count = 0;
    for (const auto& baseline : baselines) {
        const auto& navigation = baseline["camera_navigation"];
        const auto value = number(section[0] == '\0' ? navigation[metric] : navigation[section][metric]);
        if (value) {
            total += *value;
            ++count;
        }
    }
    return count > 0 ? std::optional<double>(total / static_cast<double>(count)) : std::nullopt;
}

std::optional<double> pooledBaselineTransitionsPerMinute(const std::vector<json::Value>& baselines) {
    double totalTransitions = 0.0;
    double totalActiveSeconds = 0.0;
    std::size_t count = 0;
    for (const auto& baseline : baselines) {
        const auto transitions = number(baseline["camera_navigation"]["total_transitions"]);
        const auto activeSeconds = number(baseline["session"]["active_duration_seconds"]);
        if (!transitions || !activeSeconds)
            continue;
        totalTransitions += *transitions;
        totalActiveSeconds += *activeSeconds;
        ++count;
    }
    if (count == 0)
        return std::nullopt;
    return totalActiveSeconds > 0.0 ? totalTransitions / (totalActiveSeconds / 60.0) : 0.0;
}

void comparisonRow(const char* label, std::optional<double> current, std::optional<double> baseline,
                   int precision = 1) {
    std::cout << std::left << std::setw(30) << label << std::right << std::setw(14)
              << format(current, "", precision) << std::setw(14) << format(baseline, "", precision) << '\n';
}

void writeNavigationCounts(std::ostream& output, const AutomaticSessionStats& stats) {
    writeRow(output, "Control-group jumps", std::to_string(stats.controlGroupJumps));
    writeRow(output, "Location-hotkey jumps", std::to_string(stats.locationHotkeyJumps));
    writeRow(output, "Minimap jumps", std::to_string(stats.minimapJumps));
    writeRow(output, "Edge pans", std::to_string(stats.edgePans));
    writeRow(output, "Total", std::to_string(stats.navigationTransitions()));
}

void printNavigationCounts(const AutomaticSessionStats& stats) {
    writeNavigationCounts(std::cout, stats);
}

void writeMethodDistribution(std::ostream& output, const AutomaticSessionStats& stats) {
    writeRow(output, "Control-group jump",
             format(stats.methodPercentage(stats.controlGroupJumps), "%", 1));
    writeRow(output, "Location hotkey",
             format(stats.methodPercentage(stats.locationHotkeyJumps), "%", 1));
    writeRow(output, "Minimap", format(stats.methodPercentage(stats.minimapJumps), "%", 1));
    writeRow(output, "Edge pan", format(stats.methodPercentage(stats.edgePans), "%", 1));
}

void printMethodDistribution(const AutomaticSessionStats& stats) {
    writeMethodDistribution(std::cout, stats);
}

const char* productMacroHeading(MacroProductType type) noexcept {
    return type == MacroProductType::Worker ? "WORKER MACRO" : "ARMY MACRO";
}

constexpr std::array<MacroAccessStyle, macroAccessStyleCount> macroAccessStyles{
    MacroAccessStyle::ControlGroupOnly,
    MacroAccessStyle::LocationHotkeyClick,
    MacroAccessStyle::ControlGroupCenterClick,
    MacroAccessStyle::Mixed,
    MacroAccessStyle::Other,
};

const char* macroAccessStyleLabel(MacroAccessStyle style) noexcept {
    switch (style) {
    case MacroAccessStyle::ControlGroupOnly:
        return "Control-group only";
    case MacroAccessStyle::LocationHotkeyClick:
        return "Location hotkey + click";
    case MacroAccessStyle::ControlGroupCenterClick:
        return "CG center + click";
    case MacroAccessStyle::Mixed:
        return "Mixed";
    case MacroAccessStyle::Other:
        return "Other";
    }
    return "Other";
}

void writeAccessStyleSpeed(std::ostream& output, MacroAccessStyle style,
                           const MacroAccessStyleStatistics& statistics) {
    if (statistics.cycleCount == 0)
        return;
    output << '\n' << macroAccessStyleLabel(style) << '\n';
    writeRow(output, "  Cycles", std::to_string(statistics.cycleCount));
    writeRow(output, "  Average", macroDuration(statistics.averageDurationMs));
    writeRow(output, "  Median", macroDuration(statistics.medianDurationMs));
    writeRow(output, "  Best", macroDuration(statistics.bestDurationMs));
    writeRow(output, "  P25", macroDuration(statistics.p25DurationMs));
    writeRow(output, "  P75", macroDuration(statistics.p75DurationMs));
    writeRow(output, "  P90", macroDuration(statistics.p90DurationMs));
}

void printAccessStyles(const ProductMacroCycleAnalysis& analysis) {
    std::cout << "\nMACRO ACCESS STYLES\n\n";
    for (const auto style : macroAccessStyles)
        row(macroAccessStyleLabel(style),
            format(macroAccessStylePercentage(analysis, style), "%", 1));
    std::cout << "\nMACRO SPEED BY ACCESS STYLE\n";
    for (const auto style : macroAccessStyles)
        writeAccessStyleSpeed(
            std::cout, style,
            analysis.accessStyleStatistics[macroAccessStyleIndex(style)]);
}

void printAccessStyles(const json::Value& analysis) {
    const auto& styles = analysis["macro_access_styles"];
    if (!styles.isObject())
        return;
    std::cout << "\nMACRO ACCESS STYLES\n\n";
    for (const auto style : macroAccessStyles) {
        const auto& statistics = styles[macroAccessStyleName(style)];
        row(macroAccessStyleLabel(style),
            format(number(statistics["percentage"]), "%", 1));
    }
    std::cout << "\nMACRO SPEED BY ACCESS STYLE\n";
    for (const auto style : macroAccessStyles) {
        const auto& encoded = styles[macroAccessStyleName(style)];
        MacroAccessStyleStatistics statistics;
        statistics.cycleCount =
            static_cast<std::size_t>(encoded["cycle_count"].asInt());
        statistics.averageDurationMs = number(encoded["average_duration_ms"]);
        statistics.medianDurationMs = number(encoded["median_duration_ms"]);
        statistics.bestDurationMs = number(encoded["best_duration_ms"]);
        statistics.p25DurationMs = number(encoded["p25_duration_ms"]);
        statistics.p75DurationMs = number(encoded["p75_duration_ms"]);
        statistics.p90DurationMs = number(encoded["p90_duration_ms"]);
        writeAccessStyleSpeed(std::cout, style, statistics);
    }
}

void writeSessionAccessStyles(std::ostream& output,
                              const ProductMacroSessionStats& stats) {
    output << "\nMACRO ACCESS STYLES\n\n";
    for (const auto style : macroAccessStyles)
        writeRow(output, macroAccessStyleLabel(style),
                 format(stats.accessStylePercentage(style), "%", 1));
    output << "\nMACRO SPEED BY ACCESS STYLE\n";
    for (const auto style : macroAccessStyles)
        writeAccessStyleSpeed(output, style, stats.accessStyleStatistics(style));
}

void printAccessMethod(const std::array<std::size_t, 4>& counts, std::size_t total) {
    std::cout << "\nACCESS METHOD\n\n";
    row("Control group", format(percentage(static_cast<int>(counts[0]), static_cast<int>(total)), "%", 1));
    row("Location + click", format(percentage(static_cast<int>(counts[1]), static_cast<int>(total)), "%", 1));
    row("Minimap + click", format(percentage(static_cast<int>(counts[2]), static_cast<int>(total)), "%", 1));
    row("Screen click", format(percentage(static_cast<int>(counts[3]), static_cast<int>(total)), "%", 1));
}

void printProductMacro(const ProductMacroCycleAnalysis& analysis) {
    std::cout << '\n' << productMacroHeading(analysis.productType) << "\n\n";
    if (!analysis.available) {
        std::cout << "Unavailable: " << analysis.unavailableReason << '\n';
        return;
    }
    row("Cycles", std::to_string(analysis.cycles.size()));
    row("Average", macroDuration(analysis.averageDurationMs));
    row("Best", macroDuration(analysis.bestDurationMs));
    row("Slowest", macroDuration(analysis.slowestDurationMs));
    row("Production visits", std::to_string(analysis.productionVisitCount));
    printAccessMethod(analysis.accessMethodCounts, analysis.productionVisitCount);
    printAccessStyles(analysis);
}

void printProductMacro(const json::Value& analysis, const json::Value& visits,
                       MacroProductType productType) {
    std::cout << '\n' << productMacroHeading(productType) << "\n\n";
    if (!analysis["available"].asBool(false)) {
        std::cout << "Unavailable: "
                  << analysis["reason"].asString("Replay correlation failed") << '\n';
        return;
    }
    row("Cycles", std::to_string(analysis["count"].asInt()));
    row("Average", macroDuration(number(analysis["average_duration_ms"])));
    row("Best", macroDuration(number(analysis["best_duration_ms"])));
    row("Slowest", macroDuration(number(analysis["slowest_duration_ms"])));
    row("Production visits", std::to_string(analysis["production_visit_count"].asInt()));
    std::array<std::size_t, 4> accessCounts{};
    const std::string productName = macroProductTypeName(productType);
    for (const auto& visit : visits["visits"].asArray()) {
        if (visit["product_type"].asString() != productName)
            continue;
        const auto method = visit["access_method"].asString();
        if (method == "control_group")
            ++accessCounts[0];
        else if (method == "location_hotkey_click")
            ++accessCounts[1];
        else if (method == "minimap_click")
            ++accessCounts[2];
        else if (method == "screen_click")
            ++accessCounts[3];
    }
    printAccessMethod(accessCounts, static_cast<std::size_t>(analysis["production_visit_count"].asInt()));
    printAccessStyles(analysis);
}

void writeSessionProductMacro(std::ostream& output, const ProductMacroSessionStats& stats,
                              MacroProductType productType) {
    output << '\n' << productMacroHeading(productType) << "\n\n";
    writeRow(output, "Games analyzed", std::to_string(stats.gamesAnalyzed));
    if (stats.gamesUnavailable > 0)
        writeRow(output, "Games unavailable", std::to_string(stats.gamesUnavailable));
    writeRow(output, "Cycles", std::to_string(stats.cycles));
    writeRow(output, "Average", macroDuration(stats.averageDurationMs()));
    writeRow(output, "Best", macroDuration(stats.bestDurationMs));
    writeRow(output, "Slowest", macroDuration(stats.slowestDurationMs));
    writeRow(output, "Production visits", std::to_string(stats.productionVisits));
    output << "\nACCESS METHOD\n\n";
    writeRow(output, "Control group",
             format(stats.accessMethodPercentage(ProductionAccessMethod::ControlGroup), "%", 1));
    writeRow(output, "Location + click",
             format(stats.accessMethodPercentage(ProductionAccessMethod::LocationHotkeyClick), "%", 1));
    writeRow(output, "Minimap + click",
             format(stats.accessMethodPercentage(ProductionAccessMethod::MinimapClick), "%", 1));
    writeRow(output, "Screen click",
             format(stats.accessMethodPercentage(ProductionAccessMethod::ScreenClick), "%", 1));
    writeSessionAccessStyles(output, stats);
}

constexpr std::array<ArmySelectionMethod, armySelectionMethodCount> armySelectionMethods{
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

std::string milliseconds(const std::optional<double>& value) {
    return format(value, " ms", 0);
}

std::string activitySeconds(const std::optional<double>& millisecondsValue) {
    return millisecondsValue ? format(*millisecondsValue / 1000.0, " s", 1) : "N/A";
}

std::string activeTimelineTime(const std::optional<double>& activeMs) {
    return activeMs ? duration(*activeMs / 1000.0) : "N/A";
}

void writeScoutingUnitActivities(
    std::ostream& output,
    const std::vector<ScoutingUnitActivity>& activities) {
    if (activities.empty())
        return;
    output << "\nSCOUTING UNIT ACTIVITY\n\n";
    for (std::size_t index = 0; index < activities.size(); ++index) {
        if (index > 0)
            output << '\n';
        const auto& activity = activities[index];
        writeRow(output, "Scout group",
                 std::to_string(activity.group) + " (generation " +
                     std::to_string(activity.assignmentGeneration) + ")");
        writeRow(output, "Activity duration",
                 activitySeconds(activity.scoutingActivityDurationMs));
        writeRow(output, "Selections", std::to_string(activity.selectionCount));
        writeRow(output, "Commands", std::to_string(activity.commandCount));
        writeRow(output, "Last commanded",
                 activeTimelineTime(activity.lastCommandActiveMs));
    }
}

void printScoutingUnitActivities(const json::Value& encoded) {
    const auto& activities = encoded["scouting_unit_activity"].asArray();
    if (activities.empty())
        return;
    std::cout << "\nSCOUTING UNIT ACTIVITY\n\n";
    for (std::size_t index = 0; index < activities.size(); ++index) {
        if (index > 0)
            std::cout << '\n';
        const auto& activity = activities[index];
        row("Scout group",
            std::to_string(activity["group"].asInt()) + " (generation " +
                std::to_string(activity["assignment_generation"].asInt()) + ")");
        row("Activity duration",
            activitySeconds(number(activity["activity_duration_ms"])));
        row("Selections", std::to_string(activity["selection_count"].asInt()));
        row("Commands", std::to_string(activity["command_count"].asInt()));
        row("Last commanded",
            activeTimelineTime(number(activity["last_command_active_ms"])));
    }
}

void writeArmyMethodDistribution(
    std::ostream& output, const char* heading,
    const std::array<ArmyControlGroupMethodStatistics, armySelectionMethodCount>& methods,
    std::size_t total) {
    output << '\n' << heading << "\n\n";
    for (const auto method : armySelectionMethods) {
        const auto& statistics = methods[armySelectionMethodIndex(method)];
        if (statistics.editCount == 0)
            continue;
        writeRow(output, armySelectionMethodLabel(method),
                 format(total > 0 ? static_cast<double>(statistics.editCount) * 100.0 /
                                        static_cast<double>(total)
                                  : 0.0,
                        "%", 1));
    }
    for (const auto method : armySelectionMethods) {
        const auto& statistics = methods[armySelectionMethodIndex(method)];
        if (statistics.editCount == 0 || !statistics.averageSelectionToOperationMs)
            continue;
        output << '\n' << armySelectionMethodLabel(method) << " timing\n";
        writeRow(output, "  Edits", std::to_string(statistics.editCount));
        writeRow(output, "  Selection -> group average",
                 milliseconds(statistics.averageSelectionToOperationMs));
        writeRow(output, "  Selection -> group median",
                 milliseconds(statistics.medianSelectionToOperationMs));
        writeRow(output, "  Selection -> group best",
                 milliseconds(statistics.bestSelectionToOperationMs));
        writeRow(output, "  Selection -> group P25",
                 milliseconds(statistics.p25SelectionToOperationMs));
        writeRow(output, "  Selection -> group P75",
                 milliseconds(statistics.p75SelectionToOperationMs));
        writeRow(output, "  Selection -> group P90",
                 milliseconds(statistics.p90SelectionToOperationMs));
        writeRow(output, "  Selection duration average",
                 milliseconds(statistics.averageSelectionDurationMs));
        writeRow(output, "  Total execution average",
                 milliseconds(statistics.averageTotalExecutionMs));
    }
}

void writeArmyControlGroupManagement(std::ostream& output,
                                     const ArmyControlGroupAnalysis& analysis) {
    output << "\nARMY CONTROL-GROUP MANAGEMENT\n\n";
    if (!analysis.available) {
        output << "Unavailable: " << analysis.unavailableReason << '\n';
        return;
    }
    writeRow(output, "Assignments", std::to_string(analysis.assignments));
    writeRow(output, "Additions", std::to_string(analysis.additions));
    writeRow(output, "Edits / min", format(analysis.editsPerMinute(), "", 1));
    if (analysis.excludedScoutingUnitEdits > 0)
        writeRow(output, "Scouting unit excluded",
                 std::to_string(analysis.excludedScoutingUnitEdits));
    if (analysis.excludedProductionBuildingEdits > 0)
        writeRow(output, "Production groups excluded",
                 std::to_string(analysis.excludedProductionBuildingEdits));
    if (analysis.uncertainEdits > 0)
        writeRow(output, "Uncertain (not counted)", std::to_string(analysis.uncertainEdits));
    writeScoutingUnitActivities(output, analysis.scoutingUnitActivities);
    writeArmyMethodDistribution(output, "ASSIGNMENT SELECTION METHOD",
                                analysis.assignmentMethods, analysis.assignments);
    writeArmyMethodDistribution(output, "ADDITION SELECTION METHOD",
                                analysis.additionMethods, analysis.additions);
}

void printArmyControlGroupManagement(const json::Value& encoded) {
    std::cout << "\nARMY CONTROL-GROUP MANAGEMENT\n\n";
    if (!encoded["available"].asBool(false)) {
        std::cout << "Unavailable: "
                  << encoded["reason"].asString("Replay correlation failed") << '\n';
        return;
    }
    const auto assignments = encoded["assignments"].asInt();
    const auto additions = encoded["additions"].asInt();
    row("Assignments", std::to_string(assignments));
    row("Additions", std::to_string(additions));
    row("Edits / min", format(number(encoded["total_group_edits_per_minute"]), "", 1));
    const auto scouting = encoded["excluded_scouting_unit_edits"].asInt();
    if (scouting > 0)
        row("Scouting unit excluded", std::to_string(scouting));
    const auto production = encoded["excluded_production_building_edits"].asInt();
    if (production > 0)
        row("Production groups excluded", std::to_string(production));
    const auto uncertain = encoded["uncertain_edits"].asInt();
    if (uncertain > 0)
        row("Uncertain (not counted)", std::to_string(uncertain));
    printScoutingUnitActivities(encoded);
    const auto printMethods = [&](const char* heading, const char* key, int total) {
        std::cout << '\n' << heading << "\n\n";
        for (const auto method : armySelectionMethods) {
            const auto& statistics = encoded[key][armySelectionMethodName(method)];
            if (statistics["edit_count"].asInt() == 0)
                continue;
            row(armySelectionMethodLabel(method),
                format(number(statistics["percentage"]), "%", 1));
        }
        for (const auto method : armySelectionMethods) {
            const auto& statistics = encoded[key][armySelectionMethodName(method)];
            if (statistics["edit_count"].asInt() == 0 ||
                !statistics["average_selection_to_operation_ms"].isNumber())
                continue;
            std::cout << '\n' << armySelectionMethodLabel(method) << " timing\n";
            row("  Edits", std::to_string(statistics["edit_count"].asInt()));
            row("  Selection -> group average",
                milliseconds(number(statistics["average_selection_to_operation_ms"])));
            row("  Selection -> group median",
                milliseconds(number(statistics["median_selection_to_operation_ms"])));
            row("  Selection -> group best",
                milliseconds(number(statistics["best_selection_to_operation_ms"])));
            row("  Selection -> group P25",
                milliseconds(number(statistics["p25_selection_to_operation_ms"])));
            row("  Selection -> group P75",
                milliseconds(number(statistics["p75_selection_to_operation_ms"])));
            row("  Selection -> group P90",
                milliseconds(number(statistics["p90_selection_to_operation_ms"])));
            row("  Selection duration average",
                milliseconds(number(statistics["average_selection_duration_ms"])));
            row("  Total execution average",
                milliseconds(number(statistics["average_total_execution_ms"])));
        }
        (void)total;
    };
    printMethods("ASSIGNMENT SELECTION METHOD", "assignment_methods", assignments);
    printMethods("ADDITION SELECTION METHOD", "addition_methods", additions);
}

} // namespace

void printSummary(const json::Value& summary, const std::filesystem::path& sessionPath) {
    const auto& navigation = summary["camera_navigation"];
    const int controlGroups = navigation["control_group"]["transitions"].asInt();
    const int locations = navigation["location_hotkey"]["transitions"].asInt();
    const int minimap = navigation["minimap"]["transitions"].asInt();
    const int edge = navigation["edge_scroll"]["episodes"].asInt();
    const int total = navigation["total_transitions"].asInt();

    std::cout << "------------------------------------------------------------\n"
              << "STARCRAFT MECHANICS PROFILER - CAMERA NAVIGATION\n"
              << "------------------------------------------------------------\n\n";
    row("Active time", duration(summary["session"]["active_duration_seconds"].asNumber()));
    const int dropped = summary["session"]["dropped_event_count"].asInt();
    row("Dropped raw events", std::to_string(dropped) + (dropped > 0 ? "  !!" : ""));

    std::cout << "\nNAVIGATION TRANSITIONS\n\n";
    row("Control-group jumps", std::to_string(controlGroups));
    row("Location-hotkey jumps", std::to_string(locations));
    row("Minimap jumps", std::to_string(minimap));
    row("Edge pans", std::to_string(edge));
    row("Total", std::to_string(total));

    std::cout << "\nRATE\n\n";
    row("Navigation transitions/min", format(number(navigation["transitions_per_minute"]), "", 1));

    std::cout << "\nMETHOD DISTRIBUTION\n\n";
    row("Control-group jump", format(percentage(controlGroups, total), "%", 1));
    row("Location hotkey", format(percentage(locations, total), "%", 1));
    row("Minimap", format(percentage(minimap, total), "%", 1));
    row("Edge pan", format(percentage(edge, total), "%", 1));

    printProductMacro(summary["worker_macro_cycles"], summary["production_visits"],
                      MacroProductType::Worker);
    printProductMacro(summary["army_macro_cycles"], summary["production_visits"],
                      MacroProductType::Army);
    printArmyControlGroupManagement(summary["army_control_group_management"]);

    std::cout << "\n------------------------------------------------------------\n";
    if (!sessionPath.empty())
        std::cout << "Saved: " << sessionPath.string() << '\n';
    std::cout << "------------------------------------------------------------\n";
}

std::string formatAutomaticSessionReport(const AutomaticSessionState& session) {
    constexpr const char* separator = "============================================================\n";
    std::ostringstream output;
    output << separator << "SESSION SUMMARY\n" << separator << '\n';
    if (session.empty()) {
        output << "No games recorded this session.\n";
        return output.str();
    }

    const auto& totals = session.stats();
    writeRow(output, "Games", std::to_string(totals.games));
    writeRow(output, "Total active time", duration(totals.activeSeconds));
    output << "\nTOTAL NAVIGATION TRANSITIONS\n\n";
    writeNavigationCounts(output, totals);
    output << "\nSESSION RATE\n\n";
    writeRow(output, "Navigation transitions/min",
             format(totals.navigationTransitionsPerMinute(), "", 1));
    output << "\nSESSION METHOD DISTRIBUTION\n\n";
    writeMethodDistribution(output, totals);
    writeSessionProductMacro(output, totals.workerMacro, MacroProductType::Worker);
    writeSessionProductMacro(output, totals.armyMacro, MacroProductType::Army);
    writeArmyControlGroupManagement(output, totals.armyControlGroups);
    output << '\n' << separator;
    return output.str();
}

void printAutomaticSessionReport(const AutomaticSessionState& session) {
    if (session.empty()) {
        std::cout << '\n' << formatAutomaticSessionReport(session);
        return;
    }

    const auto lastGame = automaticSessionStatsForGame(
        *session.lastGame(), *session.lastGameProduction());
    constexpr const char* separator = "============================================================\n";
    std::cout << '\n' << separator << "LAST GAME\n" << separator << '\n';
    row("Active time", duration(lastGame.activeSeconds));
    std::cout << "\nNAVIGATION TRANSITIONS\n\n";
    printNavigationCounts(lastGame);
    std::cout << "\nRATE\n\n";
    row("Navigation transitions/min", format(lastGame.navigationTransitionsPerMinute(), "", 1));
    std::cout << "\nMETHOD DISTRIBUTION\n\n";
    printMethodDistribution(lastGame);
    printProductMacro(session.lastGameProduction()->workerMacroCycles);
    printProductMacro(session.lastGameProduction()->armyMacroCycles);
    writeArmyControlGroupManagement(std::cout,
                                    session.lastGameProduction()->armyControlGroupManagement);

    std::cout << "\n\n" << formatAutomaticSessionReport(session);
}

void printComparison(const json::Value& latest, const std::vector<json::Value>& baselines) {
    std::cout << "CAMERA NAVIGATION              Latest      Baseline\n\n";
    comparisonRow("Transitions / min", number(latest["camera_navigation"]["transitions_per_minute"]),
                  pooledBaselineTransitionsPerMinute(baselines));
    comparisonRow("Control-group jumps",
                  number(latest["camera_navigation"]["control_group"]["transitions"]),
                  baselineMean(baselines, "control_group", "transitions"), 0);
    comparisonRow("Location-hotkey jumps",
                  number(latest["camera_navigation"]["location_hotkey"]["transitions"]),
                  baselineMean(baselines, "location_hotkey", "transitions"), 0);
    comparisonRow("Minimap jumps", number(latest["camera_navigation"]["minimap"]["transitions"]),
                  baselineMean(baselines, "minimap", "transitions"), 0);
    comparisonRow("Edge pans", number(latest["camera_navigation"]["edge_scroll"]["episodes"]),
                  baselineMean(baselines, "edge_scroll", "episodes"), 0);
}

} // namespace smp
