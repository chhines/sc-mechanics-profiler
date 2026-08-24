#include "app/analysis_view.h"
#include "app/analysis_ability_activity.h"
#include "app/analysis_insights.h"
#include "app/analysis_timeline_layout.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace smp {
namespace {

constexpr float analysisPlotRightGutter = 14.0f;
constexpr float analysisScrollbarSize = 19.0f;

std::string formatTime(double activeMs, bool milliseconds = true) {
    activeMs = std::max(0.0, activeMs);
    const auto totalWholeSeconds = static_cast<long long>(activeMs / 1000.0);
    const auto minutes = totalWholeSeconds / 60;
    const double seconds = activeMs / 1000.0 - static_cast<double>(minutes * 60);
    char buffer[64]{};
    if (milliseconds)
        std::snprintf(buffer, sizeof(buffer), "%lld:%06.3f", minutes, seconds);
    else
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld", minutes,
                      totalWholeSeconds % 60);
    return buffer;
}

int timeAxisFormatter(double seconds, char* buffer, int size, void*) {
    const auto label = formatTime(seconds * 1000.0, false);
    return std::snprintf(buffer, static_cast<std::size_t>(size), "%s",
                         label.c_str());
}

std::string readableName(std::string value) {
    bool capitalize = true;
    for (char& character : value) {
        if (character == '_') {
            character = ' ';
            capitalize = true;
        } else if (capitalize) {
            character = static_cast<char>(
                std::toupper(static_cast<unsigned char>(character)));
            capitalize = false;
        }
    }
    return value;
}

bool pointHovered(const ImVec2& point, float radius = 7.0f) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float x = mouse.x - point.x;
    const float y = mouse.y - point.y;
    return x * x + y * y <= radius * radius;
}

bool intervalHovered(ImVec2 first, ImVec2 second, float padding = 5.0f) {
    if (first.x > second.x)
        std::swap(first.x, second.x);
    if (first.y > second.y)
        std::swap(first.y, second.y);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    return mouse.x >= first.x - padding && mouse.x <= second.x + padding &&
           mouse.y >= first.y - padding && mouse.y <= second.y + padding;
}

void addFilledRect(ImDrawList* draw, ImVec2 first, ImVec2 second, ImU32 color,
                   float rounding = 0.0f) {
    const ImVec2 minimum{std::min(first.x, second.x),
                         std::min(first.y, second.y)};
    const ImVec2 maximum{std::max(first.x, second.x),
                         std::max(first.y, second.y)};
    draw->AddRectFilled(minimum, maximum, color, rounding);
}

void tooltipMacro(const TimelineMacroCycle& cycle) {
    ImGui::BeginTooltip();
    ImGui::Text("%s macro cycle", readableName(cycle.productType).c_str());
    ImGui::Text("Start: %s", formatTime(cycle.startActiveMs).c_str());
    ImGui::Text("Execution complete: %s",
                formatTime(cycle.executionEndActiveMs).c_str());
    ImGui::Text("Duration: %.2f s", cycle.durationMs / 1000.0);
    ImGui::Text("Full span: %.2f s", cycle.fullSpanMs / 1000.0);
    ImGui::Text("Production visits: %zu", cycle.visitCount);
    ImGui::Text("Access style: %s", readableName(cycle.accessStyle).c_str());
    ImGui::EndTooltip();
}

void tooltipControlGroup(const TimelineControlGroupEdit& edit) {
    ImGui::BeginTooltip();
    ImGui::Text("Time: %s", formatTime(edit.operationActiveMs).c_str());
    ImGui::Text("Group %d %s", edit.group, readableName(edit.operation).c_str());
    ImGui::Text("Selection: %s", readableName(edit.selectionMethod).c_str());
    if (edit.selectionToOperationMs)
        ImGui::Text("Selection -> group: %.0f ms",
                    *edit.selectionToOperationMs);
    else
        ImGui::TextUnformatted("Selection -> group: unavailable");
    if (edit.selectionDurationMs)
        ImGui::Text("Selection duration: %.0f ms", *edit.selectionDurationMs);
    if (edit.totalExecutionMs)
        ImGui::Text("Total execution: %.0f ms", *edit.totalExecutionMs);
    ImGui::Text("Scope: %s", edit.scope.c_str());
    ImGui::Text("Replay confirmation: %s (%s)",
                edit.replayConfirmed ? "yes" : "no",
                edit.bindingConfidence.c_str());
    ImGui::EndTooltip();
}

