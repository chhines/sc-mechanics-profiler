#pragma once

#include "app/game_analysis_visualization_model.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace smp::analysis_insights {

inline constexpr double cadencePlotHeight = 260.0;
inline constexpr double standardPlotHeight = 285.0;
inline constexpr double heatmapPlotHeight = 250.0;
inline constexpr double navigationBucketMs = 30000.0;
inline constexpr double multitaskingWindowMs = 5000.0;

inline std::string formatActiveTime(double seconds) {
    seconds = std::max(0.0, seconds);
    const auto whole = static_cast<long long>(seconds);
    const auto minutes = whole / 60;
    const auto remaining = whole % 60;
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%lld:%02lld", minutes, remaining);
    return buffer;
}

inline int timeAxisFormatter(double seconds, char* buffer, int size, void*) {
    const auto label = formatActiveTime(seconds);
    return std::snprintf(buffer, static_cast<std::size_t>(size), "%s",
                         label.c_str());
}

inline ImU32 seriesColor(int index) {
    return ImGui::ColorConvertFloat4ToU32(
        ImPlot::GetColormapColor(index, ImPlotColormap_Deep));
}

inline double percentile(std::vector<double> values, double probability) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const double position = probability * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = std::min(lower + 1, values.size() - 1);
    const double fraction = position - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

struct CadenceSummary {
    std::vector<double> xSeconds;
    std::vector<double> gapSeconds;
    std::optional<double> medianSeconds;
    std::optional<double> longestSeconds;
    std::optional<double> standardDeviationSeconds;
    std::optional<double> coefficientOfVariationPercent;
};

inline CadenceSummary cadenceSummary(
    const std::vector<TimelineMacroCycle>& cycles) {
    CadenceSummary summary;
    if (cycles.size() < 2)
        return summary;
    summary.xSeconds.reserve(cycles.size() - 1);
    summary.gapSeconds.reserve(cycles.size() - 1);
    for (std::size_t index = 1; index < cycles.size(); ++index) {
        const double gapMs =
            std::max(0.0, cycles[index].startActiveMs -
                              cycles[index - 1].startActiveMs);
        summary.xSeconds.push_back(cycles[index].startActiveMs / 1000.0);
        summary.gapSeconds.push_back(gapMs / 1000.0);
    }
    summary.medianSeconds = percentile(summary.gapSeconds, 0.50);
    summary.longestSeconds = *std::max_element(summary.gapSeconds.begin(),
                                               summary.gapSeconds.end());
    const double mean =
        std::accumulate(summary.gapSeconds.begin(), summary.gapSeconds.end(), 0.0) /
        static_cast<double>(summary.gapSeconds.size());
    double squared = 0.0;
    for (const double value : summary.gapSeconds) {
        const double delta = value - mean;
        squared += delta * delta;
    }
    summary.standardDeviationSeconds =
        std::sqrt(squared / static_cast<double>(summary.gapSeconds.size()));
    if (mean > 0.0)
        summary.coefficientOfVariationPercent =
            *summary.standardDeviationSeconds / mean * 100.0;
    return summary;
}

inline void drawCadenceStatsRow(const char* label,
                                const CadenceSummary& summary) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    const auto metric = [&](int column, const std::optional<double>& value,
                            const char* suffix) {
        ImGui::TableSetColumnIndex(column);
        if (value)
            ImGui::Text("%.2f%s", *value, suffix);
        else
            ImGui::TextDisabled("N/A");
    };
    metric(1, summary.medianSeconds, " s");
    metric(2, summary.longestSeconds, " s");
    metric(3, summary.standardDeviationSeconds, " s");
    metric(4, summary.coefficientOfVariationPercent, "%");
}

