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

void row(const std::string& label, const std::string& value) {
    std::cout << std::left << std::setw(40) << label << std::right << std::setw(12) << value << '\n';
}

std::string duration(double seconds) {
    const auto total = static_cast<long long>(std::llround(seconds));
    std::ostringstream output;
    output << total / 60 << ':' << std::setw(2) << std::setfill('0') << total % 60;
    return output.str();
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

void comparisonRow(const char* label, std::optional<double> current, std::optional<double> baseline,
                   int precision = 1) {
    std::cout << std::left << std::setw(30) << label << std::right << std::setw(14)
              << format(current, "", precision) << std::setw(14) << format(baseline, "", precision) << '\n';
}

void printNavigationCounts(const AutomaticSessionStats& stats) {
    row("Control-group jumps", std::to_string(stats.controlGroupJumps));
    row("Location-hotkey jumps", std::to_string(stats.locationHotkeyJumps));
    row("Minimap jumps", std::to_string(stats.minimapJumps));
    row("Edge pans", std::to_string(stats.edgePans));
    row("Total", std::to_string(stats.navigationTransitions()));
}

void printMethodDistribution(const AutomaticSessionStats& stats) {
    row("Control-group jump", format(stats.methodPercentage(stats.controlGroupJumps), "%", 1));
    row("Location hotkey", format(stats.methodPercentage(stats.locationHotkeyJumps), "%", 1));
    row("Minimap", format(stats.methodPercentage(stats.minimapJumps), "%", 1));
    row("Edge pan", format(stats.methodPercentage(stats.edgePans), "%", 1));
}

void printEdgePanCounts(const AutomaticSessionStats& stats) {
    row("Total", std::to_string(stats.edgePans));
    row("Left", std::to_string(stats.edgeLeft));
    row("Right", std::to_string(stats.edgeRight));
    row("Top", std::to_string(stats.edgeTop));
    row("Bottom", std::to_string(stats.edgeBottom));
    row("Corners", std::to_string(stats.edgeCorners));
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

    std::cout << "\nEDGE PAN\n\n";
    row("Total", std::to_string(edge));
    const auto& directions = navigation["edge_scroll"]["by_direction"];
    row("Left", std::to_string(directions["LEFT"].asInt()));
    row("Right", std::to_string(directions["RIGHT"].asInt()));
    row("Top", std::to_string(directions["TOP"].asInt()));
    row("Bottom", std::to_string(directions["BOTTOM"].asInt()));
    const int corners = directions["TOP_LEFT"].asInt() + directions["TOP_RIGHT"].asInt() +
                        directions["BOTTOM_LEFT"].asInt() + directions["BOTTOM_RIGHT"].asInt();
    row("Corners", std::to_string(corners));
    std::cout << "\n------------------------------------------------------------\n";
    if (!sessionPath.empty())
        std::cout << "Saved: " << sessionPath.string() << '\n';
    std::cout << "------------------------------------------------------------\n";
}

void printAutomaticSessionReport(const AutomaticSessionState& session) {
    constexpr const char* separator = "============================================================\n";
    if (session.empty()) {
        std::cout << '\n' << separator << "SESSION SUMMARY\n" << separator
                  << "\nNo games recorded this session.\n";
        return;
    }

    const auto lastGame = automaticSessionStatsForGame(*session.lastGame());
    std::cout << '\n' << separator << "LAST GAME\n" << separator << '\n';
    row("Active time", duration(lastGame.activeSeconds));
    std::cout << "\nNAVIGATION TRANSITIONS\n\n";
    printNavigationCounts(lastGame);
    std::cout << "\nRATE\n\n";
    row("Navigation transitions/min", format(lastGame.navigationTransitionsPerMinute(), "", 1));
    std::cout << "\nMETHOD DISTRIBUTION\n\n";
    printMethodDistribution(lastGame);
    std::cout << "\nEDGE PAN\n\n";
    printEdgePanCounts(lastGame);

    const auto& totals = session.stats();
    std::cout << "\n\n" << separator << "SESSION SUMMARY\n" << separator << '\n';
    row("Games", std::to_string(totals.games));
    row("Total active time", duration(totals.activeSeconds));
    std::cout << "\nTOTAL NAVIGATION TRANSITIONS\n\n";
    printNavigationCounts(totals);
    std::cout << "\nSESSION RATE\n\n";
    row("Navigation transitions/min", format(totals.navigationTransitionsPerMinute(), "", 1));
    std::cout << "\nSESSION METHOD DISTRIBUTION\n\n";
    printMethodDistribution(totals);
    std::cout << "\nTOTAL EDGE PANS\n\n";
    printEdgePanCounts(totals);
    std::cout << '\n' << separator;
}

void printComparison(const json::Value& latest, const std::vector<json::Value>& baselines) {
    std::cout << "CAMERA NAVIGATION              Latest      Baseline\n\n";
    comparisonRow("Transitions / min", number(latest["camera_navigation"]["transitions_per_minute"]),
                  baselineMean(baselines, "", "transitions_per_minute"));
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