void tooltipScouting(const TimelineScoutingActivity& activity) {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted("Observed scouting activity");
    if (activity.unitTag != 0)
        ImGui::Text("Replay unit tag: %u", activity.unitTag);
    ImGui::Text("Control group: %d", activity.group);
    ImGui::Text("Assignment generation: %u", activity.assignmentGeneration);
    ImGui::Text("Assigned at: %s",
                formatTime(activity.assignedActiveMs).c_str());
    if (activity.lastCommandActiveMs)
        ImGui::Text("Last commanded at: %s",
                    formatTime(*activity.lastCommandActiveMs).c_str());
    else
        ImGui::TextUnformatted("Last commanded at: no attributed command");
    if (activity.activityDurationMs)
        ImGui::Text("Activity duration: %.2f s",
                    *activity.activityDurationMs / 1000.0);
    if (activity.longestCommandGapMs)
        ImGui::Text("Longest command gap: %.2f s",
                    *activity.longestCommandGapMs / 1000.0);
    ImGui::Text("Selections: %zu", activity.selectionCount);
    ImGui::Text("Commands: %zu", activity.commandCount);
    if (activity.outcomeAvailable) {
        ImGui::Text("Observed outcome: %s",
                    activity.returnedHome ? "Returned home"
                                          : "No observed return");
        if (activity.resumedAfterTemporaryReturn)
            ImGui::TextUnformatted("Resumed scouting after a temporary return");
    }
    ImGui::TextDisabled(
        "This is observed activity, not unit lifetime or survival time.");
    ImGui::EndTooltip();
}

void drawPoint(ImDrawList* draw, const ImVec2& point, ImU32 color, int shape) {
    if (shape == 1) {
        draw->AddRectFilled(ImVec2(point.x - 4, point.y - 4),
                            ImVec2(point.x + 4, point.y + 4), color, 1.0f);
    } else if (shape == 2) {
        draw->AddQuadFilled(ImVec2(point.x, point.y - 5),
                            ImVec2(point.x + 5, point.y),
                            ImVec2(point.x, point.y + 5),
                            ImVec2(point.x - 5, point.y), color);
    } else {
        draw->AddCircleFilled(point, 4.5f, color, 16);
    }
}

ImPlotFlags plotFlags(bool fitGame, ImPlotFlags base) {
    return fitGame ? base | ImPlotFlags_NoInputs : base;
}

