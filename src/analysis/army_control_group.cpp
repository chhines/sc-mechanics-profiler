#include "analysis/army_control_group.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace smp {
namespace {

struct SelectionAcquisition {
    ArmySelectionMethod method{ArmySelectionMethod::Other};
    std::uint64_t startQpc{};
    std::uint64_t completeQpc{};
    double completeActiveMs{};
    int x{};
    int y{};
};

std::optional<double> qpcMilliseconds(std::uint64_t start, std::uint64_t end,
                                      std::uint64_t frequency) noexcept {
    if (frequency == 0 || end < start)
        return std::nullopt;
    return static_cast<double>(static_cast<long double>(end - start) * 1000.0L /
                               static_cast<long double>(frequency));
}

bool closeCoordinates(const SelectionAcquisition& first, const MechanicalInputEvent& second,
                      int maximumDistancePixels) {
    return std::abs(first.x - second.cursorX) <= maximumDistancePixels &&
           std::abs(first.y - second.cursorY) <= maximumDistancePixels;
}

bool shiftSelectionMethod(ArmySelectionMethod method) noexcept {
    return method == ArmySelectionMethod::ShiftClickModify ||
           method == ArmySelectionMethod::ShiftBoxModify ||
           method == ArmySelectionMethod::CtrlShiftClickType;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty())
        return 0.0;
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * weight;
}

std::optional<double> mean(const std::vector<double>& values) {
    if (values.empty())
        return std::nullopt;
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
}

ArmyControlGroupMethodStatistics summarize(const std::vector<const ArmyControlGroupEdit*>& edits) {
    ArmyControlGroupMethodStatistics result;
    result.editCount = edits.size();
    std::vector<double> latency;
    std::vector<double> duration;
    std::vector<double> total;
    for (const auto* edit : edits) {
        if (edit->selectionToOperationMs)
            latency.push_back(*edit->selectionToOperationMs);
        if (edit->selectionDurationMs)
            duration.push_back(*edit->selectionDurationMs);
        if (edit->totalExecutionMs)
            total.push_back(*edit->totalExecutionMs);
    }
    result.averageSelectionToOperationMs = mean(latency);
    result.averageSelectionDurationMs = mean(duration);
    result.averageTotalExecutionMs = mean(total);
    if (!latency.empty()) {
        std::sort(latency.begin(), latency.end());
        result.bestSelectionToOperationMs = latency.front();
        result.medianSelectionToOperationMs = percentile(latency, 0.50);
        result.p25SelectionToOperationMs = percentile(latency, 0.25);
        result.p75SelectionToOperationMs = percentile(latency, 0.75);
        result.p90SelectionToOperationMs = percentile(latency, 0.90);
    }
    return result;
}

} // namespace

const char* armyControlGroupOperationName(ArmyControlGroupOperation operation) noexcept {
    return operation == ArmyControlGroupOperation::Assign ? "assign" : "add";
}

const char* armySelectionMethodName(ArmySelectionMethod method) noexcept {
    switch (method) {
    case ArmySelectionMethod::DirectClick: return "direct_click";
    case ArmySelectionMethod::BoxSelect: return "box_select";
    case ArmySelectionMethod::CtrlClickType: return "ctrl_click_type";
    case ArmySelectionMethod::DoubleClickType: return "double_click_type";
    case ArmySelectionMethod::ShiftClickModify: return "shift_click_modify";
    case ArmySelectionMethod::ShiftBoxModify: return "shift_box_modify";
    case ArmySelectionMethod::CtrlShiftClickType: return "ctrl_shift_click_type";
    case ArmySelectionMethod::ExistingSelection: return "existing_selection";
    case ArmySelectionMethod::Other: return "other";
    }
    return "other";
}

