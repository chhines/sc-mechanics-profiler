#include "cli/report.h"

#include "analysis/statistics.h"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>

namespace scm {
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
    std::cout << std::left << std::setw(38) << label << std::right << std::setw(14) << value << '\n';
}

std::string duration(double seconds) {
    const auto total = static_cast<long long>(std::llround(seconds));
    std::ostringstream output;
    output << total / 60 << ':' << std::setw(2) << std::setfill('0') << total % 60;
    return output.str();
}

std::optional<double> metric(const json::Value& summary, const std::string& name) {
    if (name == "pac")
        return number(summary["pac"]["first_action_latency_ms"]["median"]);
    if (name == "switch")
        return number(summary["control_groups"]["switch_to_action_ms"]["median"]);
    if (name == "command")
        return number(summary["commands"]["command_to_target_ms"]["median"]);
    if (name == "worker")
        return number(summary["macro"]["worker"]["attempt_interval_ms"]["median"]);
    if (name == "army")
        return number(summary["macro"]["army"]["revisit_interval_ms"]["median"]);
    if (name == "slowdown")
        return number(summary["macro"]["under_load"]["army_revisit_change_pct"]);
    if (name == "capacity")
        return number(summary["capacity"]["estimated_breakpoint_eapm"]);
    return std::nullopt;
}

void comparisonRow(const char* label, const std::optional<double>& current, const std::optional<double>& baseline,
                   const char* unit) {
    std::string change = "N/A";
    if (current && baseline)
        change = format(percentageChange(*baseline, *current), "%", 1);
    std::cout << std::left << std::setw(27) << label << std::right << std::setw(15)
              << format(current, unit, unit[0] == '%' ? 1 : 0) << std::setw(17)
              << format(baseline, unit, unit[0] == '%' ? 1 : 0) << std::setw(13) << change << '\n';
}

} // namespace

