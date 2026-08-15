#include "app/analysis_view.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace smp {
namespace {

std::string formatTime(double activeMs, bool milliseconds = true) {
    activeMs = std::max(0.0, activeMs);
    const auto totalWholeSeconds = static_cast<long long>(activeMs / 1000.0);
    const auto minutes = totalWholeSeconds / 60;
    const double seconds = activeMs / 1000.0 - static_cast<double>(minutes * 60);
    char buffer[64]{};
    if (milliseconds)
        std::snprintf(buffer, sizeof(buffer), "%lld:%06.3f", minutes, seconds);
    else
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld", minutes, totalWholeSeconds % 60);
    return buffer;
}

int timeAxisFormatter(double seconds, char* buffer, int size, void*) {
    const auto label = formatTime(seconds * 1000.0, false);
    return std::snprintf(buffer, static_cast<std::size_t>(size), "%s", label.c_str());
}

std::string readableName(std::string value) {
    bool capitalize = true;
    for (char& character : value) {
        if (character == '_') {
            character = ' ';
            capitalize = true;
        } else if (capitalize) {
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
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
    return mouse.x >= first.x - padding && mouse.x <= second.x + padding && mouse.y >= first.y - padding &&
           mouse.y <= second.y + padding;
}

void addFilledRect(ImDrawList* draw, ImVec2 first, ImVec2 second, ImU32 color,
                   float rounding = 0.0f) {
    const ImVec2 minimum{std::min(first.x, second.x), std::min(first.y, second.y)};
    const ImVec2 maximum{std::max(first.x, second.x), std::max(first.y, second.y)};
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
    ImGui::Text("Execution complete: %s", formatTime(cycle.executionEndActiveMs).c_str());
    ImGui::Text("Duration: %.2f s", cycle.durationMs / 1000.0);
    ImGui::Text("Full span: %.2f s", cycle.fullSpanMs / 1000.0);
    ImGui::Text("Contexts: %zu", cycle.visitCount);
    ImGui::Text("Access style: %s", cycle.accessStyle.c_str());
    ImGui::EndTooltip();
}

void tooltipVisit(const TimelineProductionVisit& visit) {
    ImGui::BeginTooltip();
    ImGui::Text("%s production visit", readableName(visit.productType).c_str());
    ImGui::Text("Start: %s", formatTime(visit.startActiveMs).c_str());
    ImGui::Text("Context: %s", formatTime(visit.contextActiveMs).c_str());
    ImGui::Text("First production attempt: %s", formatTime(visit.firstProductionActiveMs).c_str());
    ImGui::Text("End: %s", formatTime(visit.endActiveMs).c_str());
    ImGui::Separator();
    ImGui::Text("Selection access: %s", visit.selectionAccess.c_str());
    ImGui::Text("Camera access: %s", visit.cameraAccess.c_str());
    if (visit.controlGroup)
        ImGui::Text("Control group: %d", *visit.controlGroup);
    if (visit.locationHotkey)
        ImGui::Text("Location hotkey: %d", *visit.locationHotkey);
    ImGui::Text("Production context: %s", visit.productionContext.c_str());
    ImGui::TextWrapped("Produced units: %s", joined(visit.producedUnits).c_str());
    ImGui::Text("Physical production presses: %d", visit.physicalProductionPresses);
    ImGui::Text("Replay confirmation: %s", visit.replayConfirmed ? "yes" : "no");
    ImGui::Text("Access to context: %.0f ms", visit.accessLatencyMs);
    ImGui::Text("Production response latency: %.0f ms", visit.productionLatencyMs);
    ImGui::Text("Time to first attempt: %.0f ms", visit.executionDurationMs);
    ImGui::EndTooltip();
}

void tooltipControlGroup(const TimelineControlGroupEdit& edit) {
    ImGui::BeginTooltip();
    ImGui::Text("Time: %s", formatTime(edit.operationActiveMs).c_str());
    ImGui::Text("Group %d %s", edit.group, readableName(edit.operation).c_str());
    ImGui::Text("Selection: %s", edit.selectionMethod.c_str());
    if (edit.selectionToOperationMs)
        ImGui::Text("Selection -> group: %.0f ms", *edit.selectionToOperationMs);
    else
        ImGui::TextUnformatted("Selection -> group: unavailable");
    if (edit.selectionDurationMs)
        ImGui::Text("Selection duration: %.0f ms", *edit.selectionDurationMs);
    if (edit.totalExecutionMs)
        ImGui::Text("Total execution: %.0f ms", *edit.totalExecutionMs);
    ImGui::Text("Scope: %s", edit.scope.c_str());
    ImGui::Text("Replay confirmation: %s (%s)", edit.replayConfirmed ? "yes" : "no", edit.bindingConfidence.c_str());
    ImGui::EndTooltip();
}

void tooltipScouting(const TimelineScoutingActivity& activity) {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted("Observed scouting activity");
    ImGui::Text("Control group: %d", activity.group);
    ImGui::Text("Assignment generation: %u", activity.assignmentGeneration);
    ImGui::Text("Assigned at: %s", formatTime(activity.assignedActiveMs).c_str());
    if (activity.lastCommandActiveMs)
        ImGui::Text("Last commanded at: %s", formatTime(*activity.lastCommandActiveMs).c_str());
    else
        ImGui::TextUnformatted("Last commanded at: no attributed command");
    if (activity.activityDurationMs)
        ImGui::Text("Activity duration: %.2f s", *activity.activityDurationMs / 1000.0);
    ImGui::Text("Selections: %zu", activity.selectionCount);
    ImGui::Text("Commands: %zu", activity.commandCount);
    ImGui::TextDisabled("This is observed activity, not unit lifetime or survival time.");
    ImGui::EndTooltip();
}

void drawPoint(ImDrawList* draw, const ImVec2& point, ImU32 color, int shape) {
    if (shape == 1) {
        draw->AddRectFilled(ImVec2(point.x - 4, point.y - 4), ImVec2(point.x + 4, point.y + 4), color, 1.0f);
    } else if (shape == 2) {
        draw->AddQuadFilled(ImVec2(point.x, point.y - 5), ImVec2(point.x + 5, point.y), ImVec2(point.x, point.y + 5),
                            ImVec2(point.x - 5, point.y), color);
    } else {
        draw->AddCircleFilled(point, 4.5f, color, 16);
    }
}

void showTrackStatus(const char* label, bool& enabled, const VisualizationTrackStatus& status) {
    ImGui::BeginDisabled(!status.available);
    ImGui::Checkbox(label, &enabled);
    ImGui::EndDisabled();
    if (!status.available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Unavailable: %s", status.reason.c_str());
}

void drawTimeline(const GameAnalysisVisualizationModel& model, AnalysisViewState& runtime) {
    ImGui::TextUnformatted("Tracks");
    showTrackStatus("Camera navigation", runtime.showNavigation, model.navigationStatus);
    ImGui::SameLine();
    showTrackStatus("Worker macro", runtime.showWorker, model.workerMacroStatus);
    ImGui::SameLine();
    showTrackStatus("Army macro", runtime.showArmy, model.armyMacroStatus);
    ImGui::SameLine();
    showTrackStatus("Production visits", runtime.showProductionVisits, model.productionVisitStatus);
    showTrackStatus("Army control-group edits", runtime.showControlGroupEdits, model.controlGroupEditStatus);
    ImGui::SameLine();
    showTrackStatus("Scouting activity", runtime.showScouting, model.scoutingStatus);
    ImGui::SameLine();
    if (ImGui::Button("Fit game"))
        runtime.fitTimeline = true;
    ImGui::SameLine();
    if (ImGui::Button("Reset view"))
        runtime.resetTimeline = true;
    ImGui::TextDisabled(
        "Camera: control group (blue circle), location (teal square), minimap (gold diamond), "
        "edge pan (gray interval). Macro faded tails are observed span after execution completion.");
    if (runtime.showProductionVisits)
        ImGui::TextDisabled(
            "Visit stages: start circle -> context square -> first attempt diamond -> end circle.");

    const double gameSeconds = std::max(1.0, model.activeDurationMs / 1000.0);
    if (runtime.fitTimeline || runtime.resetTimeline)
        ImPlot::SetNextAxisLimits(ImAxis_X1, 0.0, gameSeconds, ImPlotCond_Always);

    if (!ImPlot::BeginPlot("Full-game mechanics timeline", ImVec2(-1, 390),
                           ImPlotFlags_NoLegend | ImPlotFlags_Crosshairs | ImPlotFlags_NoMouseText)) {
        runtime.fitTimeline = false;
        runtime.resetTimeline = false;
        return;
    }
    runtime.fitTimeline = false;
    runtime.resetTimeline = false;

    ImPlot::SetupAxis(ImAxis_X1, "Active game time");
    ImPlot::SetupAxisFormat(ImAxis_X1, timeAxisFormatter);
    ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, gameSeconds, ImPlotCond_Once);
    ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, gameSeconds);
    ImPlot::SetupAxisZoomConstraints(ImAxis_X1, std::min(2.0, gameSeconds), gameSeconds);
    ImPlot::SetupAxis(ImAxis_Y1, nullptr,
                      ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_Lock);
    ImPlot::SetupAxisLimits(ImAxis_Y1, -0.55, 5.55, ImPlotCond_Always);
    const double trackTicks[]{0, 1, 2, 3, 4, 5};
    const char* trackLabels[]{"Scouting",   "Army CG edits", "Production visits",
                              "Army macro", "Worker macro",  "Camera navigation"};
    ImPlot::SetupAxisTicks(ImAxis_Y1, trackTicks, 6, trackLabels, false);
    ImPlot::SetupFinish();

    auto* draw = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    bool tooltipShown = false;

    if (runtime.showNavigation && model.navigationStatus.available) {
        for (const auto& event : model.navigationEvents) {
            const double start = event.activeMs / 1000.0;
            if (event.type == CameraNavigationType::EdgeScroll && event.durationMs > 0.0) {
                const ImVec2 first = ImPlot::PlotToPixels(start, 5.0 - 0.13);
                const ImVec2 second = ImPlot::PlotToPixels((event.activeMs + event.durationMs) / 1000.0, 5.0 + 0.13);
                addFilledRect(draw, first, second, IM_COL32(93, 113, 130, 190), 2.0f);
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

    const auto drawCycles = [&](const std::vector<TimelineMacroCycle>& cycles, double y, ImU32 body, ImU32 tail) {
        for (const auto& cycle : cycles) {
            const ImVec2 first = ImPlot::PlotToPixels(cycle.startActiveMs / 1000.0, y - 0.15);
            const ImVec2 execution = ImPlot::PlotToPixels(cycle.executionEndActiveMs / 1000.0, y + 0.15);
            addFilledRect(draw, first, execution, body, 2.0f);
            const ImVec2 executionCenter = ImPlot::PlotToPixels(cycle.executionEndActiveMs / 1000.0, y);
            const ImVec2 end = ImPlot::PlotToPixels(cycle.endActiveMs / 1000.0, y);
            if (cycle.endActiveMs > cycle.executionEndActiveMs)
                draw->AddLine(executionCenter, end, tail, 3.0f);
            const ImVec2 entireEnd = ImPlot::PlotToPixels(cycle.endActiveMs / 1000.0, y + 0.15);
            if (!tooltipShown && intervalHovered(first, entireEnd)) {
                tooltipMacro(cycle);
                tooltipShown = true;
            }
        }
    };
    if (runtime.showWorker && model.workerMacroStatus.available)
        drawCycles(model.workerMacroCycles, 4.0, IM_COL32(29, 137, 132, 220), IM_COL32(29, 137, 132, 95));
    if (runtime.showArmy && model.armyMacroStatus.available)
        drawCycles(model.armyMacroCycles, 3.0, IM_COL32(118, 82, 160, 220), IM_COL32(118, 82, 160, 95));

    if (runtime.showProductionVisits && model.productionVisitStatus.available) {
        for (const auto& visit : model.productionVisits) {
            const ImVec2 start = ImPlot::PlotToPixels(visit.startActiveMs / 1000.0, 2.0);
            const ImVec2 context = ImPlot::PlotToPixels(visit.contextActiveMs / 1000.0, 2.0);
            const ImVec2 production = ImPlot::PlotToPixels(visit.firstProductionActiveMs / 1000.0, 2.0);
            const ImVec2 end = ImPlot::PlotToPixels(visit.endActiveMs / 1000.0, 2.0);
            draw->AddLine(start, end, IM_COL32(104, 112, 122, 190), 2.0f);
            drawPoint(draw, start, IM_COL32(104, 112, 122, 255), 0);
            drawPoint(draw, context, IM_COL32(45, 105, 180, 255), 1);
            drawPoint(draw, production, IM_COL32(214, 145, 45, 255), 2);
            drawPoint(draw, end, IM_COL32(78, 86, 96, 255), 0);
            if (!tooltipShown && intervalHovered(start, end, 7.0f)) {
                tooltipVisit(visit);
                tooltipShown = true;
            }
        }
    }

    if (runtime.showControlGroupEdits && model.controlGroupEditStatus.available) {
        for (const auto& edit : model.armyControlGroupEdits) {
            const ImVec2 point = ImPlot::PlotToPixels(edit.operationActiveMs / 1000.0, 1.0);
            drawPoint(draw, point, edit.operation == "add" ? IM_COL32(193, 106, 47, 255) : IM_COL32(72, 96, 145, 255),
                      edit.operation == "add" ? 2 : 0);
            if (!tooltipShown && pointHovered(point)) {
                tooltipControlGroup(edit);
                tooltipShown = true;
            }
        }
    }

    if (runtime.showScouting && model.scoutingStatus.available) {
        for (const auto& activity : model.scoutingActivities) {
            const ImVec2 assigned = ImPlot::PlotToPixels(activity.assignedActiveMs / 1000.0, 0.0);
            drawPoint(draw, assigned, IM_COL32(39, 126, 153, 255), 1);
            ImVec2 ending = assigned;
            if (activity.lastCommandActiveMs) {
                ending = ImPlot::PlotToPixels(*activity.lastCommandActiveMs / 1000.0, 0.0);
                draw->AddLine(assigned, ending, IM_COL32(39, 126, 153, 185), 5.0f);
                drawPoint(draw, ending, IM_COL32(25, 85, 106, 255), 2);
            }
            if (!tooltipShown && intervalHovered(assigned, ending, 7.0f)) {
                tooltipScouting(activity);
                tooltipShown = true;
            }
        }
    }

    if (ImPlot::IsPlotHovered()) {
        const double cursor = std::clamp(ImPlot::GetPlotMousePos().x, 0.0, gameSeconds);
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

bool scatterPointHovered(double x, double y) {
    return pointHovered(ImPlot::PlotToPixels(x, y), 7.0f);
}

void drawMacroScatter(const GameAnalysisVisualizationModel& model) {
    if (!model.workerMacroStatus.available && !model.armyMacroStatus.available) {
        ImGui::TextDisabled("Unavailable: paired derived macro-cycle data is unavailable.");
        return;
    }
    if (model.workerMacroCycles.empty() && model.armyMacroCycles.empty()) {
        ImGui::TextDisabled("No macro-cycle observations in this game.");
        return;
    }
    const double gameSeconds = std::max(1.0, model.activeDurationMs / 1000.0);
    if (ImPlot::BeginPlot("Macro cycle duration over game", ImVec2(-1, 260),
                          ImPlotFlags_Crosshairs | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, "Cycle start (active game time)");
        ImPlot::SetupAxisFormat(ImAxis_X1, timeAxisFormatter);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, gameSeconds, ImPlotCond_Once);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, gameSeconds);
        ImPlot::SetupAxis(ImAxis_Y1, "Execution duration (seconds)");
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);

        const auto plotSeries = [](const char* label, const std::vector<TimelineMacroCycle>& cycles,
                                   const ImVec4& color, ImPlotMarker marker) {
            if (cycles.empty())
                return;
            std::vector<double> x;
            std::vector<double> y;
            x.reserve(cycles.size());
            y.reserve(cycles.size());
            for (const auto& cycle : cycles) {
                x.push_back(cycle.startActiveMs / 1000.0);
                y.push_back(cycle.durationMs / 1000.0);
            }
            ImPlotSpec spec;
            spec.LineColor = color;
            spec.MarkerFillColor = color;
            spec.MarkerLineColor = color;
            spec.Marker = marker;
            spec.MarkerSize = 5.0f;
            ImPlot::PlotScatter(label, x.data(), y.data(), static_cast<int>(x.size()), spec);
        };
        plotSeries("Worker", model.workerMacroCycles, ImVec4(0.11f, 0.54f, 0.52f, 1.0f), ImPlotMarker_Circle);
        plotSeries("Army", model.armyMacroCycles, ImVec4(0.46f, 0.32f, 0.63f, 1.0f), ImPlotMarker_Diamond);

        bool tooltipShown = false;
        const auto tooltipSeries = [&](const std::vector<TimelineMacroCycle>& cycles) {
            for (const auto& cycle : cycles) {
                if (!tooltipShown && scatterPointHovered(cycle.startActiveMs / 1000.0, cycle.durationMs / 1000.0)) {
                    tooltipMacro(cycle);
                    tooltipShown = true;
                }
            }
        };
        tooltipSeries(model.workerMacroCycles);
        tooltipSeries(model.armyMacroCycles);
        ImPlot::EndPlot();
    }
}

void drawControlGroupLatencyScatter(const GameAnalysisVisualizationModel& model) {
    if (!model.controlGroupEditStatus.available) {
        ImGui::TextDisabled("Unavailable: %s", model.controlGroupEditStatus.reason.c_str());
        return;
    }
    std::vector<const TimelineControlGroupEdit*> assignments;
    std::vector<const TimelineControlGroupEdit*> additions;
    for (const auto& edit : model.armyControlGroupEdits) {
        if (!edit.selectionToOperationMs)
            continue;
        (edit.operation == "add" ? additions : assignments).push_back(&edit);
    }
    if (assignments.empty() && additions.empty()) {
        ImGui::TextDisabled("No Army control-group edits have selection-to-operation latency.");
        return;
    }
    const double gameSeconds = std::max(1.0, model.activeDurationMs / 1000.0);
    if (ImPlot::BeginPlot("Army control-group edit latency", ImVec2(-1, 250),
                          ImPlotFlags_Crosshairs | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, "Operation time (active game time)");
        ImPlot::SetupAxisFormat(ImAxis_X1, timeAxisFormatter);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, gameSeconds, ImPlotCond_Once);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, gameSeconds);
        ImPlot::SetupAxis(ImAxis_Y1, "Selection -> operation (ms)");
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        const auto plot = [](const char* label, const std::vector<const TimelineControlGroupEdit*>& edits,
                             const ImVec4& color, ImPlotMarker marker) {
            if (edits.empty())
                return;
            std::vector<double> x;
            std::vector<double> y;
            for (const auto* edit : edits) {
                x.push_back(edit->operationActiveMs / 1000.0);
                y.push_back(*edit->selectionToOperationMs);
            }
            ImPlotSpec spec;
            spec.LineColor = color;
            spec.MarkerFillColor = color;
            spec.MarkerLineColor = color;
            spec.Marker = marker;
            spec.MarkerSize = 5.0f;
            ImPlot::PlotScatter(label, x.data(), y.data(), static_cast<int>(x.size()), spec);
        };
        plot("Assign", assignments, ImVec4(0.28f, 0.40f, 0.64f, 1.0f), ImPlotMarker_Circle);
        plot("Add", additions, ImVec4(0.76f, 0.42f, 0.18f, 1.0f), ImPlotMarker_Diamond);
        bool tooltipShown = false;
        for (const auto& edit : model.armyControlGroupEdits) {
            if (!edit.selectionToOperationMs || tooltipShown)
                continue;
            if (scatterPointHovered(edit.operationActiveMs / 1000.0, *edit.selectionToOperationMs)) {
                tooltipControlGroup(edit);
                tooltipShown = true;
            }
        }
        ImPlot::EndPlot();
    }
}

void drawAccessStyleComparison(
    const char* plotTitle,
    const char* productLabel,
    const VisualizationTrackStatus& status,
    const std::vector<MacroAccessStyleDurationGroup>& groups,
    ImU32 pointColor) {
    if (!status.available) {
        ImGui::TextDisabled("%s unavailable: %s", productLabel,
                            status.reason.c_str());
        return;
    }
    std::size_t observationCount = 0;
    double maximumDurationSeconds = 0.0;
    for (const auto& group : groups) {
        for (const double durationMs : group.durationMs) {
            ++observationCount;
            maximumDurationSeconds =
                std::max(maximumDurationSeconds, durationMs / 1000.0);
        }
    }
    if (observationCount == 0) {
        ImGui::TextDisabled("No %s macro access-style timing observations in this game.",
                            productLabel);
        return;
    }

    std::vector<double> ticks;
    std::vector<std::string> labels;
    std::vector<const char*> labelPointers;
    for (std::size_t index = 0; index < groups.size(); ++index) {
        ticks.push_back(static_cast<double>(index));
        labels.push_back(readableName(groups[index].accessStyle) +
                         " (n=" +
                         std::to_string(groups[index].durationMs.size()) + ")");
    }
    for (const auto& label : labels)
        labelPointers.push_back(label.c_str());

    if (ImPlot::BeginPlot(plotTitle, ImVec2(-1, 280),
                          ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, "Macro access style");
        ImPlot::SetupAxisTicks(ImAxis_X1, ticks.data(), static_cast<int>(ticks.size()), labelPointers.data(), false);
        ImPlot::SetupAxisLimits(ImAxis_X1, -0.6, static_cast<double>(ticks.size()) - 0.4, ImPlotCond_Always);
        ImPlot::SetupAxis(ImAxis_Y1, "Cycle duration (seconds)");
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0,
                                std::max(0.1, maximumDurationSeconds * 1.10),
                                ImPlotCond_Once);
        ImPlot::SetupFinish();
        auto* draw = ImPlot::GetPlotDrawList();
        ImPlot::PushPlotClipRect();
        bool tooltipShown = false;
        for (std::size_t groupIndex = 0; groupIndex < groups.size();
             ++groupIndex) {
            const auto& group = groups[groupIndex];
            for (std::size_t item = 0; item < group.durationMs.size(); ++item) {
                const double jitter =
                    group.durationMs.size() == 1 ? 0.0 : (static_cast<double>(item % 7) - 3.0) * 0.035;
                const ImVec2 point =
                    ImPlot::PlotToPixels(static_cast<double>(groupIndex) + jitter, group.durationMs[item] / 1000.0);
                drawPoint(draw, point, pointColor, 0);
                if (!tooltipShown && pointHovered(point)) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Product: %s", productLabel);
                    ImGui::Text("Access style: %s", group.accessStyle.c_str());
                    ImGui::Text("Cycle duration: %.2f s", group.durationMs[item] / 1000.0);
                    ImGui::Text("Sample count: %zu", group.durationMs.size());
                    if (group.durationMs.size() >= 2) {
                        ImGui::Text("Median: %.2f s", *group.medianMs / 1000.0);
                        ImGui::Text("P25-P75: %.2f-%.2f s",
                                    *group.p25Ms / 1000.0,
                                    *group.p75Ms / 1000.0);
                        ImGui::Text("P90: %.2f s", *group.p90Ms / 1000.0);
                    } else {
                        ImGui::TextDisabled(
                            "Single observation; distribution range is not shown.");
                    }
                    ImGui::EndTooltip();
                    tooltipShown = true;
                }
            }
            if (group.durationMs.size() >= 2 && group.medianMs) {
                const double x = static_cast<double>(groupIndex);
                if (group.p25Ms && group.p75Ms) {
                    draw->AddLine(ImPlot::PlotToPixels(x, *group.p25Ms / 1000.0),
                                  ImPlot::PlotToPixels(x, *group.p75Ms / 1000.0),
                                  IM_COL32(154, 174, 198, 230), 4.0f);
                }
                draw->AddLine(ImPlot::PlotToPixels(x - 0.18, *group.medianMs / 1000.0),
                              ImPlot::PlotToPixels(x + 0.18, *group.medianMs / 1000.0),
                              IM_COL32(224, 231, 240, 255),
                              2.0f);
                if (group.p90Ms)
                    draw->AddCircleFilled(ImPlot::PlotToPixels(x, *group.p90Ms / 1000.0), 3.0f,
                                          IM_COL32(193, 106, 47, 230), 12);
            }
        }
        ImPlot::PopPlotClipRect();
        ImPlot::EndPlot();
    }
    ImGui::TextDisabled(
        "Points are individual %s cycles; for n >= 2, horizontal line = median, "
        "vertical range = P25-P75, orange dot = P90.",
        productLabel);
}

} // namespace