inline void drawPolyline(const std::vector<double>& xs,
                         const std::vector<double>& ys, ImU32 color,
                         bool connect = true) {
    auto* draw = ImPlot::GetPlotDrawList();
    for (std::size_t index = 0; index < xs.size() && index < ys.size(); ++index) {
        const ImVec2 point = ImPlot::PlotToPixels(xs[index], ys[index]);
        if (connect && index > 0) {
            const ImVec2 previous =
                ImPlot::PlotToPixels(xs[index - 1], ys[index - 1]);
            draw->AddLine(previous, point, color, 2.0f);
        }
        draw->AddCircleFilled(point, 4.0f, color, 16);
    }
}

inline void cadenceTooltip(const char* name, const CadenceSummary& summary) {
    if (!ImPlot::IsPlotHovered() || summary.xSeconds.empty())
        return;
    const auto mouse = ImPlot::GetPlotMousePos();
    std::size_t nearest = 0;
    double distance = std::abs(summary.xSeconds.front() - mouse.x);
    for (std::size_t index = 1; index < summary.xSeconds.size(); ++index) {
        const double candidate = std::abs(summary.xSeconds[index] - mouse.x);
        if (candidate < distance) {
            distance = candidate;
            nearest = index;
        }
    }
    const double visibleRange = std::max(1.0, ImPlot::GetPlotLimits().X.Size());
    if (distance > visibleRange * 0.02)
        return;
    ImGui::BeginTooltip();
    ImGui::Text("%s macro", name);
    ImGui::Text("Cycle start: %s",
                formatActiveTime(summary.xSeconds[nearest]).c_str());
    ImGui::Text("Since previous cycle: %.2f s",
                summary.gapSeconds[nearest]);
    ImGui::EndTooltip();
}

inline void drawMacroCadence(const GameAnalysisVisualizationModel& model) {
    const auto worker = cadenceSummary(model.workerMacroCycles);
    const auto army = cadenceSummary(model.armyMacroCycles);

    ImGui::TextWrapped(
        "Cadence is measured from one macro-cycle start to the next. Variability "
        "is the population standard deviation of those intervals; CV is the "
        "standard deviation divided by the mean interval.");
    if (ImGui::BeginTable("##MacroCadenceStats", 5,
                          ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Median interval");
        ImGui::TableSetupColumn("Longest gap");
        ImGui::TableSetupColumn("Variability");
        ImGui::TableSetupColumn("CV");
        ImGui::TableHeadersRow();
        drawCadenceStatsRow("Worker", worker);
        drawCadenceStatsRow("Army", army);
        ImGui::EndTable();
    }

    const bool any = !worker.gapSeconds.empty() || !army.gapSeconds.empty();
    if (!any) {
        ImGui::TextDisabled(
            "At least two worker or army macro cycles are required for cadence.");
        return;
    }
    const double gameSeconds = std::max(1.0, model.activeDurationMs / 1000.0);
    double maximumGap = 1.0;
    for (const double value : worker.gapSeconds)
        maximumGap = std::max(maximumGap, value);
    for (const double value : army.gapSeconds)
        maximumGap = std::max(maximumGap, value);

    if (ImPlot::BeginPlot("Macro cadence over time",
                           ImVec2(-1.0f, static_cast<float>(cadencePlotHeight)),
                           ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, "Later cycle start");
        ImPlot::SetupAxisFormat(ImAxis_X1, timeAxisFormatter);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, gameSeconds,
                                ImPlotCond_Always);
        ImPlot::SetupAxis(ImAxis_Y1, "Seconds since previous cycle");
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, maximumGap * 1.15,
                                ImPlotCond_Always);
        ImPlot::SetupFinish();
        ImPlot::PushPlotClipRect();
        if (!worker.gapSeconds.empty())
            drawPolyline(worker.xSeconds, worker.gapSeconds, seriesColor(0), false);
        if (!army.gapSeconds.empty())
            drawPolyline(army.xSeconds, army.gapSeconds, seriesColor(1), false);
        ImPlot::PopPlotClipRect();
        cadenceTooltip("Worker", worker);
        cadenceTooltip("Army", army);
        ImPlot::EndPlot();
    }
    ImGui::TextDisabled("Series: Worker = first palette color; Army = second palette color.");
}

