#include "app/analysis_view.h"

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

std::string joined(const std::vector<std::string>& values) {
    if (values.empty())
        return "none recorded";
    std::string result;
    for (const auto& value : values) {
        if (!result.empty())
            result += ", ";
        result += value;
    }
    return result;
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

void tooltipNavigation(const TimelineNavigationEvent& event) {
    ImGui::BeginTooltip();
    ImGui::Text("Time: %s", formatTime(event.activeMs).c_str());
    ImGui::Text("Type: %s", cameraNavigationTypeName(event.type));
    if (event.id >= 0)
        ImGui::Text("Group/location: %d", event.id);
    if (event.type == CameraNavigationType::EdgeScroll) {
        ImGui::Text("Direction: %s", edgeDirectionName(event.edgeDirection));
        ImGui::Text("Duration: %.0f ms", event.durationMs);
    }
    ImGui::EndTooltip();
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

void tooltipVisit(const TimelineProductionVisit& visit) {
    ImGui::BeginTooltip();
    ImGui::Text("%s production visit", readableName(visit.productType).c_str());
    ImGui::Text("Access start: %s", formatTime(visit.startActiveMs).c_str());
    ImGui::Text("Building access: %s", formatTime(visit.contextActiveMs).c_str());
    ImGui::Text("First production attempt: %s",
                formatTime(visit.firstProductionActiveMs).c_str());
    ImGui::Text("Visit end: %s", formatTime(visit.endActiveMs).c_str());
    ImGui::Separator();
    ImGui::Text("Selection access: %s",
                readableName(visit.selectionAccess).c_str());
    ImGui::Text("Camera access: %s", readableName(visit.cameraAccess).c_str());
    if (visit.controlGroup)
        ImGui::Text("Control group: %d", *visit.controlGroup);
    if (visit.locationHotkey)
        ImGui::Text("Location hotkey: %d", *visit.locationHotkey);
    ImGui::TextWrapped("Produced units: %s", joined(visit.producedUnits).c_str());
    ImGui::Text("Physical production presses: %d",
                visit.physicalProductionPresses);
    ImGui::Text("Replay confirmation: %s",
                visit.replayConfirmed ? "yes" : "no");
    ImGui::Text("Access to building: %.0f ms", visit.accessLatencyMs);
    ImGui::Text("Production response latency: %.0f ms",
                visit.productionLatencyMs);
    ImGui::Text("Time to first attempt: %.0f ms", visit.executionDurationMs);
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
    ImGui::Text("Selections: %zu", activity.selectionCount);
    ImGui::Text("Commands: %zu", activity.commandCount);
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

enum class VisitStageMarker {
    AccessStart,
    BuildingAccess,
    FirstAttempt,
    VisitEnd,
};

constexpr ImU32 visitStartColor = IM_COL32(204, 129, 225, 255);
constexpr ImU32 visitBuildingColor = IM_COL32(151, 128, 235, 255);
constexpr ImU32 visitAttemptColor = IM_COL32(239, 124, 165, 255);
constexpr ImU32 visitEndColor = IM_COL32(218, 221, 240, 255);
constexpr ImU32 visitLineColor = IM_COL32(155, 132, 181, 190);

void drawVisitStageMarker(ImDrawList* draw, const ImVec2& point,
                          VisitStageMarker marker, ImU32 color) {
    switch (marker) {
    case VisitStageMarker::AccessStart:
        draw->AddCircle(point, 5.0f, color, 16, 2.0f);
        break;
    case VisitStageMarker::BuildingAccess:
        draw->AddTriangle(ImVec2(point.x, point.y - 5.5f),
                          ImVec2(point.x + 5.0f, point.y + 4.5f),
                          ImVec2(point.x - 5.0f, point.y + 4.5f), color, 2.0f);
        break;
    case VisitStageMarker::FirstAttempt:
        draw->AddLine(ImVec2(point.x - 4.5f, point.y - 4.5f),
                      ImVec2(point.x + 4.5f, point.y + 4.5f), color, 2.0f);
        draw->AddLine(ImVec2(point.x + 4.5f, point.y - 4.5f),
                      ImVec2(point.x - 4.5f, point.y + 4.5f), color, 2.0f);
        break;
    case VisitStageMarker::VisitEnd:
        draw->AddRect(ImVec2(point.x - 4.5f, point.y - 4.5f),
                      ImVec2(point.x + 4.5f, point.y + 4.5f), color, 1.0f,
                      0, 2.0f);
        break;
    }
}

void drawLegendPoint(const char* label, ImU32 color, int shape) {
    ImGui::Dummy(ImVec2(14.0f, 14.0f));
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const ImVec2 center{(minimum.x + maximum.x) * 0.5f,
                        (minimum.y + maximum.y) * 0.5f};
    drawPoint(ImGui::GetWindowDrawList(), center, color, shape);
    ImGui::SameLine(0.0f, 5.0f);
    ImGui::TextDisabled("%s", label);
}

void drawLegendInterval(const char* label, ImU32 color) {
    ImGui::Dummy(ImVec2(18.0f, 14.0f));
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const float centerY = (minimum.y + maximum.y) * 0.5f;
    addFilledRect(ImGui::GetWindowDrawList(),
                  ImVec2(minimum.x + 1.0f, centerY - 3.0f),
                  ImVec2(maximum.x - 1.0f, centerY + 3.0f), color, 2.0f);
    ImGui::SameLine(0.0f, 5.0f);
    ImGui::TextDisabled("%s", label);
}

void drawVisitLegendPoint(const char* label, VisitStageMarker marker,
                          ImU32 color) {
    ImGui::Dummy(ImVec2(16.0f, 16.0f));
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const ImVec2 center{(minimum.x + maximum.x) * 0.5f,
                        (minimum.y + maximum.y) * 0.5f};
    drawVisitStageMarker(ImGui::GetWindowDrawList(), center, marker, color);
    ImGui::SameLine(0.0f, 5.0f);
    ImGui::TextDisabled("%s", label);
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

void drawProductionVisitLegend() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4(0.13f, 0.105f, 0.16f, 0.72f));
    constexpr ImGuiChildFlags childFlags =
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY;
    constexpr ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::BeginChild("##ProductionVisitLegend", ImVec2(0.0f, 0.0f),
                      childFlags, windowFlags);
    ImGui::TextDisabled("Production visit stages:");
    ImGui::SameLine();
    drawVisitLegendPoint("Access start", VisitStageMarker::AccessStart,
                         visitStartColor);
    ImGui::SameLine(0.0f, 16.0f);
    drawVisitLegendPoint("Building access", VisitStageMarker::BuildingAccess,
                         visitBuildingColor);
    ImGui::SameLine(0.0f, 16.0f);
    drawVisitLegendPoint("First attempt", VisitStageMarker::FirstAttempt,
                         visitAttemptColor);
    ImGui::SameLine(0.0f, 16.0f);
    drawVisitLegendPoint("Visit end", VisitStageMarker::VisitEnd,
                         visitEndColor);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void drawTimeline(const GameAnalysisVisualizationModel& model,
                  AnalysisViewState& runtime) {
    ImGui::TextUnformatted("Tracks");
    showTrackStatus("Camera navigation", runtime.showNavigation,
                    model.navigationStatus);
    ImGui::SameLine();
    showTrackStatus("Worker macro", runtime.showWorker,
                    model.workerMacroStatus);
    ImGui::SameLine();
    showTrackStatus("Army macro", runtime.showArmy, model.armyMacroStatus);
    ImGui::SameLine();
    showTrackStatus("Production visits", runtime.showProductionVisits,
                    model.productionVisitStatus);
    showTrackStatus("Army control-group edits", runtime.showControlGroupEdits,
                    model.controlGroupEditStatus);
    ImGui::SameLine();
    showTrackStatus("Scouting activity", runtime.showScouting,
                    model.scoutingStatus);
    ImGui::SameLine();
    ImGui::Checkbox("Fit Game", &runtime.fitTimeline);
    ImGui::SameLine();
    ImGui::BeginDisabled(runtime.fitTimeline);
    if (ImGui::Button("Reset view"))
        runtime.resetTimeline = true;
    ImGui::EndDisabled();

    ImGui::TextDisabled("Camera:");
    ImGui::SameLine();
    drawLegendPoint("Control group", IM_COL32(45, 105, 180, 255), 0);
    ImGui::SameLine(0.0f, 16.0f);
    drawLegendPoint("Location", IM_COL32(26, 145, 160, 255), 1);
    ImGui::SameLine(0.0f, 16.0f);
    drawLegendPoint("Minimap", IM_COL32(214, 145, 45, 255), 2);
    ImGui::SameLine(0.0f, 16.0f);
    drawLegendInterval("Edge pan", IM_COL32(93, 113, 130, 190));
    ImGui::TextDisabled(
        "Macro faded tails are observed span after execution completion.");
    if (runtime.showProductionVisits)
        drawProductionVisitLegend();

    const double gameSeconds =
        std::max(1.0, model.activeDurationMs / 1000.0);
    if (runtime.fitTimeline || runtime.resetTimeline)
        ImPlot::SetNextAxisLimits(ImAxis_X1, 0.0, gameSeconds,
                                  ImPlotCond_Always);

    const ImPlotFlags flags = plotFlags(
        runtime.fitTimeline,
        ImPlotFlags_NoLegend | ImPlotFlags_Crosshairs |
            ImPlotFlags_NoMouseText);
    if (!ImPlot::BeginPlot("Full-game mechanics timeline",
                           ImVec2(-analysisPlotRightGutter, 390), flags)) {
        runtime.resetTimeline = false;
        return;
    }
    runtime.resetTimeline = false;

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
    ImPlot::SetupAxisLimits(ImAxis_Y1, -0.55, 5.55, ImPlotCond_Always);
    const double trackTicks[]{0, 1, 2, 3, 4, 5};
    const char* trackLabels[]{"Scouting", "Army CG edits", "Production visits",
                              "Army macro", "Worker macro",
                              "Camera navigation"};
    ImPlot::SetupAxisTicks(ImAxis_Y1, trackTicks, 6, trackLabels, false);
    ImPlot::SetupFinish();

    auto* draw = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    bool tooltipShown = false;

    if (runtime.showNavigation && model.navigationStatus.available) {
        for (const auto& event : model.navigationEvents) {
            const double start = event.activeMs / 1000.0;
            if (event.type == CameraNavigationType::EdgeScroll &&
                event.durationMs > 0.0) {
                const ImVec2 first =
                    ImPlot::PlotToPixels(start, 5.0 - 0.13);
                const ImVec2 second = ImPlot::PlotToPixels(
                    (event.activeMs + event.durationMs) / 1000.0,
                    5.0 + 0.13);
                addFilledRect(draw, first, second,
                              IM_COL32(93, 113, 130, 190), 2.0f);
                if (!tooltipShown && intervalHovered(first, second)) {
                    tooltipNavigation(event);
                    tooltipShown = true;
                }
                continue;
            }
            const ImVec2 point = ImPlot::PlotToPixels(start, 5.0);
            ImU32 color = IM_COL32(45, 105, 180, 255);
            int shape = 0;
            if (event.type == CameraNavigationType::LocationHotkey) {
                color = IM_COL32(26, 145, 160, 255);
                shape = 1;
            } else if (event.type == CameraNavigationType::MinimapJump) {
                color = IM_COL32(214, 145, 45, 255);
                shape = 2;
            }
            drawPoint(draw, point, color, shape);
            if (!tooltipShown && pointHovered(point)) {
                tooltipNavigation(event);
                tooltipShown = true;
            }
        }
    }

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
    if (runtime.showWorker && model.workerMacroStatus.available)
        drawCycles(model.workerMacroCycles, 4.0,
                   IM_COL32(29, 137, 132, 220),
                   IM_COL32(29, 137, 132, 95));
    if (runtime.showArmy && model.armyMacroStatus.available)
        drawCycles(model.armyMacroCycles, 3.0,
                   IM_COL32(118, 82, 160, 220),
                   IM_COL32(118, 82, 160, 95));

    if (runtime.showProductionVisits &&
        model.productionVisitStatus.available) {
        for (const auto& visit : model.productionVisits) {
            const ImVec2 start =
                ImPlot::PlotToPixels(visit.startActiveMs / 1000.0, 2.0);
            const ImVec2 building =
                ImPlot::PlotToPixels(visit.contextActiveMs / 1000.0, 2.0);
            const ImVec2 production = ImPlot::PlotToPixels(
                visit.firstProductionActiveMs / 1000.0, 2.0);
            const ImVec2 end =
                ImPlot::PlotToPixels(visit.endActiveMs / 1000.0, 2.0);
            draw->AddLine(start, end, visitLineColor, 2.0f);
            drawVisitStageMarker(draw, start, VisitStageMarker::AccessStart,
                                 visitStartColor);
            drawVisitStageMarker(draw, building,
                                 VisitStageMarker::BuildingAccess,
                                 visitBuildingColor);
            drawVisitStageMarker(draw, production,
                                 VisitStageMarker::FirstAttempt,
                                 visitAttemptColor);
            drawVisitStageMarker(draw, end, VisitStageMarker::VisitEnd,
                                 visitEndColor);
            if (!tooltipShown && intervalHovered(start, end, 7.0f)) {
                tooltipVisit(visit);
                tooltipShown = true;
            }
        }
    }

    if (runtime.showControlGroupEdits &&
        model.controlGroupEditStatus.available) {
        for (const auto& edit : model.armyControlGroupEdits) {
            const ImVec2 point = ImPlot::PlotToPixels(
                edit.operationActiveMs / 1000.0, 1.0);
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

    if (runtime.showScouting && model.scoutingStatus.available) {
        for (const auto& activity : model.scoutingActivities) {
            const ImVec2 assigned = ImPlot::PlotToPixels(
                activity.assignedActiveMs / 1000.0, 0.0);
            drawPoint(draw, assigned, IM_COL32(39, 126, 153, 255), 1);
            ImVec2 ending = assigned;
            if (activity.lastCommandActiveMs) {
                ending = ImPlot::PlotToPixels(
                    *activity.lastCommandActiveMs / 1000.0, 0.0);
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
        const ImVec2 top = ImPlot::PlotToPixels(cursor, 5.5);
        const ImVec2 bottom = ImPlot::PlotToPixels(cursor, -0.5);
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

void drawPieBreakdown(const char* title,
                      const std::vector<CategoryCount>& categories) {
    const int total = categoryTotal(categories);
    std::vector<double> values;
    std::vector<std::string> labels;
    std::vector<const char*> labelPointers;
    values.reserve(categories.size());
    labels.reserve(categories.size());
    labelPointers.reserve(categories.size());

    for (const auto& category : categories) {
        values.push_back(static_cast<double>(category.count));
        const double percentage =
            total > 0
                ? static_cast<double>(category.count) * 100.0 /
                      static_cast<double>(total)
                : 0.0;
        char suffix[64]{};
        std::snprintf(suffix, sizeof(suffix), " (%d, %.1f%%)",
                      category.count, percentage);
        labels.push_back(category.label + suffix);
    }
    for (const auto& label : labels)
        labelPointers.push_back(label.c_str());

    constexpr ImPlotFlags flags =
        ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs;
    if (ImPlot::BeginPlot(title, ImVec2(-analysisPlotRightGutter, 250),
                          flags)) {
        constexpr ImPlotAxisFlags axisFlags =
            ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_Lock;
        ImPlot::SetupAxes(nullptr, nullptr, axisFlags, axisFlags);
        ImPlot::SetupAxesLimits(-1.05, 1.05, -1.05, 1.05,
                                ImPlotCond_Always);
        ImPlot::SetupLegend(ImPlotLocation_East);
        ImPlotSpec spec;
        spec.Flags = ImPlotPieChartFlags_Normalize;
        ImPlot::PlotPieChart(labelPointers.data(), values.data(),
                             static_cast<int>(values.size()), 0.0, 0.0,
                             0.78, "%.0f", 90.0, spec);
        ImPlot::EndPlot();
    }
}

void drawBarBreakdown(const char* title,
                      const std::vector<CategoryCount>& categories) {
    const int total = categoryTotal(categories);
    int maximumCount = 1;
    std::vector<double> values;
    std::vector<double> ticks;
    std::vector<std::string> labels;
    std::vector<const char*> labelPointers;
    values.reserve(categories.size());
    ticks.reserve(categories.size());
    labels.reserve(categories.size());
    labelPointers.reserve(categories.size());

    for (std::size_t index = 0; index < categories.size(); ++index) {
        values.push_back(static_cast<double>(categories[index].count));
        ticks.push_back(static_cast<double>(index));
        labels.push_back(categories[index].label);
        maximumCount = std::max(maximumCount, categories[index].count);
    }
    for (const auto& label : labels)
        labelPointers.push_back(label.c_str());

    constexpr ImPlotFlags flags =
        ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText |
        ImPlotFlags_NoInputs;
    if (ImPlot::BeginPlot(title, ImVec2(-analysisPlotRightGutter, 285),
                          flags)) {
        ImPlot::SetupAxis(ImAxis_X1, "Count",
                          ImPlotAxisFlags_Lock);
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
    if (categories.size() <= 2)
        drawPieBreakdown(title, categories);
    else
        drawBarBreakdown(title, categories);
}

} // namespace

void drawAnalysisView(const GameAnalysisVisualizationModel& model,
                      AnalysisViewState& runtime) {
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

    ImGui::SeparatorText("Game timeline");
    drawTimeline(model, runtime);

    ImGui::SeparatorText("Mechanic breakdowns");
    drawCategoricalBreakdown(
        "Camera Navigation Methods", model.navigationStatus,
        cameraNavigationBreakdown(model),
        "No camera-navigation transitions were detected in this game.");
    drawCategoricalBreakdown(
        "Worker Macro Access Styles", model.workerMacroStatus,
        macroAccessBreakdown(model.workerAccessStyleDurations),
        "No classified worker macro access styles were detected in this game.");
    drawCategoricalBreakdown(
        "Army Macro Access Styles", model.armyMacroStatus,
        macroAccessBreakdown(model.armyAccessStyleDurations),
        "No classified army macro access styles were detected in this game.");
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
        "Breakdowns omit ambiguous Other/Existing Selection observations. "
        "Charts with one or two displayed categories use a pie chart; larger "
        "breakdowns use horizontal frequency bars.");
}

} // namespace smp
