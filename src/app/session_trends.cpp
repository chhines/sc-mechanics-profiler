#include "app/session_trends.h"

#include "app/session_trend_history.h"
#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace smp {
namespace {

constexpr float trendPlotHeight = 215.0f;

void setupSessionXAxis(std::size_t sessionCount) {
    const auto ticks = sessionTrendTickValues(sessionCount);
    const auto limits = sessionTrendXAxisLimits(sessionCount);
    ImPlot::SetupAxis(ImAxis_X1, "Session", ImPlotAxisFlags_Lock);
    ImPlot::SetupAxisFormat(ImAxis_X1, "%.0f");
    ImPlot::SetupAxisLimits(ImAxis_X1, limits.minimum, limits.maximum,
                            ImPlotCond_Always);
    ImPlot::SetupAxisTicks(ImAxis_X1, ticks.data(),
                           static_cast<int>(ticks.size()), nullptr, false);
}

void drawTrendPlot(const SessionTrendHistory& history,
                   const std::string& matchup,
                   SessionKpi kpi) {
    const auto& definition = sessionKpiDefinition(kpi);
    const auto series = sessionKpiTrendSeries(history, matchup, kpi);

    if (series.ys.empty()) {
        ImGui::TextDisabled("%s: no compatible session data.",
                            definition.title);
        return;
    }
    const double maximum = std::max(
        0.1, *std::max_element(series.ys.begin(), series.ys.end()));
    if (ImPlot::BeginPlot(definition.title,
                          ImVec2(-1.0f, trendPlotHeight),
                          ImPlotFlags_NoMouseText)) {
        setupSessionXAxis(history.points.size());
        ImPlot::SetupAxis(ImAxis_Y1,
                          sessionKpiUsesSeconds(kpi) ? "Seconds"
                                                    : definition.title);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, maximum * 1.15,
                                ImPlotCond_Always);
        ImPlot::SetupFinish();

        auto* draw = ImPlot::GetPlotDrawList();
        const ImVec4 color =
            ImPlot::GetColormapColor(definition.colorIndex,
                                     ImPlotColormap_Deep);
        const ImU32 packed = ImGui::ColorConvertFloat4ToU32(color);
        ImPlot::PushPlotClipRect(5.0f);
        for (std::size_t index = 0; index < series.xs.size(); ++index) {
            const ImVec2 point = ImPlot::PlotToPixels(
                series.xs[index], series.ys[index]);
            if (index > 0) {
                const ImVec2 previous = ImPlot::PlotToPixels(
                    series.xs[index - 1], series.ys[index - 1]);
                draw->AddLine(previous, point, packed, 2.0f);
            }
            draw->AddCircleFilled(point, 4.5f, packed, 16);
        }
        ImPlot::PopPlotClipRect();

        if (ImPlot::IsPlotHovered()) {
            const auto mouse = ImPlot::GetPlotMousePos();
            std::size_t nearest = 0;
            double best = std::abs(series.xs.front() - mouse.x);
            for (std::size_t index = 1; index < series.xs.size(); ++index) {
                const double distance =
                    std::abs(series.xs[index] - mouse.x);
                if (distance < best) {
                    best = distance;
                    nearest = index;
                }
            }
            if (best <= 0.35) {
                const auto& point =
                    history.points[series.sourceIndices[nearest]];
                ImGui::BeginTooltip();
                ImGui::Text("Session: %s", point.sessionId.c_str());
                ImGui::Text("%s: %.2f%s", definition.title,
                            series.ys[nearest],
                            sessionKpiUsesSeconds(kpi) ? " s" : "");
                if (!point.machineReadable)
                    ImGui::TextDisabled("Loaded from legacy text summary");
                ImGui::EndTooltip();
            }
        }
        ImPlot::EndPlot();
    }
}