const char* armySelectionMethodLabel(ArmySelectionMethod method) noexcept {
    switch (method) {
    case ArmySelectionMethod::DirectClick: return "Direct click";
    case ArmySelectionMethod::BoxSelect: return "Box select";
    case ArmySelectionMethod::CtrlClickType: return "Ctrl-click type";
    case ArmySelectionMethod::DoubleClickType: return "Double-click type";
    case ArmySelectionMethod::ShiftClickModify: return "Shift-click modify";
    case ArmySelectionMethod::ShiftBoxModify: return "Shift-box modify";
    case ArmySelectionMethod::CtrlShiftClickType: return "Ctrl+Shift-click type";
    case ArmySelectionMethod::ExistingSelection: return "Existing selection";
    case ArmySelectionMethod::Other: return "Other";
    }
    return "Other";
}

std::size_t armySelectionMethodIndex(ArmySelectionMethod method) noexcept {
    const auto index = static_cast<std::size_t>(method);
    return index < armySelectionMethodCount ? index : armySelectionMethodCount - 1;
}

const char* armyControlGroupBindingConfidenceName(
    ArmyControlGroupBindingConfidence confidence) noexcept {
    switch (confidence) {
    case ArmyControlGroupBindingConfidence::PhysicalOnly: return "physical_only";
    case ArmyControlGroupBindingConfidence::ReplayConfirmed: return "replay_confirmed";
    case ArmyControlGroupBindingConfidence::Ambiguous: return "ambiguous";
    }
    return "ambiguous";
}

const char* armyControlGroupScopeName(ArmyControlGroupScope scope) noexcept {
    switch (scope) {
    case ArmyControlGroupScope::Army: return "army";
    case ArmyControlGroupScope::ProductionBuilding: return "production_building";
    case ArmyControlGroupScope::ScoutingUnit: return "scouting_unit";
    case ArmyControlGroupScope::Uncertain: return "uncertain";
    }
    return "uncertain";
}

double ArmyControlGroupAnalysis::assignmentsPerMinute() const noexcept {
    return activeDurationSeconds > 0.0
               ? static_cast<double>(assignments) / (activeDurationSeconds / 60.0)
               : 0.0;
}

double ArmyControlGroupAnalysis::additionsPerMinute() const noexcept {
    return activeDurationSeconds > 0.0
               ? static_cast<double>(additions) / (activeDurationSeconds / 60.0)
               : 0.0;
}

double ArmyControlGroupAnalysis::editsPerMinute() const noexcept {
    return activeDurationSeconds > 0.0
               ? static_cast<double>(assignments + additions) /
                     (activeDurationSeconds / 60.0)
               : 0.0;
}

double ArmyControlGroupAnalysis::assignPercentage() const noexcept {
    const auto total = assignments + additions;
    return total > 0 ? static_cast<double>(assignments) * 100.0 /
                           static_cast<double>(total)
                     : 0.0;
}

double ArmyControlGroupAnalysis::addPercentage() const noexcept {
    const auto total = assignments + additions;
    return total > 0 ? static_cast<double>(additions) * 100.0 /
                           static_cast<double>(total)
                     : 0.0;
}