void showTrackStatus(const char* label, bool& enabled,
                     const VisualizationTrackStatus& status) {
    ImGui::BeginDisabled(!status.available);
    ImGui::Checkbox(label, &enabled);
    ImGui::EndDisabled();
    if (!status.available &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Unavailable: %s", status.reason.c_str());
}

void drawArmyControlGroupManagementSummary(
    const GameAnalysisVisualizationModel& model) {
    ImGui::TextUnformatted("Army Control-Group Management");
    if (!model.controlGroupEditStatus.available) {
        ImGui::TextDisabled("Army Control-Group Management unavailable: %s",
                            model.controlGroupEditStatus.reason.c_str());
        return;
    }
    if (ImGui::BeginTable("##ArmyControlGroupManagementSummary", 2,
                          ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Edits / min");
        ImGui::TableSetColumnIndex(1);
        if (model.armyControlGroupEditsPerMinute) {
            ImGui::Text("%.1f", *model.armyControlGroupEditsPerMinute);
        } else {
            ImGui::TextDisabled("N/A");
        }
        ImGui::EndTable();
    }
}

void drawArmyCommandActivity(const GameAnalysisVisualizationModel& model) {
    ImGui::TextUnformatted("Army Command Activity");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Army commands are replay unit commands attributed only to "
            "selections confidently inferred as army units. Unknown or mixed "
            "selections are excluded.");
    }
    if (!model.armyCommandStatus.available) {
        ImGui::TextDisabled("Army Command Activity unavailable: %s",
                            model.armyCommandStatus.reason.c_str());
        return;
    }

    const auto metric = [](const char* label,
                           const std::optional<double>& value,
                           const char* format, double scale = 1.0) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
        if (value)
            ImGui::Text(format, *value * scale);
        else
            ImGui::TextDisabled("N/A");
    };
    if (ImGui::BeginTable("##ArmyCommandActivitySummary", 2,
                          ImGuiTableFlags_SizingFixedFit)) {
        metric("Commands / min", model.armyCommandsPerMinute, "%.1f");
        metric("Median command gap", model.medianArmyCommandGapMs, "%.2f s",
               0.001);
        metric("P90 command gap", model.p90ArmyCommandGapMs, "%.2f s", 0.001);
        metric("Longest command gap", model.longestArmyCommandGapMs,
               "%.2f s", 0.001);
        ImGui::EndTable();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Command gap = time between consecutive attributed army commands. "
            "It measures observed army-command inactivity, not literal unit "
            "idle time.");
    }

    ImGui::TextUnformatted("Army Command Gap Timeline");
    if (model.armyCommandGaps.empty()) {
        ImGui::TextDisabled(
            "At least two attributed army commands are required for command gaps.");
        return;
    }

    const double gameSeconds =
        std::max(1.0, model.activeDurationMs / 1000.0);
    if (!ImPlot::BeginPlot("##ArmyCommandGapTimeline",
                           ImVec2(-analysisPlotRightGutter, 175),
                           ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText |
                               ImPlotFlags_NoInputs))
        return;
    ImPlot::SetupAxis(ImAxis_X1, "Active game time");
    ImPlot::SetupAxisFormat(ImAxis_X1, timeAxisFormatter);
    ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, gameSeconds,
                            ImPlotCond_Always);
    ImPlot::SetupAxis(
        ImAxis_Y1, nullptr,
        ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks |
            ImPlotAxisFlags_Lock);
    ImPlot::SetupAxisLimits(ImAxis_Y1, -0.5, 0.5, ImPlotCond_Always);
    const double tick = 0.0;
    const char* label = "Army command gaps";
    ImPlot::SetupAxisTicks(ImAxis_Y1, &tick, 1, &label, false);
    ImPlot::SetupFinish();

    auto* draw = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    const ImVec2 plotMinimum = ImPlot::GetPlotPos();
    const ImVec2 plotSize = ImPlot::GetPlotSize();
    const ImVec2 plotMaximum{plotMinimum.x + plotSize.x,
                             plotMinimum.y + plotSize.y};
    bool tooltipShown = false;
    for (const auto& gap : model.armyCommandGaps) {
        const ImVec2 first = ImPlot::PlotToPixels(
            gap.startActiveMs / 1000.0, -0.16);
        const ImVec2 second = ImPlot::PlotToPixels(
            gap.endActiveMs / 1000.0, 0.16);
        const ImVec2 minimum{std::min(first.x, second.x),
                             std::min(first.y, second.y)};
        const ImVec2 maximum{std::max(first.x, second.x),
                             std::max(first.y, second.y)};
        const auto band = analysis_insights::macroGapBand(
            gap.durationMs / 1000.0);
        const ImU32 color = analysis_insights::macroGapBandColor(band);
        if (gap.durationMs > 0.0)
            addFilledRect(draw, first, second, color, 1.0f);
        else
            draw->AddLine(ImVec2(first.x, first.y),
                          ImVec2(first.x, second.y), color, 2.0f);
        if (!tooltipShown &&
            ImGui::IsMouseHoveringRect(plotMinimum, plotMaximum) &&
            ImGui::IsMouseHoveringRect(minimum, maximum)) {
            ImGui::BeginTooltip();
            ImGui::Text("Gap start: %s",
                        formatTime(gap.startActiveMs).c_str());
            ImGui::Text("Gap end: %s",
                        formatTime(gap.endActiveMs).c_str());
            ImGui::Text("Gap duration: %.2f s", gap.durationMs / 1000.0);
            ImGui::EndTooltip();
            tooltipShown = true;
        }
    }
    ImPlot::PopPlotClipRect();
    ImPlot::EndPlot();
    analysis_insights::drawMacroGapLengthLegend();
}