void printSummary(const json::Value& summary, const std::filesystem::path& sessionDirectory) {
    std::cout << "------------------------------------------------------------\n"
              << "SCMECHANICS SESSION\n"
              << "------------------------------------------------------------\n";
    row("Session", summary["session"]["id"].asString("unknown"));
    row("Active time", duration(summary["session"]["active_duration_seconds"].asNumber()));
    const auto dropped = summary["session"]["dropped_event_count"].asInt();
    row("Dropped events", std::to_string(dropped) + (dropped > 0 ? "  !!" : ""));

    std::cout << "\nPACE\n";
    row("Raw APM", format(number(summary["pace"]["raw_apm"]), "", 1));
    row("Input-derived EAPM", format(number(summary["pace"]["input_derived_eapm"]), "", 1));
    row("Median effective inter-action", format(number(summary["pace"]["effective_inter_action_ms"]["median"]), " ms"));
    row("P90 effective inter-action", format(number(summary["pace"]["effective_inter_action_ms"]["p90"]), " ms"));

    std::cout << "\nINFERRED PAC\n";
    row("PACs / min", format(number(summary["pac"]["rate_per_minute"]), "", 1));
    row("Median first action", format(number(summary["pac"]["first_action_latency_ms"]["median"]), " ms"));
    row("P90 first action", format(number(summary["pac"]["first_action_latency_ms"]["p90"]), " ms"));
    row("Actions / PAC", format(number(summary["pac"]["actions_per_pac"]["mean"]), "", 1));
    row("Actionless PAC ratio", format(number(summary["pac"]["actionless_ratio"]), "", 2));

    std::cout << "\nCONTROL GROUPS\n";
    row("Switches / min", format(number(summary["control_groups"]["switches_per_minute"]), "", 1));
    row("Median switch -> action", format(number(summary["control_groups"]["switch_to_action_ms"]["median"]), " ms"));
    row("P90 switch -> action", format(number(summary["control_groups"]["switch_to_action_ms"]["p90"]), " ms"));
    row("Productive selection ratio", format(number(summary["control_groups"]["productive_selection_ratio"]), "", 2));

    std::cout << "\nCAMERA NAVIGATION\n"
              << std::left << std::setw(26) << "Method" << std::right << std::setw(10) << "Usage" << std::setw(14)
              << "Med cost" << std::setw(14) << "P90 cost" << '\n';
    const auto navRow = [&](const char* label, const char* key) {
        const auto& nav = summary["camera_navigation"][key];
        std::cout << std::left << std::setw(26) << label << std::right << std::setw(9)
                  << format(number(nav["events_per_minute"]), "", 1) << std::setw(14)
                  << format(number(nav["transition_cost_ms"]["median"]), " ms") << std::setw(14)
                  << format(number(nav["transition_cost_ms"]["p90"]), " ms") << '\n';
    };
    navRow("Control-group jump", "control_group_jump");
    navRow("Location hotkey", "location_hotkey");
    navRow("Minimap jump", "minimap_jump");
    navRow("Edge scroll", "edge_scroll");

    std::cout << "\nCOMMAND EXECUTION\n";
    row("Command -> target median", format(number(summary["commands"]["command_to_target_ms"]["median"]), " ms"));
    row("Command -> target P90", format(number(summary["commands"]["command_to_target_ms"]["p90"]), " ms"));

    std::cout << "\nBOX SELECTION\n";
    row("Box duration median", format(number(summary["box_selection"]["duration_ms"]["median"]), " ms"));
    row("Box -> command median", format(number(summary["box_selection"]["box_to_command_ms"]["median"]), " ms"));
    row("Mean path efficiency", format(number(summary["box_selection"]["mean_path_efficiency"]), "", 2));
    row("Probable re-selection rate", format(number(summary["box_selection"]["probable_reselection_rate"]), "", 2));

    std::cout << "\nMACRO - WORKER PRODUCTION\n";
    row("Attempt interval median", format(number(summary["macro"]["worker"]["attempt_interval_ms"]["median"]), " ms"));
    row("Attempt interval P90", format(number(summary["macro"]["worker"]["attempt_interval_ms"]["p90"]), " ms"));
    row("Longest attempt interval",
        format(number(summary["macro"]["worker"]["attempt_interval_ms"]["maximum"]), " ms"));
    std::cout << "\nMACRO - ARMY PRODUCTION\n";
    row("Revisit interval median", format(number(summary["macro"]["army"]["revisit_interval_ms"]["median"]), " ms"));
    row("Revisit interval P90", format(number(summary["macro"]["army"]["revisit_interval_ms"]["p90"]), " ms"));
    row("Army episode duration", format(number(summary["macro"]["army"]["episode_duration_ms"]["median"]), " ms"));
    row("Army production-group coverage", format(number(summary["macro"]["army"]["production_group_coverage"]), "", 2));
    std::cout << "\nMACRO UNDER LOAD\n";
    row("Worker interval change", format(number(summary["macro"]["under_load"]["worker_interval_change_pct"]), "%", 1));
    row("Army revisit change", format(number(summary["macro"]["under_load"]["army_revisit_change_pct"]), "%", 1));
    row("Micro -> macro return median", format(number(summary["macro"]["micro_to_macro_return_ms"]["median"]), " ms"));

    std::cout << "\nREPEATED SEQUENCES\n";
    const auto& sequences = summary["sequences"].asArray();
    if (sequences.empty())
        row("Top sequence", "N/A");
    else {
        row("Top sequence", sequences.front()["sequence"].asString());
        row("Occurrences", std::to_string(sequences.front()["count"].asInt()));
        row("Median execution", format(number(sequences.front()["duration_ms"]["median"]), " ms"));
        row("Execution MAD", format(number(sequences.front()["duration_ms"]["mad"]), " ms"));
    }

    std::cout << "\nCAPACITY\n";
    row("Estimated mechanical capacity breakpoint",
        format(number(summary["capacity"]["estimated_breakpoint_eapm"]), " EAPM"));

    std::cout << "\nCONSISTENCY\n";
    row("PAC late-session change",
        format(number(summary["consistency"]["late_session_change_pct"]["pac_first_action"]), "%", 1));
    row("Switch late-session change",
        format(number(summary["consistency"]["late_session_change_pct"]["control_group_switch"]), "%", 1));
    std::cout << "------------------------------------------------------------\n";
    if (!sessionDirectory.empty())
        std::cout << "Saved: " << sessionDirectory.string() << "\n";
    std::cout << "------------------------------------------------------------\n";
}

void printComparison(const json::Value& latest, const std::vector<json::Value>& baselines) {
    std::cout << "Metric                         Latest         Baseline       Change\n\n";
    const auto baselineFor = [&](const std::string& name) -> std::optional<double> {
        std::vector<double> values;
        for (const auto& item : baselines)
            if (const auto value = metric(item, name))
                values.push_back(*value);
        return median(std::move(values), 1);
    };
    comparisonRow("PAC latency", metric(latest, "pac"), baselineFor("pac"), " ms");
    comparisonRow("Switch latency", metric(latest, "switch"), baselineFor("switch"), " ms");
    comparisonRow("Command -> target", metric(latest, "command"), baselineFor("command"), " ms");
    comparisonRow("Worker interval", metric(latest, "worker"), baselineFor("worker"), " ms");
    comparisonRow("Army revisit", metric(latest, "army"), baselineFor("army"), " ms");
    comparisonRow("High-load army slowdown", metric(latest, "slowdown"), baselineFor("slowdown"), "%");
    comparisonRow("Capacity breakpoint", metric(latest, "capacity"), baselineFor("capacity"), " EAPM");
}

} // namespace scm