ArmyControlGroupAnalysis detectArmyControlGroupManagement(const AnalysisResult& result,
                                                          std::uint64_t qpcFrequency,
                                                          const ArmyControlGroupDetectionConfig& config) {
    ArmyControlGroupAnalysis analysis;
    analysis.activeDurationSeconds = result.activeDurationSeconds;
    analysis.available = qpcFrequency != 0;
    if (!analysis.available) {
        analysis.unavailableReason = "QPC frequency is unavailable";
        return analysis;
    }

    std::optional<MechanicalInputEvent> leftDown;
    std::optional<SelectionAcquisition> latest;
    std::optional<SelectionAcquisition> previousDirectClick;
    for (const auto& event : result.mechanicalEvents) {
        if (event.type == MechanicalInputType::MouseLeftDown) {
            leftDown = event;
            continue;
        }
        if (event.type == MechanicalInputType::MouseLeftUp && leftDown) {
            if (event.timestampTicks < leftDown->timestampTicks) {
                leftDown.reset();
                previousDirectClick.reset();
                continue;
            }
            const int dx = std::abs(event.cursorX - leftDown->cursorX);
            const int dy = std::abs(event.cursorY - leftDown->cursorY);
            const bool box = std::max(dx, dy) >= config.dragThresholdPixels;
            const bool ctrl = (leftDown->modifiers & ModifierCtrl) != 0;
            const bool shift = (leftDown->modifiers & ModifierShift) != 0;
            SelectionAcquisition acquisition;
            acquisition.startQpc = leftDown->timestampTicks;
            acquisition.completeQpc = event.timestampTicks;
            acquisition.completeActiveMs = event.activeMs;
            acquisition.x = event.cursorX;
            acquisition.y = event.cursorY;
            if (box)
                acquisition.method = shift ? ArmySelectionMethod::ShiftBoxModify
                                           : ArmySelectionMethod::BoxSelect;
            else if (ctrl && shift)
                acquisition.method = ArmySelectionMethod::CtrlShiftClickType;
            else if (ctrl)
                acquisition.method = ArmySelectionMethod::CtrlClickType;
            else if (shift)
                acquisition.method = ArmySelectionMethod::ShiftClickModify;
            else
                acquisition.method = ArmySelectionMethod::DirectClick;

            if (acquisition.method == ArmySelectionMethod::DirectClick && previousDirectClick) {
                const auto gap = qpcMilliseconds(previousDirectClick->completeQpc,
                                                 leftDown->timestampTicks, qpcFrequency);
                if (gap && *gap <= config.doubleClickThresholdMs &&
                    closeCoordinates(*previousDirectClick, event,
                                     config.doubleClickDistancePixels)) {
                    acquisition.method = ArmySelectionMethod::DoubleClickType;
                    acquisition.startQpc = previousDirectClick->startQpc;
                    previousDirectClick.reset();
                } else {
                    previousDirectClick = acquisition;
                }
            } else if (acquisition.method == ArmySelectionMethod::DirectClick) {
                previousDirectClick = acquisition;
            } else {
                previousDirectClick.reset();
            }

            if (shiftSelectionMethod(acquisition.method) && latest &&
                shiftSelectionMethod(latest->method)) {
                const auto gap = qpcMilliseconds(latest->completeQpc, acquisition.startQpc,
                                                 qpcFrequency);
                if (gap && *gap <= config.attributionWindowMs)
                    acquisition.startQpc = latest->startQpc;
            }
            if (!box && !shiftSelectionMethod(acquisition.method) &&
                acquisition.method != ArmySelectionMethod::DoubleClickType)
                acquisition.startQpc = acquisition.completeQpc;
            latest = acquisition;
            leftDown.reset();
            continue;
        }

        const bool operation = event.type == MechanicalInputType::ControlGroupAssign ||
                               event.type == MechanicalInputType::ControlGroupAdd;
        if (operation && event.value >= 0 && event.value <= 9) {
            ArmyControlGroupEdit edit;
            edit.operationQpc = event.timestampTicks;
            edit.operationActiveMs = event.activeMs;
            edit.group = event.value;
            edit.operation = event.type == MechanicalInputType::ControlGroupAssign
                                 ? ArmyControlGroupOperation::Assign
                                 : ArmyControlGroupOperation::Add;
            if (latest) {
                const auto realGap = qpcMilliseconds(latest->completeQpc,
                                                     event.timestampTicks, qpcFrequency);
                const double activeGap = event.activeMs - latest->completeActiveMs;
                if (realGap && *realGap <= config.attributionWindowMs &&
                    activeGap >= 0.0 && activeGap <= config.attributionWindowMs) {
                    edit.selectionMethod = latest->method;
                    edit.selectionStartQpc = latest->startQpc;
                    edit.selectionCompleteQpc = latest->completeQpc;
                    edit.selectionDurationMs = qpcMilliseconds(latest->startQpc,
                                                               latest->completeQpc,
                                                               qpcFrequency);
                    edit.selectionToOperationMs = realGap;
                    edit.totalExecutionMs = qpcMilliseconds(latest->startQpc,
                                                            event.timestampTicks,
                                                            qpcFrequency);
                } else {
                    edit.selectionMethod = ArmySelectionMethod::ExistingSelection;
                }
            } else {
                edit.selectionMethod = ArmySelectionMethod::ExistingSelection;
            }
            analysis.edits.push_back(std::move(edit));
            latest.reset();
            previousDirectClick.reset();
            continue;
        }
        if (event.type == MechanicalInputType::ControlGroupSelect ||
            event.type == MechanicalInputType::LocationRecall ||
            event.type == MechanicalInputType::LocationAssign) {
            latest.reset();
            previousDirectClick.reset();
        }
    }
    rebuildArmyControlGroupStatistics(analysis);
    return analysis;
}

