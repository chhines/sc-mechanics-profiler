#include "app/game_analysis_visualization_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <tuple>

namespace smp {
namespace {

constexpr std::array<const char*, 5> accessStyles{
    "control_group_only", "location_hotkey_click", "control_group_center_click", "mixed", "other",
};

std::optional<double> optionalNumber(const json::Value& value) {
    return value.isNumber() ? std::optional<double>(value.asNumber()) : std::nullopt;
}

std::optional<int> optionalInt(const json::Value& value) {
    return value.isNumber() ? std::optional<int>(value.asInt()) : std::nullopt;
}

std::vector<std::string> strings(const json::Value& value) {
    std::vector<std::string> result;
    if (!value.isArray())
        return result;
    result.reserve(value.asArray().size());
    for (const auto& item : value.asArray()) {
        if (item.isString())
            result.push_back(item.asString());
    }
    return result;
}

std::string contextDescription(const json::Value& value) {
    if (!value.isObject())
        return "unknown";
    const std::string kind = value["kind"].asString("unknown");
    if (kind == "control_group" && value["control_group"].isNumber())
        return "control group " + std::to_string(value["control_group"].asInt());
    if (kind == "location_hotkey" && value["location_hotkey"].isNumber())
        return "location hotkey " + std::to_string(value["location_hotkey"].asInt());
    if (kind == "replay_selection" && value["unit_tags"].isArray())
        return "replay selection (" + std::to_string(value["unit_tags"].asArray().size()) + " unit tag(s))";
    return kind;
}

VisualizationTrackStatus statusFromObject(const json::Value& value, const char* absentReason) {
    if (!value.isObject())
        return {false, absentReason};
    if (!value["available"].asBool(false))
        return {false, value["reason"].asString("Marked unavailable by analysis")};
    return {true, {}};
}

void appendMacroCycles(const json::Value& source, const char* productType, std::vector<TimelineMacroCycle>& output) {
    if (!source["cycles"].isArray())
        return;
    output.reserve(source["cycles"].asArray().size());
    for (const auto& cycle : source["cycles"].asArray()) {
        if (!cycle.isObject())
            continue;
        output.push_back(TimelineMacroCycle{
            productType,
            cycle["start_active_ms"].asNumber(),
            cycle["execution_end_active_ms"].asNumber(),
            cycle["end_active_ms"].asNumber(),
            cycle["duration_ms"].asNumber(),
            cycle["full_span_ms"].asNumber(),
            static_cast<std::size_t>(std::max(0, cycle["visit_count"].asInt())),
            cycle["macro_access_style"].asString("other"),
        });
    }
    std::stable_sort(output.begin(), output.end(), [](const auto& left, const auto& right) {
        return std::tie(left.startActiveMs, left.executionEndActiveMs, left.endActiveMs, left.accessStyle) <
               std::tie(right.startActiveMs, right.executionEndActiveMs, right.endActiveMs, right.accessStyle);
    });
}

double percentile(const std::vector<double>& sorted, double quantile) {
    if (sorted.empty())
        return 0.0;
    const double position = quantile * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

void buildAccessStyleDurations(GameAnalysisVisualizationModel& model) {
    for (const char* style : accessStyles) {
        MacroAccessStyleDurationGroup group;
        group.accessStyle = style;
        const auto collect = [&](const std::vector<TimelineMacroCycle>& cycles) {
            for (const auto& cycle : cycles) {
                if (cycle.accessStyle == style)
                    group.durationMs.push_back(cycle.durationMs);
            }
        };
        collect(model.workerMacroCycles);
        collect(model.armyMacroCycles);
        if (group.durationMs.empty())
            continue;
        std::sort(group.durationMs.begin(), group.durationMs.end());
        group.medianMs = percentile(group.durationMs, 0.50);
        group.p25Ms = percentile(group.durationMs, 0.25);
        group.p75Ms = percentile(group.durationMs, 0.75);
        group.p90Ms = percentile(group.durationMs, 0.90);
        model.accessStyleDurations.push_back(std::move(group));
    }
}

double maximumTimelineTime(const GameAnalysisVisualizationModel& model) {
    double maximum = model.activeDurationMs;
    for (const auto& item : model.navigationEvents)
        maximum = std::max(maximum, item.activeMs + item.durationMs);
    for (const auto& item : model.workerMacroCycles)
        maximum = std::max(maximum, item.endActiveMs);
    for (const auto& item : model.armyMacroCycles)
        maximum = std::max(maximum, item.endActiveMs);
    for (const auto& item : model.productionVisits)
        maximum = std::max(maximum, item.endActiveMs);
    for (const auto& item : model.armyControlGroupEdits)
        maximum = std::max(maximum, item.operationActiveMs);
    for (const auto& item : model.scoutingActivities)
        maximum = std::max(maximum, item.lastCommandActiveMs.value_or(item.assignedActiveMs));
    return maximum;
}

std::filesystem::path pairedPath(const std::filesystem::path& selected, const wchar_t* extension) {
    auto result = selected;
    result.replace_extension(extension);
    return result;
}

} // namespace

GameAnalysisVisualizationModel buildGameAnalysisVisualizationModel(const NavSession* nav,
                                                                   const json::Value* derivedJson) {
    GameAnalysisVisualizationModel model;

    if (nav) {
        model.navLoaded = true;
        model.sessionId = nav->sessionId;
        model.activeDurationMs = std::max(0.0, nav->analysis.activeDurationSeconds * 1000.0);
        model.navigationStatus = {true, {}};
        model.navigationEvents.reserve(nav->analysis.navigationEvents.size());
        for (const auto& event : nav->analysis.navigationEvents) {
            model.navigationEvents.push_back(TimelineNavigationEvent{
                event.activeMs,
                event.type,
                event.id,
                event.cursorX,
                event.cursorY,
                event.durationMs,
                event.edgeDirection,
                event.startCursorX,
                event.startCursorY,
            });
        }
        std::stable_sort(model.navigationEvents.begin(), model.navigationEvents.end(),
                         [](const auto& left, const auto& right) {
                             return std::tie(left.activeMs, left.type, left.id, left.durationMs) <
                                    std::tie(right.activeMs, right.type, right.id, right.durationMs);
                         });
    } else {
        model.navigationStatus = {false, "Paired .nav file is unavailable"};
    }

    if (!derivedJson) {
        const VisualizationTrackStatus missing{false, "Paired derived JSON is unavailable"};
        model.workerMacroStatus = missing;
        model.armyMacroStatus = missing;
        model.productionVisitStatus = missing;
        model.controlGroupEditStatus = missing;
        model.scoutingStatus = missing;
        model.activeDurationMs = maximumTimelineTime(model);
        return model;
    }

    model.jsonLoaded = true;
    if (model.sessionId.empty())
        model.sessionId = (*derivedJson)["session"]["id"].asString();
    if ((*derivedJson)["session"]["active_duration_seconds"].isNumber()) {
        model.activeDurationMs =
            std::max(model.activeDurationMs, (*derivedJson)["session"]["active_duration_seconds"].asNumber() * 1000.0);
    }

    const auto& worker = (*derivedJson)["worker_macro_cycles"];
    model.workerMacroStatus = statusFromObject(worker, "Worker macro data is not present");
    if (model.workerMacroStatus.available)
        appendMacroCycles(worker, "worker", model.workerMacroCycles);

    const auto& army = (*derivedJson)["army_macro_cycles"];
    model.armyMacroStatus = statusFromObject(army, "Army macro data is not present");
    if (model.armyMacroStatus.available)
        appendMacroCycles(army, "army", model.armyMacroCycles);

    const auto& visits = (*derivedJson)["production_visits"];
    model.productionVisitStatus = statusFromObject(visits, "Production visit data is not present");
    if (model.productionVisitStatus.available && visits["visits"].isArray()) {
        const auto& sourceVisits = visits["visits"].asArray();
        model.productionVisits.reserve(sourceVisits.size());
        for (std::size_t index = 0; index < sourceVisits.size(); ++index) {
            const auto& visit = sourceVisits[index];
            if (!visit.isObject())
                continue;
            model.productionVisits.push_back(TimelineProductionVisit{
                index,
                visit["product_type"].asString("unknown"),
                visit["start_active_ms"].asNumber(),
                visit["context_active_ms"].asNumber(),
                visit["first_production_active_ms"].asNumber(),
                visit["end_active_ms"].asNumber(),
                visit["selection_access"].asString("unknown"),
                visit["camera_access"].asString("none"),
                optionalInt(visit["control_group"]),
                optionalInt(visit["location_hotkey"]),
                contextDescription(visit["production_context"]),
                strings(visit["produced_units"]),
                visit["physical_production_presses"].asInt(),
                visit["replay_confirmed"].asBool(false),
                visit["access_latency_ms"].asNumber(),
                visit["production_latency_ms"].asNumber(),
            });
        }
        std::stable_sort(model.productionVisits.begin(), model.productionVisits.end(),
                         [](const auto& left, const auto& right) {
                             return std::tie(left.startActiveMs, left.contextActiveMs, left.firstProductionActiveMs,
                                             left.endActiveMs, left.sourceIndex) <
                                    std::tie(right.startActiveMs, right.contextActiveMs, right.firstProductionActiveMs,
                                             right.endActiveMs, right.sourceIndex);
                         });
    }

    const auto& controlGroups = (*derivedJson)["army_control_group_management"];
    model.controlGroupEditStatus = statusFromObject(controlGroups, "Army control-group data is not present");
    model.scoutingStatus = model.controlGroupEditStatus;
    if (model.controlGroupEditStatus.available) {
        if (controlGroups["edits"].isArray()) {
            for (const auto& edit : controlGroups["edits"].asArray()) {
                if (!edit.isObject() || edit["scope"].asString() != "army")
                    continue;
                model.armyControlGroupEdits.push_back(TimelineControlGroupEdit{
                    edit["operation_active_ms"].asNumber(),
                    edit["group"].asInt(-1),
                    edit["operation"].asString("unknown"),
                    edit["selection_method"].asString("unknown"),
                    optionalNumber(edit["selection_to_operation_ms"]),
                    optionalNumber(edit["selection_duration_ms"]),
                    optionalNumber(edit["total_execution_ms"]),
                    edit["scope"].asString(),
                    edit["replay_confirmed"].asBool(false),
                    edit["binding_confidence"].asString("unknown"),
                });
            }
            std::stable_sort(
                model.armyControlGroupEdits.begin(), model.armyControlGroupEdits.end(),
                [](const auto& left, const auto& right) {
                    return std::tie(left.operationActiveMs, left.group, left.operation, left.selectionMethod) <
                           std::tie(right.operationActiveMs, right.group, right.operation, right.selectionMethod);
                });
        }
        if (controlGroups["scouting_unit_activity"].isArray()) {
            for (const auto& activity : controlGroups["scouting_unit_activity"].asArray()) {
                if (!activity.isObject())
                    continue;
                model.scoutingActivities.push_back(TimelineScoutingActivity{
                    activity["group"].asInt(-1),
                    static_cast<std::uint32_t>(std::max(0, activity["assignment_generation"].asInt())),
                    activity["assigned_active_ms"].asNumber(),
                    optionalNumber(activity["last_command_active_ms"]),
                    optionalNumber(activity["activity_duration_ms"]),
                    static_cast<std::size_t>(std::max(0, activity["selection_count"].asInt())),
                    static_cast<std::size_t>(std::max(0, activity["command_count"].asInt())),
                });
            }
            std::stable_sort(model.scoutingActivities.begin(), model.scoutingActivities.end(),
                             [](const auto& left, const auto& right) {
                                 return std::tie(left.assignedActiveMs, left.group, left.assignmentGeneration) <
                                        std::tie(right.assignedActiveMs, right.group, right.assignmentGeneration);
                             });
        }
    }

    buildAccessStyleDurations(model);
    model.activeDurationMs = maximumTimelineTime(model);
    return model;
}

GameAnalysisVisualizationModel loadGameAnalysisVisualizationModel(const std::filesystem::path& selectedResultPath) {
    GameAnalysisVisualizationModel result;
    result.navPath = pairedPath(selectedResultPath, L".nav");
    result.jsonPath = pairedPath(selectedResultPath, L".json");

    std::optional<NavSession> nav;
    std::optional<json::Value> derived;
    std::string navFailure;
    std::string jsonFailure;
    try {
        if (std::filesystem::is_regular_file(result.navPath))
            nav = readNavSession(result.navPath);
        else
            navFailure = "Paired .nav file was not found";
    } catch (const std::exception& error) {
        navFailure = std::string("Could not read paired .nav: ") + error.what();
    }
    try {
        if (std::filesystem::is_regular_file(result.jsonPath))
            derived = json::parseFile(result.jsonPath);
        else
            jsonFailure = "Paired derived JSON was not found";
    } catch (const std::exception& error) {
        jsonFailure = std::string("Could not read paired JSON: ") + error.what();
    }

    auto built = buildGameAnalysisVisualizationModel(nav ? &*nav : nullptr, derived ? &*derived : nullptr);
    built.navPath = result.navPath;
    built.jsonPath = result.jsonPath;
    if (!navFailure.empty())
        built.navigationStatus.reason = navFailure;
    if (!jsonFailure.empty()) {
        built.workerMacroStatus.reason = jsonFailure;
        built.armyMacroStatus.reason = jsonFailure;
        built.productionVisitStatus.reason = jsonFailure;
        built.controlGroupEditStatus.reason = jsonFailure;
        built.scoutingStatus.reason = jsonFailure;
    }
    return built;
}

} // namespace smp
