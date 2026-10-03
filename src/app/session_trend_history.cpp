#include "app/session_trend_history.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>

namespace smp {
namespace {

constexpr std::string_view textSuffix = "_session.txt";
constexpr std::string_view jsonSuffix = "_session.json";

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
        point.overall = decodeSessionTrendStats(root["overall"]);
        if (point.overall.games == 0)
            return std::nullopt;
        if (root["matchups"].isObject()) {
            for (const auto& [name, value] : root["matchups"].asObject()) {
                if (value.isObject())
                    point.matchups.emplace(name,
                                           decodeSessionTrendStats(value));
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

    point.machineReadable = false;
    return point;
}

const SessionTrendStats* statsFor(const SessionTrendPoint& point,
                                  const std::string& matchup) {
    if (matchup == "All matchups")
        return &point.overall;
    const auto found = point.matchups.find(matchup);
    return found == point.matchups.end() ? nullptr : &found->second;
}

} // namespace

SessionTrendHistory loadSessionTrendHistory(const std::filesystem::path& sessionsRoot) {
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

SessionKpiTrendSeries sessionKpiTrendSeries(
    const SessionTrendHistory& history, const std::string& matchup,
    SessionKpi kpi) {
    SessionKpiTrendSeries series;
    for (std::size_t index = 0; index < history.points.size(); ++index) {
        const auto* stats = statsFor(history.points[index], matchup);
        if (!stats)
            continue;
        const auto value = sessionKpiValue(*stats, kpi);
        if (!value || !std::isfinite(*value))
            continue;
        series.xs.push_back(static_cast<double>(index + 1));
        series.ys.push_back(*value);
        series.sourceIndices.push_back(index);
    }
    return series;
}

} // namespace smp