void drawAbilityActivity(const GameAnalysisVisualizationModel& model) {
    ImGui::TextUnformatted("Ability Activity");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Ability Activity counts recognized replay ability commands "
            "issued by the player. One replay command counts as one use "
            "regardless of how many units were selected. These are issued "
            "ability commands, not independently verified spell effects.");
    }
    if (!model.abilityActivityStatus.available) {
        ImGui::TextDisabled("Ability Activity unavailable: %s",
                            model.abilityActivityStatus.reason.c_str());
        return;
    }
    const bool hasAbilityActivity =
        analysis_insights::hasAbilityActivityForDisplay(
            model.totalAbilityUses);

    if (ImGui::BeginTable("##AbilityActivitySummary", 2,
                          ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Abilities / min");
        ImGui::TableSetColumnIndex(1);
        if (hasAbilityActivity && model.abilitiesPerMinute)
            ImGui::Text("%.1f", *model.abilitiesPerMinute);
        else
            ImGui::TextDisabled("N/A");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Total abilities");
        ImGui::TableSetColumnIndex(1);
        if (hasAbilityActivity)
            ImGui::Text("%zu", *model.totalAbilityUses);
        else
            ImGui::TextDisabled("N/A");
        ImGui::EndTable();
    }

    if (model.abilityActivityBreakdown.empty()) {
        ImGui::TextDisabled(
            "No recognized ability commands were observed in this game.");
        return;
    }
    if (ImGui::BeginTable(
            "##AbilityActivityBreakdown", 3,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Ability");
        ImGui::TableSetupColumn("Uses");
        ImGui::TableSetupColumn("Uses / min");
        ImGui::TableHeadersRow();
        for (const auto& ability : model.abilityActivityBreakdown) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(ability.ability.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%zu", ability.uses);
            ImGui::TableSetColumnIndex(2);
            if (ability.usesPerMinute)
                ImGui::Text("%.1f", *ability.usesPerMinute);
            else
                ImGui::TextDisabled("N/A");
        }
        ImGui::EndTable();
    }
}

void drawTimeline(const GameAnalysisVisualizationModel& model,
                  AnalysisViewState& runtime) {
    ImGui::TextUnformatted("Tracks");
    showTrackStatus("Worker macro", runtime.showWorker,
                    model.workerMacroStatus);
    ImGui::SameLine();
    showTrackStatus("Army macro", runtime.showArmy, model.armyMacroStatus);
    ImGui::SameLine();
    showTrackStatus("Army control-group edits", runtime.showControlGroupEdits,
                    model.controlGroupEditStatus);
    ImGui::SameLine();
    showTrackStatus("Scouting activity", runtime.showScouting,
                    model.scoutingStatus);
    ImGui::SameLine();
    ImGui::Checkbox("Fit Game", &runtime.fitTimeline);
    ImGui::SameLine();
    if (ImGui::Button("Select all")) {
        runtime.showWorker = true;
        runtime.showArmy = true;
        runtime.showControlGroupEdits = true;
        runtime.showScouting = true;
    }

    ImGui::TextDisabled(
        "Macro faded tails are observed span after execution completion.");

    const AnalysisTimelineTrackVisibility enabled{
        runtime.showWorker, runtime.showArmy, runtime.showControlGroupEdits,
        runtime.showScouting};
    const AnalysisTimelineTrackVisibility available{
        model.workerMacroStatus.available, model.armyMacroStatus.available,
        model.controlGroupEditStatus.available, model.scoutingStatus.available};
    const auto visibleTracks =
        buildAnalysisTimelineTrackLayout(enabled, available);
    if (visibleTracks.empty()) {
        ImGui::TextDisabled("No timeline tracks selected/available.");
        return;
    }

    const auto trackY = [&](AnalysisTimelineTrack track) {
        const auto found = std::find_if(
            visibleTracks.begin(), visibleTracks.end(),
            [track](const AnalysisTimelineTrackRow& row) {
                return row.track == track;
            });
        return found == visibleTracks.end() ? nullptr : &*found;
    };
    const auto trackLabel = [](AnalysisTimelineTrack track) {
        switch (track) {
        case AnalysisTimelineTrack::WorkerMacro:
            return "Worker macro";
        case AnalysisTimelineTrack::ArmyMacro:
            return "Army macro";
        case AnalysisTimelineTrack::ControlGroupEdits:
            return "Army CG edits";
        case AnalysisTimelineTrack::Scouting:
            return "Scouting";
        }
        return "";
    };
    const double timelineYMinimum = -0.55;
    const double timelineYMaximum =
        static_cast<double>(visibleTracks.size()) - 0.45;

    const double gameSeconds =
        std::max(1.0, model.activeDurationMs / 1000.0);
    if (runtime.fitTimeline)
        ImPlot::SetNextAxisLimits(ImAxis_X1, 0.0, gameSeconds,
                                  ImPlotCond_Always);

    const ImPlotFlags flags = plotFlags(
        runtime.fitTimeline,
        ImPlotFlags_NoLegend | ImPlotFlags_Crosshairs |
            ImPlotFlags_NoMouseText);
    if (!ImPlot::BeginPlot("Full-game mechanics timeline",
                           ImVec2(-analysisPlotRightGutter, 390), flags))
        return;

    ImPlot::SetupAxis(ImAxis_X1, "Active game time");
    ImPlot::SetupAxisFormat(ImAxis_X1, timeAxisFormatter);
    ImPlot::SetupAxisLimits(
        ImAxis_X1, 0.0, gameSeconds,
        runtime.fitTimeline ? ImPlotCond_Always : ImPlotCond_Once);
    ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, gameSeconds);
    ImPlot::SetupAxisZoomConstraints(ImAxis_X1, std::min(2.0, gameSeconds),
                                     gameSeconds);
    ImPlot::SetupAxis(
        ImAxis_Y1, nullptr,
        ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks |
            ImPlotAxisFlags_Lock);
    ImPlot::SetupAxisLimits(ImAxis_Y1, timelineYMinimum, timelineYMaximum,
                            ImPlotCond_Always);
    std::vector<double> trackTicks;
    std::vector<const char*> trackLabels;
    trackTicks.reserve(visibleTracks.size());
    trackLabels.reserve(visibleTracks.size());
    for (auto track = visibleTracks.rbegin(); track != visibleTracks.rend();
         ++track) {
        trackTicks.push_back(track->y);
        trackLabels.push_back(trackLabel(track->track));
    }
    ImPlot::SetupAxisTicks(ImAxis_Y1, trackTicks.data(),
                           static_cast<int>(trackTicks.size()),
                           trackLabels.data(), false);
    ImPlot::SetupFinish();

    auto* draw = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    bool tooltipShown = false;

    const auto drawCycles =
        [&](const std::vector<TimelineMacroCycle>& cycles, double y,
            ImU32 body, ImU32 tail) {
            for (const auto& cycle : cycles) {
                const ImVec2 first = ImPlot::PlotToPixels(
                    cycle.startActiveMs / 1000.0, y - 0.15);
                const ImVec2 execution = ImPlot::PlotToPixels(
                    cycle.executionEndActiveMs / 1000.0, y + 0.15);
                addFilledRect(draw, first, execution, body, 2.0f);
                const ImVec2 executionCenter = ImPlot::PlotToPixels(
                    cycle.executionEndActiveMs / 1000.0, y);
                const ImVec2 end =
                    ImPlot::PlotToPixels(cycle.endActiveMs / 1000.0, y);
                if (cycle.endActiveMs > cycle.executionEndActiveMs)
                    draw->AddLine(executionCenter, end, tail, 3.0f);
                const ImVec2 entireEnd = ImPlot::PlotToPixels(
                    cycle.endActiveMs / 1000.0, y + 0.15);
                if (!tooltipShown && intervalHovered(first, entireEnd)) {
                    tooltipMacro(cycle);
                    tooltipShown = true;
                }
            }
        };
    if (const auto* worker = trackY(AnalysisTimelineTrack::WorkerMacro))
        drawCycles(model.workerMacroCycles, worker->y,
                   IM_COL32(29, 137, 132, 220),
                   IM_COL32(29, 137, 132, 95));
    if (const auto* army = trackY(AnalysisTimelineTrack::ArmyMacro))
        drawCycles(model.armyMacroCycles, army->y,
                   IM_COL32(118, 82, 160, 220),
                   IM_COL32(118, 82, 160, 95));

    if (const auto* controlGroups =
            trackY(AnalysisTimelineTrack::ControlGroupEdits)) {
        for (const auto& edit : model.armyControlGroupEdits) {
            const ImVec2 point = ImPlot::PlotToPixels(
                edit.operationActiveMs / 1000.0, controlGroups->y);
            drawPoint(
                draw, point,
                edit.operation == "add"
                    ? IM_COL32(193, 106, 47, 255)
                    : IM_COL32(72, 96, 145, 255),
                edit.operation == "add" ? 2 : 0);
            if (!tooltipShown && pointHovered(point)) {
                tooltipControlGroup(edit);
                tooltipShown = true;
            }
        }
    }

    if (const auto* scouting = trackY(AnalysisTimelineTrack::Scouting)) {
        for (const auto& activity : model.scoutingActivities) {
            const ImVec2 assigned = ImPlot::PlotToPixels(
                activity.assignedActiveMs / 1000.0, scouting->y);
            drawPoint(draw, assigned, IM_COL32(39, 126, 153, 255), 1);
            ImVec2 ending = assigned;
            if (activity.lastCommandActiveMs) {
                ending = ImPlot::PlotToPixels(
                    *activity.lastCommandActiveMs / 1000.0, scouting->y);
                draw->AddLine(assigned, ending,
                              IM_COL32(39, 126, 153, 185), 5.0f);
                drawPoint(draw, ending, IM_COL32(25, 85, 106, 255), 2);
            }
            if (!tooltipShown && intervalHovered(assigned, ending, 7.0f)) {
                tooltipScouting(activity);
                tooltipShown = true;
            }
        }
    }

    if (ImPlot::IsPlotHovered()) {
        const double cursor =
            std::clamp(ImPlot::GetPlotMousePos().x, 0.0, gameSeconds);
        const ImVec2 top =
            ImPlot::PlotToPixels(cursor, timelineYMaximum - 0.05);
        const ImVec2 bottom =
            ImPlot::PlotToPixels(cursor, timelineYMinimum + 0.05);
        draw->AddLine(top, bottom, IM_COL32(205, 214, 225, 115), 1.0f);
        const std::string label = formatTime(cursor * 1000.0);
        draw->AddText(ImVec2(top.x + 5.0f, top.y + 3.0f),
                      IM_COL32(226, 232, 240, 230), label.c_str());
    }
    ImPlot::PopPlotClipRect();
    ImPlot::EndPlot();
}