inline void drawMacroDurationVsSize(
    const GameAnalysisVisualizationModel& model) {
    const bool any = !model.workerMacroCycles.empty() ||
                     !model.armyMacroCycles.empty();
    if (!any) {
        ImGui::TextDisabled("No macro cycles are available for this plot.");
        return;
    }
    std::size_t maximumVisits = 1;
    double maximumSeconds = 0.1;
    const auto inspect = [&](const std::vector<TimelineMacroCycle>& cycles) {
        for (const auto& cycle : cycles) {
            maximumVisits = std::max(maximumVisits, cycle.visitCount);
            maximumSeconds = std::max(maximumSeconds, cycle.durationMs / 1000.0);
        }
    };
    inspect(model.workerMacroCycles);
    inspect(model.armyMacroCycles);

    if (ImPlot::BeginPlot("Macro duration vs cycle size",
                           ImVec2(-1.0f, static_cast<float>(standardPlotHeight)),
                           ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, "Production visits in cycle");
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.5,
                                static_cast<double>(maximumVisits) + 0.5,
                                ImPlotCond_Always);
        ImPlot::SetupAxis(ImAxis_Y1, "Macro cycle duration (s)");
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, maximumSeconds * 1.15,
                                ImPlotCond_Always);
        ImPlot::SetupFinish();
        auto* draw = ImPlot::GetPlotDrawList();
        ImPlot::PushPlotClipRect();
        const auto points = [&](const std::vector<TimelineMacroCycle>& cycles,
                                ImU32 color) {
            for (const auto& cycle : cycles) {
                const ImVec2 point = ImPlot::PlotToPixels(
                    static_cast<double>(cycle.visitCount),
                    cycle.durationMs / 1000.0);
                draw->AddCircleFilled(point, 4.5f, color, 16);
            }
        };
        points(model.workerMacroCycles, seriesColor(0));
        points(model.armyMacroCycles, seriesColor(1));
        ImPlot::PopPlotClipRect();

        if (ImPlot::IsPlotHovered()) {
            const auto mouse = ImPlot::GetPlotMousePos();
            const auto tooltipNearest = [&](const char* label,
                                            const std::vector<TimelineMacroCycle>& cycles) {
                const TimelineMacroCycle* nearest = nullptr;
                double best = 1e9;
                for (const auto& cycle : cycles) {
                    const double dx = static_cast<double>(cycle.visitCount) - mouse.x;
                    const double dy = cycle.durationMs / 1000.0 - mouse.y;
                    const double scaled = dx * dx + dy * dy;
                    if (scaled < best) {
                        best = scaled;
                        nearest = &cycle;
                    }
                }
                if (!nearest || best > 0.20)
                    return false;
                ImGui::BeginTooltip();
                ImGui::Text("%s macro", label);
                ImGui::Text("Start: %s",
                            formatActiveTime(nearest->startActiveMs / 1000.0).c_str());
                ImGui::Text("Production visits: %zu", nearest->visitCount);
                ImGui::Text("Duration: %.2f s", nearest->durationMs / 1000.0);
                ImGui::EndTooltip();
                return true;
            };
            if (!tooltipNearest("Worker", model.workerMacroCycles))
                (void)tooltipNearest("Army", model.armyMacroCycles);
        }
        ImPlot::EndPlot();
    }
    ImGui::TextDisabled("Series: Worker = first palette color; Army = second palette color.");
}

struct NavigationBucketSeries {
    std::vector<double> xSeconds;
    std::vector<double> ratePerMinute;
};

