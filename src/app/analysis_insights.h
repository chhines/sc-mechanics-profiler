#pragma once

#include "app/analysis_macro_gap.h"
#include "app/analysis_navigation_rate.h"
#include "app/game_analysis_visualization_model.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace smp::analysis_insights {

inline constexpr double macroGapTimelineHeight = 230.0;
inline constexpr double macroGapHistogramHeight = 260.0;
inline constexpr double macroGapTimelineHalfHeight = 0.15;
inline constexpr double standardPlotHeight = 285.0;
inline constexpr double heatmapPlotHeight = 250.0;
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

inline std::string formatActiveTimePrecise(double seconds) {
    seconds = std::max(0.0, seconds);
    const auto minutes = static_cast<long long>(seconds / 60.0);
    const double remaining = seconds - static_cast<double>(minutes * 60);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%lld:%06.3f", minutes, remaining);
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

inline void drawSeriesLegendItem(const char* label, int paletteIndex) {
    ImGui::Dummy(ImVec2(12.0f, 12.0f));
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
        seriesColor(paletteIndex), 2.0f);
    ImGui::SameLine(0.0f, 5.0f);
    ImGui::TextDisabled("%s", label);
}

inline void drawWorkerArmySeriesLegend() {
    ImGui::TextDisabled("Series:");
    ImGui::SameLine();
    drawSeriesLegendItem("Worker", 0);
    ImGui::SameLine(0.0f, 16.0f);
    drawSeriesLegendItem("Army", 1);
}

inline ImU32 macroGapBandColor(MacroGapBand band) {
    switch (band) {
    case MacroGapBand::UnderTenSeconds:
        return IM_COL32(118, 132, 148, 80);
    case MacroGapBand::TenToTwentySeconds:
        return IM_COL32(210, 154, 55, 205);
    case MacroGapBand::OverTwentySeconds:
        return IM_COL32(196, 76, 76, 225);
    }
    return IM_COL32(118, 132, 148, 80);
}

inline void drawMacroGapStatsRow(const char* label,
                                 const MacroGapSummary& summary) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    const auto secondsMetric = [&](int column,
                                   const std::optional<double>& value) {
        ImGui::TableSetColumnIndex(column);
        if (value)
            ImGui::Text("%.2f s", *value);
        else
            ImGui::TextDisabled("N/A");
    };

    ImGui::TableSetColumnIndex(1);
    if (summary.cyclesPerMinute)
        ImGui::Text("%.2f", *summary.cyclesPerMinute);
    else
        ImGui::TextDisabled("N/A");
    secondsMetric(2, summary.medianSeconds);
    secondsMetric(3, summary.p90Seconds);
    secondsMetric(4, summary.longestSeconds);
    ImGui::TableSetColumnIndex(5);
    if (summary.gaps.empty())
        ImGui::TextDisabled("N/A");
    else
        ImGui::Text("%zu", summary.overTenSeconds);
    ImGui::TableSetColumnIndex(6);
    if (summary.gaps.empty())
        ImGui::TextDisabled("N/A");
    else
        ImGui::Text("%zu", summary.overTwentySeconds);
}

inline void drawMacroGapTimelineSeries(const char* label,
                                       const MacroGapSummary& summary,
                                       double y, bool& tooltipShown) {
    auto* draw = ImPlot::GetPlotDrawList();
    for (const auto& gap : summary.gaps) {
        if (gap.durationSeconds <= 0.0)
            continue;
        const auto band = macroGapBand(gap.durationSeconds);
        const ImVec2 first =
            ImPlot::PlotToPixels(gap.startSeconds,
                                 y - macroGapTimelineHalfHeight);
        const ImVec2 second =
            ImPlot::PlotToPixels(gap.endSeconds,
                                 y + macroGapTimelineHalfHeight);
        const ImVec2 minimum{std::min(first.x, second.x),
                             std::min(first.y, second.y)};
        const ImVec2 maximum{std::max(first.x, second.x),
                             std::max(first.y, second.y)};
        draw->AddRectFilled(minimum, maximum, macroGapBandColor(band), 2.0f);
        if (!tooltipShown && ImPlot::IsPlotHovered() &&
            ImGui::IsMouseHoveringRect(minimum, maximum)) {
            ImGui::BeginTooltip();
            ImGui::Text("%s Macro Gap", label);
            ImGui::Text("Previous cycle end: %s",
                        formatActiveTimePrecise(gap.startSeconds).c_str());
            ImGui::Text("Next cycle start: %s",
                        formatActiveTimePrecise(gap.endSeconds).c_str());
            ImGui::Text("Gap duration: %.2f s", gap.durationSeconds);
            ImGui::EndTooltip();
            tooltipShown = true;
        }
    }
}