struct CategoryCount {
    std::string label;
    int count{};
};

int categoryTotal(const std::vector<CategoryCount>& categories) {
    int total = 0;
    for (const auto& category : categories)
        total += category.count;
    return total;
}

std::string accessStyleLabel(const std::string& style) {
    if (style == "control_group_only")
        return "Control Group Only";
    if (style == "location_hotkey_click")
        return "Location Hotkey Click";
    if (style == "control_group_center_click")
        return "Control Group Center Click";
    if (style == "mixed")
        return "Mixed";
    return readableName(style);
}

bool meaningfulAccessStyle(const std::string& style) {
    return !style.empty() && style != "other" && style != "unknown";
}

std::string selectionMethodLabel(const std::string& method) {
    if (method == "direct_click")
        return "Direct Click";
    if (method == "box_select")
        return "Box Select";
    if (method == "ctrl_click_type")
        return "Ctrl-Click Type";
    if (method == "double_click_type")
        return "Double-Click Type";
    if (method == "shift_click_modify")
        return "Shift-Click Modify";
    if (method == "shift_box_modify")
        return "Shift-Box Modify";
    if (method == "ctrl_shift_click_type")
        return "Ctrl+Shift-Click Type";
    return readableName(method);
}

bool meaningfulSelectionMethod(const std::string& method) {
    return !method.empty() && method != "other" &&
           method != "existing_selection" && method != "unknown";
}