inline NavigationBucketSeries navigationRates(
    const GameAnalysisVisualizationModel& model) {
    NavigationBucketSeries series;
    const double durationMs = std::max(0.0, model.activeDurationMs);
    if (durationMs <= 0.0)
        return series;
    const auto bucketCount = static_cast<std::size_t>(
        std::max(1.0, std::ceil(durationMs / navigationBucketMs)));
    std::vector<int> counts(bucketCount, 0);
    for (const auto& event : model.navigationEvents) {
        const auto index = std::min(
            bucketCount - 1,
            static_cast<std::size_t>(std::max(0.0, event.activeMs) /
                                     navigationBucketMs));
        ++counts[index];
    }
    series.xSeconds.reserve(bucketCount);
    series.ratePerMinute.reserve(bucketCount);
    for (std::size_t index = 0; index < bucketCount; ++index) {
        const double startMs = static_cast<double>(index) * navigationBucketMs;
        const double midpointMs =
            std::min(durationMs, startMs + navigationBucketMs * 0.5);
        series.xSeconds.push_back(midpointMs / 1000.0);
        // Every bucket is presented as a 30-second equivalent rate. The final
        // partial bucket uses the same denominator so a few closing seconds do
        // not create an artificial spike.
        series.ratePerMinute.push_back(static_cast<double>(counts[index]) * 2.0);
    }
    return series;
}

inline void drawNavigationRate(const GameAnalysisVisualizationModel& model) {
    if (!model.navigationStatus.available) {
        ImGui::TextDisabled("Navigation rate unavailable: %s",
                            model.navigationStatus.reason.c_str());
        return;
    }
    const auto series = navigationRates(model);
    if (series.xSeconds.empty()) {
        ImGui::TextDisabled("No active game duration is available.");
        return;
    }
    const double maximum = std::max(
        1.0, *std::max_element(series.ratePerMinute.begin(),
                               series.ratePerMinute.end()));
    const double gameSeconds = std::max(1.0, model.activeDurationMs / 1000.0);
    if (ImPlot::BeginPlot("Navigation transition rate",
                           ImVec2(-1.0f, static_cast<float>(standardPlotHeight)),
                           ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, "Active game time");
        ImPlot::SetupAxisFormat(ImAxis_X1, timeAxisFormatter);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, gameSeconds,
                                ImPlotCond_Always);
        ImPlot::SetupAxis(ImAxis_Y1, "Transitions / minute");
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, maximum * 1.15,
                                ImPlotCond_Always);
        ImPlot::SetupFinish();
        ImPlot::PushPlotClipRect();
        drawPolyline(series.xSeconds, series.ratePerMinute, seriesColor(2));
        ImPlot::PopPlotClipRect();
        if (ImPlot::IsPlotHovered()) {
            const auto mouse = ImPlot::GetPlotMousePos();
            std::size_t nearest = 0;
            double best = std::abs(series.xSeconds.front() - mouse.x);
            for (std::size_t index = 1; index < series.xSeconds.size(); ++index) {
                const double candidate =
                    std::abs(series.xSeconds[index] - mouse.x);
                if (candidate < best) {
                    best = candidate;
                    nearest = index;
                }
            }
            if (best <= 15.0) {
                ImGui::BeginTooltip();
                ImGui::Text("30-second bucket near %s",
                            formatActiveTime(series.xSeconds[nearest]).c_str());
                ImGui::Text("Equivalent rate: %.1f transitions/min",
                            series.ratePerMinute[nearest]);
                ImGui::EndTooltip();
            }
        }
        ImPlot::EndPlot();
    }
    ImGui::TextDisabled(
        "30-second buckets are scaled to a per-minute equivalent; the final "
        "partial bucket keeps the 30-second denominator to avoid an end-game spike.");
}

struct MultitaskingWindows {
    std::array<std::vector<int>, 5> counts;
    std::vector<int> diversity;
    std::optional<double> averageActiveDiversity;
    int peakDiversity{};
    std::size_t activeWindows{};
};

