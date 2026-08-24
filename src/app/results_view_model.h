#pragma once

#include "app/game_analysis_visualization_model.h"
#include "app/gui_preferences.h"
#include "cli/automatic_session_stats.h"
#include "util/json.h"

#include <string>
#include <vector>

namespace smp {

struct ResultsMetric {
    std::string label;
    std::string value;
    std::string tooltip;
};

struct ResultsSection {
    std::string id;
    std::string title;
    std::vector<ResultsMetric> metrics;
};

struct ResultsViewModel {
    std::string title;
    std::string subtitle;
    std::vector<ResultsSection> sections;

    [[nodiscard]] bool hasSection(const std::string& id) const noexcept;
};

[[nodiscard]] ResultsViewModel deriveGameResults(
    const json::Value& summary, const ReportGroupVisibility& visibility,
    const GameAnalysisVisualizationModel* visualization = nullptr);
[[nodiscard]] ResultsViewModel deriveSessionResults(
    const AutomaticSessionStats& stats,
    const SessionReportVisibility& visibility);

} // namespace smp