std::vector<CategoryCount>
cameraNavigationBreakdown(const GameAnalysisVisualizationModel& model) {
    std::array<int, 4> counts{};
    for (const auto& event : model.navigationEvents) {
        switch (event.type) {
        case CameraNavigationType::ControlGroupJump:
            ++counts[0];
            break;
        case CameraNavigationType::LocationHotkey:
            ++counts[1];
            break;
        case CameraNavigationType::MinimapJump:
            ++counts[2];
            break;
        case CameraNavigationType::EdgeScroll:
            ++counts[3];
            break;
        }
    }
    constexpr std::array<const char*, 4> labels{
        "Control-Group Jumps",
        "Location-Hotkey Jumps",
        "Minimap Jumps",
        "Edge Pans",
    };
    std::vector<CategoryCount> result;
    for (std::size_t index = 0; index < counts.size(); ++index) {
        if (counts[index] > 0)
            result.push_back({labels[index], counts[index]});
    }
    return result;
}

std::vector<CategoryCount> macroAccessBreakdown(
    const std::vector<MacroAccessStyleDurationGroup>& groups) {
    std::vector<CategoryCount> result;
    for (const auto& group : groups) {
        if (!meaningfulAccessStyle(group.accessStyle) ||
            group.durationMs.empty())
            continue;
        result.push_back(
            {accessStyleLabel(group.accessStyle),
             static_cast<int>(group.durationMs.size())});
    }
    return result;
}