void rebuildArmyControlGroupStatistics(ArmyControlGroupAnalysis& analysis) {
    analysis.assignments = 0;
    analysis.additions = 0;
    analysis.uncertainEdits = 0;
    analysis.excludedProductionBuildingEdits = 0;
    analysis.excludedScoutingUnitEdits = 0;
    analysis.assignmentMethods = {};
    analysis.additionMethods = {};
    analysis.byGroup = {};
    std::array<std::vector<const ArmyControlGroupEdit*>, armySelectionMethodCount> assigns;
    std::array<std::vector<const ArmyControlGroupEdit*>, armySelectionMethodCount> adds;
    for (const auto& edit : analysis.edits) {
        if (edit.scope == ArmyControlGroupScope::Uncertain) {
            ++analysis.uncertainEdits;
            continue;
        }
        if (edit.scope == ArmyControlGroupScope::ProductionBuilding) {
            ++analysis.excludedProductionBuildingEdits;
            continue;
        }
        if (edit.scope == ArmyControlGroupScope::ScoutingUnit) {
            ++analysis.excludedScoutingUnitEdits;
            continue;
        }
        const auto method = armySelectionMethodIndex(edit.selectionMethod);
        if (edit.operation == ArmyControlGroupOperation::Assign) {
            ++analysis.assignments;
            assigns[method].push_back(&edit);
            if (edit.group >= 0 && edit.group <= 9)
                ++analysis.byGroup[static_cast<std::size_t>(edit.group)].assignments;
        } else {
            ++analysis.additions;
            adds[method].push_back(&edit);
            if (edit.group >= 0 && edit.group <= 9)
                ++analysis.byGroup[static_cast<std::size_t>(edit.group)].additions;
        }
    }
    for (std::size_t index = 0; index < armySelectionMethodCount; ++index) {
        analysis.assignmentMethods[index] = summarize(assigns[index]);
        analysis.additionMethods[index] = summarize(adds[index]);
    }
}

void applyScoutingUnitClassification(ArmyControlGroupAnalysis& analysis) {
    // ArmyControlGroupAnalysis contains edits only, so ordinary group selections
    // deliberately neither cancel nor terminate an assignment generation.
    for (std::size_t index = 0; index < analysis.edits.size(); ++index) {
        auto& assignment = analysis.edits[index];
        if (assignment.scope != ArmyControlGroupScope::Army ||
            assignment.operation != ArmyControlGroupOperation::Assign ||
            assignment.operationActiveMs >= scoutingUnitCutoffMs)
            continue;

        bool addedBeforeOverwrite = false;
        for (std::size_t laterIndex = index + 1; laterIndex < analysis.edits.size();
             ++laterIndex) {
            const auto& later = analysis.edits[laterIndex];
            if (later.group != assignment.group)
                continue;
            if (later.operation == ArmyControlGroupOperation::Assign)
                break;
            if (later.operation == ArmyControlGroupOperation::Add) {
                addedBeforeOverwrite = true;
                break;
            }
        }
        if (!addedBeforeOverwrite)
            assignment.scope = ArmyControlGroupScope::ScoutingUnit;
    }
    rebuildArmyControlGroupStatistics(analysis);
}