inline MultitaskingWindows multitaskingWindows(
    const GameAnalysisVisualizationModel& model) {
    MultitaskingWindows result;
    const double durationMs = std::max(0.0, model.activeDurationMs);
    if (durationMs <= 0.0)
        return result;
    const auto windowCount = static_cast<std::size_t>(
        std::max(1.0, std::ceil(durationMs / multitaskingWindowMs)));
    for (auto& row : result.counts)
        row.assign(windowCount, 0);
    result.diversity.assign(windowCount, 0);

    const auto add = [&](std::size_t row, double activeMs) {
        if (activeMs < 0.0 || row >= result.counts.size())
            return;
        const auto index = std::min(
            windowCount - 1,
            static_cast<std::size_t>(activeMs / multitaskingWindowMs));
        ++result.counts[row][index];
    };
    for (const auto& event : model.navigationEvents)
        add(0, event.activeMs);
    for (const auto& cycle : model.workerMacroCycles)
        add(1, cycle.startActiveMs);
    for (const auto& cycle : model.armyMacroCycles)
        add(2, cycle.startActiveMs);
    for (const auto& edit : model.armyControlGroupEdits)
        add(3, edit.operationActiveMs);
    for (const auto& scout : model.scoutingActivities) {
        for (const double command : scout.commandActiveMs)
            add(4, command);
    }

    double totalDiversity = 0.0;
    for (std::size_t index = 0; index < windowCount; ++index) {
        int diversity = 0;
        for (const auto& row : result.counts) {
            if (row[index] > 0)
                ++diversity;
        }
        result.diversity[index] = diversity;
        result.peakDiversity = std::max(result.peakDiversity, diversity);
        if (diversity > 0) {
            ++result.activeWindows;
            totalDiversity += static_cast<double>(diversity);
        }
    }
    if (result.activeWindows > 0)
        result.averageActiveDiversity =
            totalDiversity / static_cast<double>(result.activeWindows);
    return result;
}

inline void drawMultitaskingDensity(const GameAnalysisVisualizationModel& model) {
    const auto windows = multitaskingWindows(model);
    if (windows.diversity.empty()) {
        ImGui::TextDisabled("No active game duration is available.");
        return;
    }
    ImGui::Text("Average mechanic types / active 5-second window: ");
    ImGui::SameLine();
    if (windows.averageActiveDiversity)
        ImGui::Text("%.2f", *windows.averageActiveDiversity);
    else
        ImGui::TextDisabled("N/A");
    ImGui::SameLine(0.0f, 28.0f);
    ImGui::Text("Peak mechanic types in one window: %d", windows.peakDiversity);
    ImGui::TextDisabled(
        "An active window contains at least one tracked mechanic. A class counts "
        "once toward diversity even if it contains several actions in the window.");

    const double gameSeconds = std::max(1.0, model.activeDurationMs / 1000.0);
    const double windowSeconds = multitaskingWindowMs / 1000.0;
    constexpr std::array<const char*, 5> labels{
        "Camera", "Worker macro", "Army macro", "CG edit", "Scout command"};
    constexpr std::array<double, 5> ticks{4.0, 3.0, 2.0, 1.0, 0.0};
    if (ImPlot::BeginPlot("Mechanics activity heatmap",
                           ImVec2(-1.0f, static_cast<float>(heatmapPlotHeight)),
                           ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, "Active game time");
        ImPlot::SetupAxisFormat(ImAxis_X1, timeAxisFormatter);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, gameSeconds,
                                ImPlotCond_Always);
        ImPlot::SetupAxis(ImAxis_Y1, nullptr,
                          ImPlotAxisFlags_NoGridLines |
                              ImPlotAxisFlags_NoTickMarks |
                              ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -0.5, 4.5, ImPlotCond_Always);
        ImPlot::SetupAxisTicks(ImAxis_Y1, ticks.data(),
                               static_cast<int>(ticks.size()), labels.data(), false);
        ImPlot::SetupFinish();

        auto* draw = ImPlot::GetPlotDrawList();
        ImPlot::PushPlotClipRect();
        for (std::size_t row = 0; row < windows.counts.size(); ++row) {
            const double y = 4.0 - static_cast<double>(row);
            const ImVec4 base = ImPlot::GetColormapColor(
                static_cast<int>(row), ImPlotColormap_Deep);
            for (std::size_t column = 0; column < windows.counts[row].size();
                 ++column) {
                const int count = windows.counts[row][column];
                if (count <= 0)
                    continue;
                ImVec4 shaded = base;
                shaded.w = std::min(1.0f, 0.32f + 0.20f * static_cast<float>(count));
                const double x0 = static_cast<double>(column) * windowSeconds;
                const double x1 = std::min(gameSeconds, x0 + windowSeconds);
                const ImVec2 first = ImPlot::PlotToPixels(x0, y - 0.38);
                const ImVec2 second = ImPlot::PlotToPixels(x1, y + 0.38);
                draw->AddRectFilled(
                    ImVec2(std::min(first.x, second.x), std::min(first.y, second.y)),
                    ImVec2(std::max(first.x, second.x), std::max(first.y, second.y)),
                    ImGui::ColorConvertFloat4ToU32(shaded), 1.0f);
            }
        }
        ImPlot::PopPlotClipRect();

        if (ImPlot::IsPlotHovered()) {
            const auto mouse = ImPlot::GetPlotMousePos();
            if (mouse.x >= 0.0 && mouse.x <= gameSeconds &&
                mouse.y >= -0.5 && mouse.y <= 4.5) {
                const auto column = std::min(
                    windows.diversity.size() - 1,
                    static_cast<std::size_t>(mouse.x / windowSeconds));
                ImGui::BeginTooltip();
                ImGui::Text("Window: %s - %s",
                            formatActiveTime(column * windowSeconds).c_str(),
                            formatActiveTime(std::min(gameSeconds,
                                (column + 1) * windowSeconds)).c_str());
                ImGui::Text("Mechanic types: %d", windows.diversity[column]);
                for (std::size_t row = 0; row < windows.counts.size(); ++row)
                    ImGui::Text("%s: %d", labels[row], windows.counts[row][column]);
                ImGui::EndTooltip();
            }
        }
        ImPlot::EndPlot();
    }
    if (!model.scoutingOutcomeDataAvailable)
        ImGui::TextDisabled(
            "Scout-command heatmap detail requires a game analyzed with the "
            "current scouting telemetry; other rows remain valid for older games.");
}