inline void drawMacroGapLengthLegend() {
    ImGui::TextDisabled("Gap length:");
    const auto item = [](const char* label, MacroGapBand band) {
        ImVec4 color =
            ImGui::ColorConvertU32ToFloat4(macroGapBandColor(band));
        color.w = 1.0f;
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextColored(color, "%s", label);
    };
    item("<10 s", MacroGapBand::UnderTenSeconds);
    item("10-20 s", MacroGapBand::TenToTwentySeconds);
    item(">20 s", MacroGapBand::OverTwentySeconds);
}

inline void drawMacroGapTimeline(const MacroGapSummary& worker,
                                 const MacroGapSummary& army,
                                 double gameSeconds) {
    constexpr std::array<double, 2> ticks{0.0, 1.0};
    constexpr std::array<const char*, 2> labels{"Army macro", "Worker macro"};
    if (ImPlot::BeginPlot(
            "Macro Gap Timeline",
            ImVec2(-1.0f, static_cast<float>(macroGapTimelineHeight)),
            ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, "Active game time");
        ImPlot::SetupAxisFormat(ImAxis_X1, timeAxisFormatter);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, gameSeconds,
                                ImPlotCond_Always);
        ImPlot::SetupAxis(
            ImAxis_Y1, nullptr,
            ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks |
                ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -0.55, 1.55, ImPlotCond_Always);
        ImPlot::SetupAxisTicks(ImAxis_Y1, ticks.data(),
                               static_cast<int>(ticks.size()), labels.data(),
                               false);
        ImPlot::SetupFinish();
        ImPlot::PushPlotClipRect();
        bool tooltipShown = false;
        drawMacroGapTimelineSeries("Worker", worker, 1.0, tooltipShown);
        drawMacroGapTimelineSeries("Army", army, 0.0, tooltipShown);
        ImPlot::PopPlotClipRect();
        ImPlot::EndPlot();
    }
    drawMacroGapLengthLegend();
}

inline void drawMacroGapHistogramSeries(
    const char* label, const MacroGapSummary& summary, double offset,
    ImU32 color, bool& tooltipShown) {
    constexpr std::array<const char*, 5> bucketLabels{
        "0-5 s", "5-10 s", "10-15 s", "15-20 s", ">20 s"};
    auto* draw = ImPlot::GetPlotDrawList();
    for (std::size_t index = 0; index < summary.histogram.size(); ++index) {
        const auto count = summary.histogram[index];
        if (count == 0)
            continue;
        const double center = static_cast<double>(index) + offset;
        const ImVec2 first = ImPlot::PlotToPixels(center - 0.15, 0.0);
        const ImVec2 second = ImPlot::PlotToPixels(
            center + 0.15, static_cast<double>(count));
        const ImVec2 minimum{std::min(first.x, second.x),
                             std::min(first.y, second.y)};
        const ImVec2 maximum{std::max(first.x, second.x),
                             std::max(first.y, second.y)};
        draw->AddRectFilled(minimum, maximum, color, 2.0f);
        if (!tooltipShown && ImPlot::IsPlotHovered() &&
            ImGui::IsMouseHoveringRect(minimum, maximum)) {
            ImGui::BeginTooltip();
            ImGui::Text("%s Macro Gaps", label);
            ImGui::Text("Length: %s", bucketLabels[index]);
            ImGui::Text("Count: %zu", count);
            ImGui::EndTooltip();
            tooltipShown = true;
        }
    }
}

