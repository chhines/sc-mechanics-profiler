#include "app/session_trends.h"

#include "imgui.h"
#include "implot.h"
#include "util/json.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace smp {
namespace {

constexpr std::string_view textSuffix = "_session.txt";
constexpr std::string_view jsonSuffix = "_session.json";
constexpr float trendPlotHeight = 215.0f;

struct SessionTrendStats {
    std::uint64_t games{};
    std::optional<double> navigationTransitionsPerMinute;
    std::optional<double> workerMacroAverageMs;
    std::optional<double> armyMacroAverageMs;
    std::optional<double> armyControlGroupEditsPerMinute;
    std::optional<double> workerMacroCyclesPerGame;
    std::optional<double> armyMacroCyclesPerGame;
};

struct SessionTrendPoint {
    std::string sessionId;
    SessionTrendStats overall;
    std::map<std::string, SessionTrendStats> matchups;
    bool machineReadable{};
};

struct SessionTrendHistory {
    std::vector<SessionTrendPoint> points;
    std::size_t jsonSessions{};
    std::size_t legacyTextSessions{};
};

std::string trim(std::string value) {
    const auto notSpace = [](unsigned char ch) { return std::isspace(ch) == 0; };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
                value.end());
    return value;
}

std::string sessionIdFromFilename(const std::filesystem::path& path,
                                  std::string_view suffix) {
    const auto filename = path.filename().string();
    if (filename.size() <= suffix.size() || !filename.ends_with(suffix))
        return {};
    return filename.substr(0, filename.size() - suffix.size());
}

std::optional<double> numeric(const json::Value& value) {
    return value.isNumber() ? std::optional<double>(value.asNumber())
                            : std::nullopt;
}

SessionTrendStats decodeStats(const json::Value& value) {
    SessionTrendStats stats;
    stats.games = static_cast<std::uint64_t>(
        std::max(0.0, value["games"].asNumber()));
    stats.navigationTransitionsPerMinute =
        numeric(value["navigation"]["transitions_per_minute"]);
    stats.workerMacroAverageMs =
        numeric(value["worker_macro"]["average_duration_ms"]);
    stats.armyMacroAverageMs =
        numeric(value["army_macro"]["average_duration_ms"]);
    stats.armyControlGroupEditsPerMinute =
        numeric(value["army_control_groups"]["edits_per_minute"]);
    const auto workerCycles = numeric(value["worker_macro"]["cycles"]);
    const auto armyCycles = numeric(value["army_macro"]["cycles"]);
    if (stats.games > 0 && workerCycles)
        stats.workerMacroCyclesPerGame =
            *workerCycles / static_cast<double>(stats.games);
    if (stats.games > 0 && armyCycles)
        stats.armyMacroCyclesPerGame =
            *armyCycles / static_cast<double>(stats.games);
    return stats;
}

