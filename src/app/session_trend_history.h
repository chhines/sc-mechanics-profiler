#pragma once

#include "app/session_trend_data.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace smp {

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

struct SessionKpiTrendSeries {
    std::vector<double> xs;
    std::vector<double> ys;
    std::vector<std::size_t> sourceIndices;
};

[[nodiscard]] SessionTrendHistory loadSessionTrendHistory(
    const std::filesystem::path& sessionsRoot);

[[nodiscard]] SessionKpiTrendSeries sessionKpiTrendSeries(
    const SessionTrendHistory& history, const std::string& matchup,
    SessionKpi kpi);

} // namespace smp