void drawPairedTrendPlot(
    const SessionTrendHistory& history, const std::string& matchup,
    const SessionReportVisibility& visibility,
    const SessionKpiPairDefinition& pair) {
    const bool showWorker = visibility.visible(pair.worker);
    const bool showArmy = visibility.visible(pair.army);
    if (!showWorker && !showArmy)
        return;

    const SessionKpiTrendSeries worker =
        showWorker ? sessionKpiTrendSeries(history, matchup, pair.worker)
                   : SessionKpiTrendSeries{};
    const SessionKpiTrendSeries army =
        showArmy ? sessionKpiTrendSeries(history, matchup, pair.army)
                 : SessionKpiTrendSeries{};
    if (worker.ys.empty() && army.ys.empty()) {
        ImGui::TextDisabled("%s: no compatible session data.", pair.title);
        return;
    }

    const auto workerColor = ImPlot::GetColormapColor(
        sessionKpiDefinition(pair.worker).colorIndex, ImPlotColormap_Deep);
    const auto armyColor = ImPlot::GetColormapColor(
        sessionKpiDefinition(pair.army).colorIndex, ImPlotColormap_Deep);
    if (showWorker)
        ImGui::TextColored(workerColor, "Worker");
    if (showWorker && showArmy)
        ImGui::SameLine(0.0f, 18.0f);
    if (showArmy)
        ImGui::TextColored(armyColor, "Army");

    double maximum = 0.1;
    if (!worker.ys.empty()) {
        maximum = std::max(
            maximum, *std::max_element(worker.ys.begin(), worker.ys.end()));
    }
    if (!army.ys.empty()) {
        maximum = std::max(
            maximum, *std::max_element(army.ys.begin(), army.ys.end()));
    }
    if (!ImPlot::BeginPlot(pair.title, ImVec2(-1.0f, trendPlotHeight),
                           ImPlotFlags_NoMouseText)) {
        return;
    }

    setupSessionXAxis(history.points.size());
    ImPlot::SetupAxis(
        ImAxis_Y1,
        sessionKpiUsesSeconds(pair.worker) ? "Seconds" : pair.title);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, maximum * 1.15,
                            ImPlotCond_Always);
    ImPlot::SetupFinish();

    auto* draw = ImPlot::GetPlotDrawList();
    const auto drawSeries = [&](const SessionKpiTrendSeries& series,
                                const ImVec4& color) {
        const ImU32 packed = ImGui::ColorConvertFloat4ToU32(color);
        for (std::size_t index = 0; index < series.xs.size(); ++index) {
            const ImVec2 point =
                ImPlot::PlotToPixels(series.xs[index], series.ys[index]);
            if (index > 0) {
                const ImVec2 previous = ImPlot::PlotToPixels(
                    series.xs[index - 1], series.ys[index - 1]);
                draw->AddLine(previous, point, packed, 2.0f);
            }
            draw->AddCircleFilled(point, 4.5f, packed, 16);
        }
    };
    ImPlot::PushPlotClipRect(5.0f);
    drawSeries(worker, workerColor);
    drawSeries(army, armyColor);
    ImPlot::PopPlotClipRect();

    if (ImPlot::IsPlotHovered()) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const SessionKpiTrendSeries* nearestSeries = nullptr;
        const char* nearestLabel = nullptr;
        std::size_t nearestIndex = 0;
        double nearestDistanceSquared = 64.0;
        const auto consider = [&](const SessionKpiTrendSeries& series,
                                  const char* label) {
            for (std::size_t index = 0; index < series.xs.size(); ++index) {
                const ImVec2 point =
                    ImPlot::PlotToPixels(series.xs[index], series.ys[index]);
                const double dx = static_cast<double>(point.x - mouse.x);
                const double dy = static_cast<double>(point.y - mouse.y);
                const double distanceSquared = dx * dx + dy * dy;
                if (distanceSquared <= nearestDistanceSquared) {
                    nearestDistanceSquared = distanceSquared;
                    nearestSeries = &series;
                    nearestLabel = label;
                    nearestIndex = index;
                }
            }
        };
        consider(worker, "Worker");
        consider(army, "Army");
        if (nearestSeries) {
            const auto& point =
                history.points[nearestSeries->sourceIndices[nearestIndex]];
            ImGui::BeginTooltip();
            ImGui::Text("Session: %s", point.sessionId.c_str());
            ImGui::Text("%s: %.2f%s", nearestLabel,
                        nearestSeries->ys[nearestIndex],
                        sessionKpiUsesSeconds(pair.worker) ? " s" : "");
            if (!point.machineReadable)
                ImGui::TextDisabled("Loaded from legacy text summary");
            ImGui::EndTooltip();
        }
    }
    ImPlot::EndPlot();
}

} // namespace

void drawSessionTrends(const std::filesystem::path& sessionsRoot,
                       const SessionReportVisibility& visibility,
                       SessionTrendsPresentation presentation) {
    static std::filesystem::path cachedRoot;
    static SessionTrendHistory history;
    static double refreshAt{};
    static std::string selectedMatchup{"All matchups"};

    const double now = ImGui::GetTime();
    if (cachedRoot != sessionsRoot || now >= refreshAt) {
        cachedRoot = sessionsRoot;
        history = loadSessionTrendHistory(sessionsRoot);
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

    if (presentation == SessionTrendsPresentation::Capture) {
        ImGui::Text("Matchup: %s", selectedMatchup.c_str());
    } else {
        ImGui::TextDisabled("Matchup");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("##TrendMatchup", selectedMatchup.c_str())) {
            if (ImGui::Selectable("All matchups",
                                  selectedMatchup == "All matchups"))
                selectedMatchup = "All matchups";
            for (const auto& matchup : matchups) {
                if (ImGui::Selectable(matchup.c_str(),
                                      selectedMatchup == matchup))
                    selectedMatchup = matchup;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh session history")) {
            history = loadSessionTrendHistory(sessionsRoot);
            refreshAt = now + 2.0;
        }
    }

    if (history.points.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No automatic session summaries were found yet.");
        return;
    }

    ImGui::Spacing();
    const auto drawUnpairedGroup = [&](SessionKpiGroup group) {
        forEachVisibleSessionKpi(
            visibility, group, [&](const auto& definition) {
                if (!sessionKpiUsesPairedPlot(definition.kpi))
                    drawTrendPlot(history, selectedMatchup, definition.kpi);
            });
    };

    if (hasVisibleSessionKpi(visibility, SessionKpiGroup::WorkerMacro) ||
        hasVisibleSessionKpi(visibility, SessionKpiGroup::ArmyMacro)) {
        ImGui::SeparatorText("Macro");
        drawUnpairedGroup(SessionKpiGroup::WorkerMacro);
        drawUnpairedGroup(SessionKpiGroup::ArmyMacro);
        for (const auto& pair : sessionKpiPairDefinitions)
            drawPairedTrendPlot(history, selectedMatchup, visibility, pair);
    }

    if (hasVisibleSessionKpi(visibility, SessionKpiGroup::ArmyManagement)) {
        ImGui::SeparatorText("Army Management");
        drawUnpairedGroup(SessionKpiGroup::ArmyManagement);
    }

    if (hasVisibleSessionKpi(visibility, SessionKpiGroup::Navigation) ||
        hasVisibleSessionKpi(visibility, SessionKpiGroup::Multitasking)) {
        ImGui::SeparatorText("Multitasking");
        drawUnpairedGroup(SessionKpiGroup::Navigation);
        drawUnpairedGroup(SessionKpiGroup::Multitasking);
    }
}

} // namespace smp