std::optional<SessionTrendPoint>
loadJsonSession(const std::filesystem::path& path) {
    try {
        const auto root = json::parseFile(path);
        if (!root.isObject() || !root["overall"].isObject())
            return std::nullopt;
        SessionTrendPoint point;
        point.sessionId = root["session_id"].asString();
        if (point.sessionId.empty())
            point.sessionId = sessionIdFromFilename(path, jsonSuffix);
        if (point.sessionId.empty())
            return std::nullopt;
        point.overall = decodeStats(root["overall"]);
        if (root["matchups"].isObject()) {
            for (const auto& [name, value] : root["matchups"].asObject()) {
                if (value.isObject())
                    point.matchups.emplace(name, decodeStats(value));
            }
        }
        point.machineReadable = true;
        return point;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> parseNumber(std::string value) {
    value = trim(std::move(value));
    if (value.empty() || value == "N/A")
        return std::nullopt;
    try {
        std::size_t consumed{};
        const double parsed = std::stod(value, &consumed);
        if (consumed == 0 || !std::isfinite(parsed))
            return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::pair<std::string, std::string> reportRow(const std::string& line) {
    if (line.size() <= 40)
        return {};
    return {trim(line.substr(0, 40)), trim(line.substr(40))};
}

std::optional<SessionTrendPoint>
loadLegacyTextSession(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    enum class Section { None, WorkerMacro, ArmyMacro, ControlGroups };
    Section section = Section::None;
    SessionTrendPoint point;
    point.sessionId = sessionIdFromFilename(path, textSuffix);
    if (point.sessionId.empty())
        return std::nullopt;
    bool workerAverageSet = false;
    bool armyAverageSet = false;
    std::optional<double> workerCycles;
    std::optional<double> armyCycles;

    std::string line;
    while (std::getline(input, line)) {
        line = trim(std::move(line));
        if (line == "MATCHUP BREAKDOWN")
            break;
        if (line == "WORKER MACRO") {
            section = Section::WorkerMacro;
            continue;
        }
        if (line == "ARMY MACRO") {
            section = Section::ArmyMacro;
            continue;
        }
        if (line.find("CONTROL-GROUP") != std::string::npos ||
            line.find("CONTROL GROUP") != std::string::npos) {
            section = Section::ControlGroups;
            continue;
        }
        if (line == "ACCESS METHOD" || line.find("ACCESS STYLES") != std::string::npos ||
            line.find("SPEED BY ACCESS STYLE") != std::string::npos ||
            line == "METHOD DISTRIBUTION") {
            section = Section::None;
            continue;
        }

        const auto [label, value] = reportRow(line);
        if (label.empty())
            continue;
        if (label == "Games" && point.overall.games == 0) {
            if (const auto parsed = parseNumber(value))
                point.overall.games = static_cast<std::uint64_t>(
                    std::max(0.0, *parsed));
        } else if (label == "Navigation transitions/min") {
            point.overall.navigationTransitionsPerMinute = parseNumber(value);
        } else if (section == Section::WorkerMacro && label == "Cycles") {
            workerCycles = parseNumber(value);
        } else if (section == Section::ArmyMacro && label == "Cycles") {
            armyCycles = parseNumber(value);
        } else if (section == Section::WorkerMacro && label == "Average" &&
                   !workerAverageSet) {
            if (const auto parsed = parseNumber(value)) {
                point.overall.workerMacroAverageMs = *parsed * 1000.0;
                workerAverageSet = true;
            }
        } else if (section == Section::ArmyMacro && label == "Average" &&
                   !armyAverageSet) {
            if (const auto parsed = parseNumber(value)) {
                point.overall.armyMacroAverageMs = *parsed * 1000.0;
                armyAverageSet = true;
            }
        } else if (section == Section::ControlGroups && label == "Edits / min") {
            point.overall.armyControlGroupEditsPerMinute = parseNumber(value);
        }
    }

    if (point.overall.games > 0) {
        if (workerCycles)
            point.overall.workerMacroCyclesPerGame =
                *workerCycles / static_cast<double>(point.overall.games);
        if (armyCycles)
            point.overall.armyMacroCyclesPerGame =
                *armyCycles / static_cast<double>(point.overall.games);
    }
    point.machineReadable = false;
    return point;
}

SessionTrendHistory loadHistory(const std::filesystem::path& sessionsRoot) {
    SessionTrendHistory history;
    std::map<std::string, SessionTrendPoint> byId;
    std::error_code error;
    if (!std::filesystem::is_directory(sessionsRoot, error) || error)
        return history;

    for (std::filesystem::directory_iterator iterator(
             sessionsRoot, std::filesystem::directory_options::skip_permission_denied,
             error),
         end;
         !error && iterator != end; iterator.increment(error)) {
        std::error_code entryError;
        if (!iterator->is_regular_file(entryError) || entryError)
            continue;
        const auto path = iterator->path();
        const auto filename = path.filename().string();
        if (!filename.ends_with(jsonSuffix))
            continue;
        if (auto point = loadJsonSession(path))
            byId[point->sessionId] = std::move(*point);
    }

    error.clear();
    for (std::filesystem::directory_iterator iterator(
             sessionsRoot, std::filesystem::directory_options::skip_permission_denied,
             error),
         end;
         !error && iterator != end; iterator.increment(error)) {
        std::error_code entryError;
        if (!iterator->is_regular_file(entryError) || entryError)
            continue;
        const auto path = iterator->path();
        const auto filename = path.filename().string();
        if (!filename.ends_with(textSuffix))
            continue;
        const auto id = sessionIdFromFilename(path, textSuffix);
        if (id.empty() || byId.contains(id))
            continue;
        if (auto point = loadLegacyTextSession(path))
            byId[point->sessionId] = std::move(*point);
    }

    history.points.reserve(byId.size());
    for (auto& [id, point] : byId) {
        if (point.machineReadable)
            ++history.jsonSessions;
        else
            ++history.legacyTextSessions;
        history.points.push_back(std::move(point));
    }
    return history;
}

const SessionTrendStats* statsFor(const SessionTrendPoint& point,
                                  const std::string& matchup) {
    if (matchup == "All matchups")
        return &point.overall;
    const auto found = point.matchups.find(matchup);
    return found == point.matchups.end() ? nullptr : &found->second;
}

enum class TrendMetric {
    NavigationRate,
    WorkerMacroDuration,
    ArmyMacroDuration,
    ControlGroupRate,
    WorkerCyclesPerGame,
    ArmyCyclesPerGame,
};

std::optional<double> metricValue(const SessionTrendStats& stats,
                                  TrendMetric metric) {
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
    case TrendMetric::WorkerCyclesPerGame:
        return stats.workerMacroCyclesPerGame;
    case TrendMetric::ArmyCyclesPerGame:
        return stats.armyMacroCyclesPerGame;
    }
    return std::nullopt;
}

const char* metricTitle(TrendMetric metric) {
    switch (metric) {
    case TrendMetric::NavigationRate:
        return "Navigation transitions / minute";
    case TrendMetric::WorkerMacroDuration:
        return "Average worker macro duration";
    case TrendMetric::ArmyMacroDuration:
        return "Average army macro duration";
    case TrendMetric::ControlGroupRate:
        return "Army control-group edits / minute";
    case TrendMetric::WorkerCyclesPerGame:
        return "Worker macro cycles / game";
    case TrendMetric::ArmyCyclesPerGame:
        return "Army macro cycles / game";
    }
    return "Trend";
}

const char* metricYAxis(TrendMetric metric) {
    return metric == TrendMetric::WorkerMacroDuration ||
                   metric == TrendMetric::ArmyMacroDuration
               ? "Seconds"
               : metricTitle(metric);
}

void drawTrendPlot(const SessionTrendHistory& history,
                   const std::string& matchup,
                   TrendMetric metric,
                   int colorIndex) {
    std::vector<double> xs;
    std::vector<double> ys;
    std::vector<std::size_t> sourceIndices;
    for (std::size_t index = 0; index < history.points.size(); ++index) {
        const auto* stats = statsFor(history.points[index], matchup);
        if (!stats)
            continue;
        const auto value = metricValue(*stats, metric);
        if (!value || !std::isfinite(*value))
            continue;
        xs.push_back(static_cast<double>(index + 1));
        ys.push_back(*value);
        sourceIndices.push_back(index);
    }

    if (ys.empty()) {
        ImGui::TextDisabled("%s: no compatible session data.", metricTitle(metric));
        return;
    }
    const double maximum = std::max(
        0.1, *std::max_element(ys.begin(), ys.end()));
    const double sessionMaximum =
        std::max(1.0, static_cast<double>(history.points.size()));
    if (ImPlot::BeginPlot(metricTitle(metric),
                          ImVec2(-1.0f, trendPlotHeight),
                          ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, "Session", ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.5, sessionMaximum + 0.5,
                                ImPlotCond_Always);
        ImPlot::SetupAxis(ImAxis_Y1, metricYAxis(metric));
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, maximum * 1.15,
                                ImPlotCond_Always);
        ImPlot::SetupFinish();

        auto* draw = ImPlot::GetPlotDrawList();
        const ImVec4 color =
            ImPlot::GetColormapColor(colorIndex, ImPlotColormap_Deep);
        const ImU32 packed = ImGui::ColorConvertFloat4ToU32(color);
        ImPlot::PushPlotClipRect();
        for (std::size_t index = 0; index < xs.size(); ++index) {
            const ImVec2 point = ImPlot::PlotToPixels(xs[index], ys[index]);
            if (index > 0) {
                const ImVec2 previous =
                    ImPlot::PlotToPixels(xs[index - 1], ys[index - 1]);
                draw->AddLine(previous, point, packed, 2.0f);
            }
            draw->AddCircleFilled(point, 4.5f, packed, 16);
        }
        ImPlot::PopPlotClipRect();

        if (ImPlot::IsPlotHovered()) {
            const auto mouse = ImPlot::GetPlotMousePos();
            std::size_t nearest = 0;
            double best = std::abs(xs.front() - mouse.x);
            for (std::size_t index = 1; index < xs.size(); ++index) {
                const double distance = std::abs(xs[index] - mouse.x);
                if (distance < best) {
                    best = distance;
                    nearest = index;
                }
            }
            if (best <= 0.35) {
                const auto& point = history.points[sourceIndices[nearest]];
                const auto* stats = statsFor(point, matchup);
                ImGui::BeginTooltip();
                ImGui::Text("Session: %s", point.sessionId.c_str());
                if (stats)
                    ImGui::Text("Games in sample: %llu",
                                static_cast<unsigned long long>(stats->games));
                ImGui::Text("%s: %.2f", metricTitle(metric), ys[nearest]);
                if (!point.machineReadable)
                    ImGui::TextDisabled("Loaded from legacy text summary");
                ImGui::EndTooltip();
            }
        }
        ImPlot::EndPlot();
    }
}

} // namespace

void drawSessionTrends(const std::filesystem::path& sessionsRoot) {
    static std::filesystem::path cachedRoot;
    static SessionTrendHistory history;
    static double refreshAt{};
    static std::string selectedMatchup{"All matchups"};

    const double now = ImGui::GetTime();
    if (cachedRoot != sessionsRoot || now >= refreshAt) {
        cachedRoot = sessionsRoot;
        history = loadHistory(sessionsRoot);
        refreshAt = now + 2.0;
    }

    ImGui::SeparatorText("Session trends");
    ImGui::TextWrapped(
        "Each point is one automatic-session summary in the sessions folder. "
        "New JSON summaries provide matchup-aware history; older text summaries "
        "remain in the overall trend when their headline values can be parsed.");
    ImGui::Text("Sessions loaded: %zu", history.points.size());
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::TextDisabled("%zu JSON, %zu legacy text", history.jsonSessions,
                        history.legacyTextSessions);

    std::set<std::string> matchups;
    for (const auto& point : history.points) {
        for (const auto& [name, ignored] : point.matchups) {
            if (name != "Unknown")
                matchups.insert(name);
            (void)ignored;
        }
    }
    if (selectedMatchup != "All matchups" && !matchups.contains(selectedMatchup))
        selectedMatchup = "All matchups";

    ImGui::TextDisabled("Matchup");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("##TrendMatchup", selectedMatchup.c_str())) {
        if (ImGui::Selectable("All matchups",
                              selectedMatchup == "All matchups"))
            selectedMatchup = "All matchups";
        for (const auto& matchup : matchups) {
            if (ImGui::Selectable(matchup.c_str(), selectedMatchup == matchup))
                selectedMatchup = matchup;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh session history")) {
        history = loadHistory(sessionsRoot);
        refreshAt = now + 2.0;
    }

    if (history.points.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No automatic session summaries were found yet.");
        return;
    }

    ImGui::Spacing();
    drawTrendPlot(history, selectedMatchup, TrendMetric::NavigationRate, 0);
    drawTrendPlot(history, selectedMatchup, TrendMetric::WorkerMacroDuration, 1);
    drawTrendPlot(history, selectedMatchup, TrendMetric::ArmyMacroDuration, 2);
    drawTrendPlot(history, selectedMatchup, TrendMetric::ControlGroupRate, 3);
    drawTrendPlot(history, selectedMatchup, TrendMetric::WorkerCyclesPerGame, 4);
    drawTrendPlot(history, selectedMatchup, TrendMetric::ArmyCyclesPerGame, 5);
}

} // namespace smp