std::vector<CategoryCount> selectionMethodBreakdown(
    const GameAnalysisVisualizationModel& model,
    const char* operation) {
    std::vector<CategoryCount> result;
    for (const auto& edit : model.armyControlGroupEdits) {
        if (edit.operation != operation ||
            !meaningfulSelectionMethod(edit.selectionMethod))
            continue;
        const std::string label =
            selectionMethodLabel(edit.selectionMethod);
        auto found = std::find_if(
            result.begin(), result.end(),
            [&](const CategoryCount& item) { return item.label == label; });
        if (found == result.end())
            result.push_back({label, 1});
        else
            ++found->count;
    }
    return result;
}

void drawBarBreakdown(const char* title,
                      const std::vector<CategoryCount>& categories) {
    const int total = categoryTotal(categories);
    int maximumCount = 1;
    std::vector<double> values;
    std::vector<double> ticks;
    std::vector<std::string> labels;
    std::vector<const char*> labelPointers;
    std::vector<ImU32> barColors;
    values.reserve(categories.size());
    ticks.reserve(categories.size());
    labels.reserve(categories.size());
    labelPointers.reserve(categories.size());
    barColors.reserve(categories.size());

    for (std::size_t index = 0; index < categories.size(); ++index) {
        values.push_back(static_cast<double>(categories[index].count));
        ticks.push_back(static_cast<double>(index));
        labels.push_back(categories[index].label);
        maximumCount = std::max(maximumCount, categories[index].count);
        barColors.push_back(ImGui::ColorConvertFloat4ToU32(
            ImPlot::GetColormapColor(static_cast<int>(index),
                                     ImPlotColormap_Deep)));
    }
    for (const auto& label : labels)
        labelPointers.push_back(label.c_str());

    constexpr ImPlotFlags flags =
        ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText |
        ImPlotFlags_NoInputs;
    if (ImPlot::BeginPlot(title, ImVec2(-analysisPlotRightGutter, 285),
                          flags)) {
        ImPlot::SetupAxis(ImAxis_X1, "Count", ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(
            ImAxis_X1, 0.0, static_cast<double>(maximumCount) * 1.28,
            ImPlotCond_Always);
        ImPlot::SetupAxis(ImAxis_Y1, nullptr,
                          ImPlotAxisFlags_NoGridLines |
                              ImPlotAxisFlags_NoTickMarks |
                              ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisTicks(ImAxis_Y1, ticks.data(),
                               static_cast<int>(ticks.size()),
                               labelPointers.data(), false);
        ImPlot::SetupAxisLimits(
            ImAxis_Y1, -0.6,
            static_cast<double>(categories.size()) - 0.4,
            ImPlotCond_Always);
        ImPlot::SetupFinish();

        ImPlotSpec spec;
        spec.Flags = ImPlotBarsFlags_Horizontal;
        spec.FillColors = barColors.data();
        spec.LineColors = barColors.data();
        ImPlot::PlotBars("Count", values.data(),
                         static_cast<int>(values.size()), 0.62, 0.0, spec);

        for (std::size_t index = 0; index < categories.size(); ++index) {
            const double percentage =
                total > 0
                    ? static_cast<double>(categories[index].count) *
                          100.0 / static_cast<double>(total)
                    : 0.0;
            char label[64]{};
            std::snprintf(label, sizeof(label), "%d  |  %.1f%%",
                          categories[index].count, percentage);
            ImPlot::PlotText(label, values[index],
                             static_cast<double>(index),
                             ImVec2(8.0f, 0.0f));
        }
        ImPlot::EndPlot();
    }
}

void drawCategoricalBreakdown(
    const char* title,
    const VisualizationTrackStatus& status,
    const std::vector<CategoryCount>& categories,
    const char* emptyMessage) {
    if (!status.available) {
        ImGui::TextDisabled("%s unavailable: %s", title,
                            status.reason.c_str());
        return;
    }
    if (categories.empty()) {
        ImGui::TextDisabled("%s", emptyMessage);
        return;
    }
    drawBarBreakdown(title, categories);
}

} // namespace

void drawAnalysisView(const GameAnalysisVisualizationModel& model,
                      AnalysisViewState& runtime,
                      const ReportGroupVisibility& visibility) {
    ImGui::GetStyle().ScrollbarSize =
        std::max(ImGui::GetStyle().ScrollbarSize, analysisScrollbarSize);

    ImGui::Text("Latest Game Analysis%s%s",
                model.sessionId.empty() ? "" : " - ",
                model.sessionId.c_str());
    ImGui::TextDisabled(
        "Read-only visualization from the paired .nav and derived .json files.");
    if (!model.navLoaded)
        ImGui::TextColored(
            ImVec4(0.72f, 0.40f, 0.12f, 1.0f),
            "Navigation unavailable: %s", model.navigationStatus.reason.c_str());
    if (!model.jsonLoaded)
        ImGui::TextColored(
            ImVec4(0.72f, 0.40f, 0.12f, 1.0f),
            "Derived analysis unavailable: %s",
            model.workerMacroStatus.reason.c_str());

    if (visibility.gameTimeline) {
        ImGui::SeparatorText("Game timeline");
        drawTimeline(model, runtime);
    }

    if (visibility.hasMacroAnalysisSections()) {
        ImGui::SeparatorText("Macro");
        analysis_insights::drawMacroAnalysis(
            model, visibility.macroGaps,
            visibility.macroDurationDistribution);
        if (visibility.macroAccessStyles) {
            drawCategoricalBreakdown(
                "Worker Macro Access Styles", model.workerMacroStatus,
                macroAccessBreakdown(model.workerAccessStyleDurations),
                "No classified worker macro access styles were detected in this game.");
            drawCategoricalBreakdown(
                "Army Macro Access Styles", model.armyMacroStatus,
                macroAccessBreakdown(model.armyAccessStyleDurations),
                "No classified army macro access styles were detected in this game.");
        }
    }

    if (visibility.hasArmyManagementAnalysisSections()) {
        ImGui::SeparatorText("Army Management");
        if (visibility.armyControlGroupManagement)
            drawArmyControlGroupManagementSummary(model);
        if (visibility.armyCommandActivity)
            drawArmyCommandActivity(model);
        if (visibility.abilityActivity)
            drawAbilityActivity(model);
        if (visibility.armyControlGroupManagement) {
            drawCategoricalBreakdown(
                "Control-Group Assignment Selection Methods",
                model.controlGroupEditStatus,
                selectionMethodBreakdown(model, "assign"),
                "No classified control-group assignment selection methods were detected in this game.");
            drawCategoricalBreakdown(
                "Control-Group Addition Selection Methods",
                model.controlGroupEditStatus,
                selectionMethodBreakdown(model, "add"),
                "No classified control-group addition selection methods were detected in this game.");

            ImGui::TextDisabled(
                "Control-group breakdowns omit ambiguous Other/Existing Selection "
                "observations. Horizontal frequency bars use varied category colors "
                "for readability.");
        }
    }

    if (visibility.hasMultitaskingAnalysisSections()) {
        ImGui::SeparatorText("Multitasking");
        analysis_insights::drawMultitaskingAnalysis(
            model, visibility.navigationTransitionRate,
            visibility.multitaskingDensity,
            visibility.scoutingUnitActivity);
        if (visibility.cameraNavigation) {
            drawCategoricalBreakdown(
                "Camera Navigation Methods", model.navigationStatus,
                cameraNavigationBreakdown(model),
                "No camera-navigation transitions were detected in this game.");
        }
    }
}

} // namespace smp