inline void drawMacroGapHistogram(const MacroGapSummary& worker,
                                  const MacroGapSummary& army) {
    constexpr std::array<double, 5> ticks{0.0, 1.0, 2.0, 3.0, 4.0};
    constexpr std::array<const char*, 5> labels{
        "0-5 s", "5-10 s", "10-15 s", "15-20 s", ">20 s"};
    std::size_t maximumCount = 1;
    for (const auto count : worker.histogram)
        maximumCount = std::max(maximumCount, count);
    for (const auto count : army.histogram)
        maximumCount = std::max(maximumCount, count);
    const double yMaximum = static_cast<double>(maximumCount) * 1.25;

    if (ImPlot::BeginPlot(
            "Gap Length Histogram",
            ImVec2(-1.0f, static_cast<float>(macroGapHistogramHeight)),
            ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, "Macro Gap length",
                          ImPlotAxisFlags_NoGridLines |
                              ImPlotAxisFlags_NoTickMarks |
                              ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_X1, -0.6, 4.6, ImPlotCond_Always);
        ImPlot::SetupAxisTicks(ImAxis_X1, ticks.data(),
                               static_cast<int>(ticks.size()), labels.data(),
                               false);
        ImPlot::SetupAxis(ImAxis_Y1, "Gap count");
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, yMaximum,
                                ImPlotCond_Always);
        ImPlot::SetupFinish();

        auto* draw = ImPlot::GetPlotDrawList();
        ImPlot::PushPlotClipRect();
        draw->AddLine(ImPlot::PlotToPixels(1.5, 0.0),
                      ImPlot::PlotToPixels(1.5, yMaximum),
                      macroGapBandColor(MacroGapBand::TenToTwentySeconds),
                      1.0f);
        draw->AddLine(ImPlot::PlotToPixels(3.5, 0.0),
                      ImPlot::PlotToPixels(3.5, yMaximum),
                      macroGapBandColor(MacroGapBand::OverTwentySeconds),
                      1.0f);
        bool tooltipShown = false;
        drawMacroGapHistogramSeries("Worker", worker, -0.17, seriesColor(0),
                                    tooltipShown);
        drawMacroGapHistogramSeries("Army", army, 0.17, seriesColor(1),
                                    tooltipShown);
        ImPlot::PopPlotClipRect();
        ImPlot::EndPlot();
    }
    drawWorkerArmySeriesLegend();
}