void analyzeScoutingUnitActivity(ArmyControlGroupAnalysis& analysis,
                                 const AnalysisResult& result,
                                 std::uint64_t qpcFrequency) {
    analysis.scoutingUnitActivities.clear();
    if (qpcFrequency == 0)
        return;

    std::array<std::uint32_t, 10> assignmentGenerations{};
    for (std::size_t editIndex = 0; editIndex < analysis.edits.size(); ++editIndex) {
        const auto& assignment = analysis.edits[editIndex];
        if (assignment.operation != ArmyControlGroupOperation::Assign ||
            assignment.group < 0 || assignment.group > 9)
            continue;

        const auto groupIndex = static_cast<std::size_t>(assignment.group);
        const auto generation = ++assignmentGenerations[groupIndex];
        if (assignment.scope != ArmyControlGroupScope::ScoutingUnit)
            continue;

        std::optional<std::uint64_t> generationEndQpc;
        for (std::size_t laterIndex = editIndex + 1; laterIndex < analysis.edits.size();
             ++laterIndex) {
            const auto& later = analysis.edits[laterIndex];
            if (later.group == assignment.group &&
                later.operation == ArmyControlGroupOperation::Assign) {
                generationEndQpc = later.operationQpc;
                break;
            }
        }

        ScoutingUnitActivity activity;
        activity.group = assignment.group;
        activity.assignmentGeneration = generation;
        activity.assignedQpc = assignment.operationQpc;
        activity.assignedActiveMs = assignment.operationActiveMs;
        bool scoutSelectionActive = false;
        for (const auto& event : result.mechanicalEvents) {
            if (event.timestampTicks <= assignment.operationQpc)
                continue;
            if (generationEndQpc && event.timestampTicks >= *generationEndQpc)
                break;

            if (event.type == MechanicalInputType::ControlGroupSelect) {
                scoutSelectionActive = event.value == assignment.group;
                if (!scoutSelectionActive)
                    continue;
                if (!activity.firstSelectionQpc) {
                    activity.firstSelectionQpc = event.timestampTicks;
                    activity.firstSelectionActiveMs = event.activeMs;
                }
                activity.lastSelectionQpc = event.timestampTicks;
                activity.lastSelectionActiveMs = event.activeMs;
                ++activity.selectionCount;
                continue;
            }

            // The compact mechanical stream does not distinguish unit/box clicks from
            // every other left click. Match the existing physical selection-acquisition
            // semantics and conservatively invalidate as soon as a left selection gesture
            // begins. Location recalls/assignments deliberately leave unit selection intact.
            if (event.type == MechanicalInputType::MouseLeftDown) {
                scoutSelectionActive = false;
                continue;
            }

            // MouseRightDown is the only reliably command-shaped physical action in the
            // current stream. MouseRightUp is the same command's release, not another command.
            if (scoutSelectionActive &&
                event.type == MechanicalInputType::MouseRightDown) {
                if (!activity.firstCommandQpc) {
                    activity.firstCommandQpc = event.timestampTicks;
                    activity.firstCommandActiveMs = event.activeMs;
                }
                activity.lastCommandQpc = event.timestampTicks;
                activity.lastCommandActiveMs = event.activeMs;
                ++activity.commandCount;
            }
        }

        if (activity.lastSelectionQpc)
            activity.assignmentToLastSelectionMs =
                qpcMilliseconds(activity.assignedQpc, *activity.lastSelectionQpc,
                                qpcFrequency);
        if (activity.lastCommandQpc) {
            activity.assignmentToLastCommandMs =
                qpcMilliseconds(activity.assignedQpc, *activity.lastCommandQpc,
                                qpcFrequency);
            activity.scoutingActivityDurationMs =
                activity.assignmentToLastCommandMs;
        }
        if (activity.firstCommandQpc && activity.lastCommandQpc)
            activity.firstToLastCommandMs =
                qpcMilliseconds(*activity.firstCommandQpc, *activity.lastCommandQpc,
                                qpcFrequency);
        analysis.scoutingUnitActivities.push_back(std::move(activity));
    }
}

} // namespace smp
