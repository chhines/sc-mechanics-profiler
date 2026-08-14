#include "app/analysis_window.h"

#include "platform/resource_ids.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "implot.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace smp {
namespace {

constexpr wchar_t analysisWindowClass[] = L"StarcraftMechanicsProfilerAnalysisWindow";
constexpr UINT bringAnalysisForwardMessage = WM_APP + 71;

struct D3dResources {
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    IDXGISwapChain* swapChain{};
    ID3D11RenderTargetView* renderTarget{};

    void destroyRenderTarget() noexcept {
        if (renderTarget) {
            renderTarget->Release();
            renderTarget = nullptr;
        }
    }

    void cleanup() noexcept {
        destroyRenderTarget();
        if (swapChain) {
            swapChain->Release();
            swapChain = nullptr;
        }
        if (context) {
            context->Release();
            context = nullptr;
        }
        if (device) {
            device->Release();
            device = nullptr;
        }
    }

    bool createRenderTarget() noexcept {
        ID3D11Texture2D* buffer = nullptr;
        if (!swapChain || FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
            return false;
        const HRESULT result = device->CreateRenderTargetView(buffer, nullptr, &renderTarget);
        buffer->Release();
        return SUCCEEDED(result);
    }
};

struct AnalysisRuntime {
    D3dResources d3d;
    bool imguiReady{};
    bool fitTimeline{};
    bool resetTimeline{};
    bool showNavigation{true};
    bool showWorker{true};
    bool showArmy{true};
    bool showProductionVisits{};
    bool showControlGroupEdits{true};
    bool showScouting{true};
};

bool createD3d(HWND window, D3dResources& d3d) noexcept {
    DXGI_SWAP_CHAIN_DESC swapDescription{};
    swapDescription.BufferCount = 2;
    swapDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.OutputWindow = window;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.Windowed = TRUE;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr std::array<D3D_FEATURE_LEVEL, 2> featureLevels{
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevels.data(), static_cast<UINT>(featureLevels.size()),
        D3D11_SDK_VERSION, &swapDescription, &d3d.swapChain, &d3d.device, &selected, &d3d.context);
    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, featureLevels.data(),
                                               static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
                                               &swapDescription, &d3d.swapChain, &d3d.device, &selected, &d3d.context);
    }
    if (FAILED(result)) {
        d3d.cleanup();
        return false;
    }
    return d3d.createRenderTarget();
}

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
    ImGui::Text("Access latency: %.0f ms", visit.accessLatencyMs);
    ImGui::Text("Time to first attempt: %.0f ms", visit.productionLatencyMs);
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