inline void drawMacroCadence(const GameAnalysisVisualizationModel& model) {
    const auto worker =
        macroGapSummary(model.workerMacroCycles, model.activeDurationMs);
    const auto army =
        macroGapSummary(model.armyMacroCycles, model.activeDurationMs);

    ImGui::TextWrapped(
        "Macro Gap = end of one macro cycle -> start of the next macro cycle. "
        "It measures how long the player went without returning to that macro "
        "task; Worker and Army gaps are calculated independently.");
    if (ImGui::BeginTable("##MacroCadenceStats", 7,
                          ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Cycles/min");
        ImGui::TableSetupColumn("Median gap");
        ImGui::TableSetupColumn("P90 gap");
        ImGui::TableSetupColumn("Longest gap");
        ImGui::TableSetupColumn("Gaps >10 s");
        ImGui::TableSetupColumn("Gaps >20 s");
        ImGui::TableHeadersRow();
        drawMacroGapStatsRow("Worker", worker);
        drawMacroGapStatsRow("Army", army);
        ImGui::EndTable();
    }

    if (worker.gaps.empty() && army.gaps.empty()) {
        ImGui::TextDisabled(
            "At least two completed Worker or Army macro cycles are required "
            "to calculate Macro Gaps.");
        return;
    }

    double gameSeconds = std::max(1.0, model.activeDurationMs / 1000.0);
    for (const auto& gap : worker.gaps)
        gameSeconds = std::max(gameSeconds, gap.endSeconds);
    for (const auto& gap : army.gaps)
        gameSeconds = std::max(gameSeconds, gap.endSeconds);
    drawMacroGapTimeline(worker, army, gameSeconds);
    drawMacroGapHistogram(worker, army);
}

inline void drawNavigationRate(const GameAnalysisVisualizationModel& model) {
    if (!model.navigationStatus.available) {
        ImGui::TextDisabled("Navigation rate unavailable: %s",
                            model.navigationStatus.reason.c_str());
        return;
    }
    const auto series = navigationRates(model);
    if (series.buckets.empty()) {
        ImGui::TextDisabled("No active game duration is available.");
        return;
    }
    double maximum = 1.0;
    for (const auto& bucket : series.buckets)
        maximum = std::max(maximum, bucket.ratePerMinute);
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
        auto* draw = ImPlot::GetPlotDrawList();
        const ImU32 color = seriesColor(2);
        for (std::size_t index = 0; index < series.buckets.size(); ++index) {
            const auto& bucket = series.buckets[index];
            draw->AddLine(
                ImPlot::PlotToPixels(bucket.startSeconds,
                                     bucket.ratePerMinute),
                ImPlot::PlotToPixels(bucket.endSeconds,
                                     bucket.ratePerMinute),
                color, 2.0f);
            if (index + 1 < series.buckets.size()) {
                const auto& next = series.buckets[index + 1];
                draw->AddLine(
                    ImPlot::PlotToPixels(bucket.endSeconds,
                                         bucket.ratePerMinute),
                    ImPlot::PlotToPixels(bucket.endSeconds,
                                         next.ratePerMinute),
                    color, 2.0f);
            }
        }
        ImPlot::PopPlotClipRect();
        if (ImPlot::IsPlotHovered()) {
            const auto mouse = ImPlot::GetPlotMousePos();
            if (const auto* bucket = navigationBucketAt(series, mouse.x)) {
                ImGui::BeginTooltip();
                ImGui::Text("Bucket: %s - %s",
                            formatActiveTime(bucket->startSeconds).c_str(),
                            formatActiveTime(bucket->endSeconds).c_str());
                ImGui::Text("Equivalent rate: %.1f transitions/min",
                            bucket->ratePerMinute);
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
        ImGui::TextDisabled("Scouting activity unavailable: %s",
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
            "Observed scouting outcomes and detection details require a game "
            "analyzed with the current scouting telemetry.");
        return;
    }
    ImGui::Spacing();
    if (ImGui::TreeNode("Observed scouting outcomes")) {
        ImGui::BulletText("Returned home: %zu", returnedHome);
        ImGui::BulletText("No observed return: %zu", noObservedReturn);
        ImGui::BulletText("Resumed scouting after temporary return: %zu",
                          resumed);
        ImGui::TextDisabled(
            "Resumed after temporary return is supplemental and can overlap the "
            "final Returned home / No observed return outcome. No observed "
            "return is not a death inference.");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Detection details")) {
        ImGui::TextDisabled(
            "Detector/classifier diagnostics; these values do not measure player "
            "performance.");
        ImGui::BulletText("Scouting candidates: %zu",
                          model.scoutingCandidateCount);
        ImGui::BulletText("Scouting never confirmed: %zu",
                          model.unconfirmedScoutingCandidateCount);
        ImGui::TreePop();
    }
}

inline void drawAnalysisInsights(const GameAnalysisVisualizationModel& model) {
    ImGui::SeparatorText("Macro cadence");
    drawMacroCadence(model);

    ImGui::SeparatorText("Navigation transition rate over time");
    drawNavigationRate(model);

    ImGui::SeparatorText("Multitasking density");
    drawMultitaskingDensity(model);

    ImGui::SeparatorText("Scouting activity");
    drawScoutingAnalysis(model);

    ImGui::SeparatorText("Macro-duration distribution");
    drawDurationDistributionPlot("Worker macro-duration distribution",
                                 model.workerMacroCycles, 0);
    drawDurationDistributionPlot("Army macro-duration distribution",
                                 model.armyMacroCycles, 1);
}

} // namespace smp::analysis_insights