inline std::array<int, 5> durationBins(
    const std::vector<TimelineMacroCycle>& cycles) {
    std::array<int, 5> bins{};
    for (const auto& cycle : cycles) {
        const double seconds = cycle.durationMs / 1000.0;
        std::size_t index = 4;
        if (seconds < 1.0)
            index = 0;
        else if (seconds < 2.0)
            index = 1;
        else if (seconds < 3.0)
            index = 2;
        else if (seconds < 4.0)
            index = 3;
        ++bins[index];
    }
    return bins;
}

inline void drawDurationDistributionPlot(
    const char* title, const std::vector<TimelineMacroCycle>& cycles,
    int paletteIndex) {
    if (cycles.empty()) {
        ImGui::TextDisabled("%s: no macro cycles available.", title);
        return;
    }
    const auto bins = durationBins(cycles);
    const int maximum = std::max(1, *std::max_element(bins.begin(), bins.end()));
    constexpr std::array<double, 5> ticks{0, 1, 2, 3, 4};
    constexpr std::array<const char*, 5> labels{
        "0-1 s", "1-2 s", "2-3 s", "3-4 s", "4+ s"};
    if (ImPlot::BeginPlot(title,
                           ImVec2(-1.0f, static_cast<float>(standardPlotHeight)),
                           ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, "Cycle count");
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0,
                                static_cast<double>(maximum) * 1.25,
                                ImPlotCond_Always);
        ImPlot::SetupAxis(ImAxis_Y1, nullptr,
                          ImPlotAxisFlags_NoGridLines |
                              ImPlotAxisFlags_NoTickMarks |
                              ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -0.6, 4.6, ImPlotCond_Always);
        ImPlot::SetupAxisTicks(ImAxis_Y1, ticks.data(), 5, labels.data(), false);
        ImPlot::SetupFinish();
        auto* draw = ImPlot::GetPlotDrawList();
        ImPlot::PushPlotClipRect();
        const ImU32 color = seriesColor(paletteIndex);
        for (std::size_t index = 0; index < bins.size(); ++index) {
            const ImVec2 first = ImPlot::PlotToPixels(0.0,
                                                      static_cast<double>(index) - 0.30);
            const ImVec2 second = ImPlot::PlotToPixels(
                static_cast<double>(bins[index]),
                static_cast<double>(index) + 0.30);
            draw->AddRectFilled(
                ImVec2(std::min(first.x, second.x), std::min(first.y, second.y)),
                ImVec2(std::max(first.x, second.x), std::max(first.y, second.y)),
                color, 2.0f);
        }
        ImPlot::PopPlotClipRect();
        ImPlot::EndPlot();
    }
}

