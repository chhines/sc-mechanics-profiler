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
    output << '\n' << separator;
    return output.str();
}

void printAutomaticSessionReport(const AutomaticSessionState& session) {
    if (session.empty()) {
        std::cout << '\n' << formatAutomaticSessionReport(session);
        return;
    }

    const auto lastGame = automaticSessionStatsForGame(*session.lastGame());
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