void drawTimeline(const GameAnalysisVisualizationModel& model, AnalysisRuntime& runtime) {
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
        draw->AddLine(top, bottom, IM_COL32(42, 48, 57, 115), 1.0f);
        const std::string label = formatTime(cursor * 1000.0);
        draw->AddText(ImVec2(top.x + 5.0f, top.y + 3.0f), IM_COL32(38, 45, 54, 220), label.c_str());
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

void drawAccessStyleComparison(const GameAnalysisVisualizationModel& model) {
    std::size_t observationCount = 0;
    double maximumDurationSeconds = 0.0;
    for (const auto& group : model.accessStyleDurations) {
        for (const double durationMs : group.durationMs) {
            ++observationCount;
            maximumDurationSeconds =
                std::max(maximumDurationSeconds, durationMs / 1000.0);
        }
    }
    if (observationCount == 0) {
        ImGui::TextDisabled("No macro access-style timing observations in this game.");
        return;
    }

    std::vector<double> ticks;
    std::vector<std::string> labels;
    std::vector<const char*> labelPointers;
    for (std::size_t index = 0; index < model.accessStyleDurations.size(); ++index) {
        ticks.push_back(static_cast<double>(index));
        labels.push_back(readableName(model.accessStyleDurations[index].accessStyle) +
                         " (n=" + std::to_string(model.accessStyleDurations[index].durationMs.size()) + ")");
    }
    for (const auto& label : labels)
        labelPointers.push_back(label.c_str());

    if (ImPlot::BeginPlot("Macro duration by access style", ImVec2(-1, 280),
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
        for (std::size_t groupIndex = 0; groupIndex < model.accessStyleDurations.size(); ++groupIndex) {
            const auto& group = model.accessStyleDurations[groupIndex];
            for (std::size_t item = 0; item < group.durationMs.size(); ++item) {
                const double jitter =
                    group.durationMs.size() == 1 ? 0.0 : (static_cast<double>(item % 7) - 3.0) * 0.035;
                const ImVec2 point =
                    ImPlot::PlotToPixels(static_cast<double>(groupIndex) + jitter, group.durationMs[item] / 1000.0);
                drawPoint(draw, point, IM_COL32(67, 104, 148, 190), 0);
                if (!tooltipShown && pointHovered(point)) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Access style: %s", group.accessStyle.c_str());
                    ImGui::Text("Cycle duration: %.2f s", group.durationMs[item] / 1000.0);
                    ImGui::Text("Sample count: %zu", group.durationMs.size());
                    ImGui::EndTooltip();
                    tooltipShown = true;
                }
            }
            if (group.medianMs) {
                const double x = static_cast<double>(groupIndex);
                if (group.p25Ms && group.p75Ms) {
                    draw->AddLine(ImPlot::PlotToPixels(x, *group.p25Ms / 1000.0),
                                  ImPlot::PlotToPixels(x, *group.p75Ms / 1000.0), IM_COL32(38, 54, 72, 230), 4.0f);
                }
                draw->AddLine(ImPlot::PlotToPixels(x - 0.18, *group.medianMs / 1000.0),
                              ImPlot::PlotToPixels(x + 0.18, *group.medianMs / 1000.0), IM_COL32(20, 30, 42, 255),
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
        "Points are individual cycles; dark line = median, vertical range = P25-P75, orange dot = P90.");
}

void renderAnalysis(const GameAnalysisVisualizationModel& model, AnalysisRuntime& runtime) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("AnalysisRoot", nullptr, flags);
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
    drawAccessStyleComparison(model);
    ImGui::End();
}

LRESULT CALLBACK analysisWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* runtime = reinterpret_cast<AnalysisRuntime*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        runtime = static_cast<AnalysisRuntime*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(runtime));
    }
    if (runtime && runtime->imguiReady && ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
        return TRUE;

    switch (message) {
    case bringAnalysisForwardMessage:
        if (IsIconic(window))
            ShowWindow(window, SW_RESTORE);
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        return 0;
    case WM_SIZE:
        if (runtime && runtime->d3d.device && wParam != SIZE_MINIMIZED) {
            runtime->d3d.destroyRenderTarget();
            runtime->d3d.swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            runtime->d3d.createRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

AnalysisWindow::~AnalysisWindow() {
    close();
}

void AnalysisWindow::open(HWND owner, GameAnalysisVisualizationModel model) {
    std::lock_guard lock(mutex_);
    if (running_.load(std::memory_order_acquire)) {
        if (const HWND existing = window_.load(std::memory_order_acquire))
            PostMessageW(existing, bringAnalysisForwardMessage, 0, 0);
        return;
    }
    if (thread_.joinable())
        thread_.join();
    closeRequested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&AnalysisWindow::run, this, owner, std::move(model));
}

void AnalysisWindow::close() noexcept {
    std::lock_guard lock(mutex_);
    closeRequested_.store(true, std::memory_order_release);
    if (const HWND window = window_.load(std::memory_order_acquire))
        PostMessageW(window, WM_CLOSE, 0, 0);
    if (thread_.joinable())
        thread_.join();
}

bool AnalysisWindow::isOpen() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void AnalysisWindow::run(HWND owner, GameAnalysisVisualizationModel model) noexcept {
    AnalysisRuntime runtime;
    try {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.style = CS_CLASSDC;
        windowClass.lpfnWndProc = analysisWindowProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hIcon = static_cast<HICON>(LoadImageW(
            windowClass.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32,
            LR_DEFAULTCOLOR));
        windowClass.hIconSm = static_cast<HICON>(LoadImageW(
            windowClass.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16,
            LR_DEFAULTCOLOR));
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = analysisWindowClass;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Could not register the analysis window class");

        RECT ownerRect{100, 100, 1300, 900};
        if (owner)
            GetWindowRect(owner, &ownerRect);
        constexpr int width = 1240;
        constexpr int height = 860;
        const int ownerWidth = static_cast<int>(ownerRect.right - ownerRect.left);
        const int ownerHeight = static_cast<int>(ownerRect.bottom - ownerRect.top);
        const int x = static_cast<int>(ownerRect.left) + std::max(0, (ownerWidth - width) / 2);
        const int y = static_cast<int>(ownerRect.top) + std::max(0, (ownerHeight - height) / 2);
        const HWND window = CreateWindowExW(0, analysisWindowClass,
                                            L"Starcraft Mechanics Profiler - Analysis / Timeline", WS_OVERLAPPEDWINDOW,
                                            x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), &runtime);
        if (!window)
            throw std::runtime_error("Could not create the analysis window");
        window_.store(window, std::memory_order_release);

        if (!createD3d(window, runtime.d3d)) {
            MessageBoxW(owner, L"DirectX 11 could not initialize the analysis window.", L"Analysis unavailable",
                        MB_OK | MB_ICONERROR);
            DestroyWindow(window);
        } else {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImPlot::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.IniFilename = nullptr;
            ImGui::StyleColorsLight();
            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowRounding = 0.0f;
            style.FrameRounding = 3.0f;
            runtime.imguiReady =
                ImGui_ImplWin32_Init(window) && ImGui_ImplDX11_Init(runtime.d3d.device, runtime.d3d.context);
            if (!runtime.imguiReady) {
                MessageBoxW(owner, L"The ImGui analysis renderer could not initialize.", L"Analysis unavailable",
                            MB_OK | MB_ICONERROR);
                DestroyWindow(window);
            } else {
                ShowWindow(window, SW_SHOW);
                UpdateWindow(window);
                if (closeRequested_.load(std::memory_order_acquire))
                    PostMessageW(window, WM_CLOSE, 0, 0);

                bool done = false;
                while (!done) {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        if (message.message == WM_QUIT) {
                            done = true;
                            break;
                        }
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    if (done)
                        break;
                    if (!IsWindowVisible(window) || IsIconic(window)) {
                        WaitMessage();
                        continue;
                    }

                    ImGui_ImplDX11_NewFrame();
                    ImGui_ImplWin32_NewFrame();
                    ImGui::NewFrame();
                    renderAnalysis(model, runtime);
                    ImGui::Render();
                    constexpr float clearColor[4]{0.94f, 0.95f, 0.97f, 1.0f};
                    runtime.d3d.context->OMSetRenderTargets(1, &runtime.d3d.renderTarget, nullptr);
                    runtime.d3d.context->ClearRenderTargetView(runtime.d3d.renderTarget, clearColor);
                    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                    runtime.d3d.swapChain->Present(1, 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            }
        }

        if (runtime.imguiReady) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImPlot::DestroyContext();
            ImGui::DestroyContext();
            runtime.imguiReady = false;
        }
        runtime.d3d.cleanup();
        if (IsWindow(window))
            DestroyWindow(window);
    } catch (const std::exception& error) {
        const std::wstring message(error.what(), error.what() + std::strlen(error.what()));
        MessageBoxW(owner, message.c_str(), L"Analysis unavailable", MB_OK | MB_ICONERROR);
        if (runtime.imguiReady) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImPlot::DestroyContext();
            ImGui::DestroyContext();
        }
        runtime.d3d.cleanup();
    } catch (...) {
        MessageBoxW(owner, L"The analysis window could not be opened.", L"Analysis unavailable", MB_OK | MB_ICONERROR);
        runtime.d3d.cleanup();
    }
    window_.store(nullptr, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

} // namespace smp