inline void drawScoutingAnalysis(const GameAnalysisVisualizationModel& model) {
    if (!model.scoutingStatus.available) {
        ImGui::TextDisabled("Scouting analysis unavailable: %s",
                            model.scoutingStatus.reason.c_str());
        return;
    }
    std::optional<double> longestGapMs;
    std::size_t returnedHome = 0;
    std::size_t noObservedReturn = 0;
    std::size_t resumed = 0;
    for (const auto& activity : model.scoutingActivities) {
        if (activity.longestCommandGapMs &&
            (!longestGapMs || *activity.longestCommandGapMs > *longestGapMs))
            longestGapMs = activity.longestCommandGapMs;
        if (activity.outcomeAvailable) {
            if (activity.returnedHome)
                ++returnedHome;
            else
                ++noObservedReturn;
            if (activity.resumedAfterTemporaryReturn)
                ++resumed;
        }
    }

    if (ImGui::BeginTable("##ScoutingSummary", 2,
                          ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp)) {
        const auto row = [](const char* label, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(value.c_str());
        };
        row("Confirmed scouts", std::to_string(model.scoutingActivities.size()));
        if (model.scoutingOutcomeDataAvailable) {
            row("Scouting candidates", std::to_string(model.scoutingCandidateCount));
            row("Scouting never confirmed",
                std::to_string(model.unconfirmedScoutingCandidateCount));
        }
        if (longestGapMs) {
            char buffer[64]{};
            std::snprintf(buffer, sizeof(buffer), "%.2f s", *longestGapMs / 1000.0);
            row("Longest scout command gap", buffer);
        } else {
            row("Longest scout command gap", "N/A");
        }
        ImGui::EndTable();
    }

    if (!model.scoutingOutcomeDataAvailable) {
        ImGui::TextDisabled(
            "Detailed scouting outcomes require a game analyzed with the current build.");
        return;
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Scouting outcomes");
    ImGui::BulletText("Returned home: %zu", returnedHome);
    ImGui::BulletText("No observed return: %zu", noObservedReturn);
    ImGui::BulletText("Resumed scouting after temporary return: %zu", resumed);
    ImGui::BulletText("Scouting never confirmed: %zu",
                      model.unconfirmedScoutingCandidateCount);
    ImGui::TextDisabled(
        "Resumed after temporary return is supplemental and can overlap the final "
        "Returned home / No observed return outcome. No observed return is not a "
        "death inference.");
}

inline void drawAnalysisInsights(const GameAnalysisVisualizationModel& model) {
    ImGui::SeparatorText("Macro cadence");
    drawMacroCadence(model);

    ImGui::SeparatorText("Macro duration vs cycle size");
    drawMacroDurationVsSize(model);

    ImGui::SeparatorText("Navigation transition rate over time");
    drawNavigationRate(model);

    ImGui::SeparatorText("Multitasking density");
    drawMultitaskingDensity(model);

    ImGui::SeparatorText("Scouting analysis");
    drawScoutingAnalysis(model);

    ImGui::SeparatorText("Macro-duration distribution");
    drawDurationDistributionPlot("Worker macro-duration distribution",
                                 model.workerMacroCycles, 0);
    drawDurationDistributionPlot("Army macro-duration distribution",
                                 model.armyMacroCycles, 1);
}

} // namespace smp::analysis_insights