void drawAnalysisView(const GameAnalysisVisualizationModel& model,
                      AnalysisViewState& runtime) {
    ImGui::Text("Latest Game Analysis%s%s", model.sessionId.empty() ? "" : " - ", model.sessionId.c_str());
    ImGui::TextDisabled("Read-only visualization from the paired .nav and derived .json files.");
    if (!model.navLoaded)
        ImGui::TextColored(ImVec4(0.72f, 0.40f, 0.12f, 1.0f), "Navigation unavailable: %s",
                           model.navigationStatus.reason.c_str());
    if (!model.jsonLoaded)
        ImGui::TextColored(ImVec4(0.72f, 0.40f, 0.12f, 1.0f), "Derived analysis unavailable: %s",
                           model.workerMacroStatus.reason.c_str());

    ImGui::SeparatorText("Game timeline");
    drawTimeline(model, runtime);
    ImGui::SeparatorText("Timing relationships");
    drawMacroScatter(model);
    drawControlGroupLatencyScatter(model);
    drawAccessStyleComparison(
        "Worker macro duration by access style", "Worker",
        model.workerMacroStatus, model.workerAccessStyleDurations,
        IM_COL32(29, 137, 132, 190));
    drawAccessStyleComparison(
        "Army macro duration by access style", "Army",
        model.armyMacroStatus, model.armyAccessStyleDurations,
        IM_COL32(118, 82, 160, 190));
}

} // namespace smp
