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

struct ScoutingCandidate {
    std::vector<std::size_t> editIndices;
    std::uint32_t unitTag{};
    double assignedActiveMs{};
};

struct LegacyScoutingCandidate {
    std::vector<std::size_t> editIndices;
    std::vector<std::uint32_t> unitTags;
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

bool sameUnitMembership(const std::vector<std::uint32_t>& first,
                        const std::vector<std::uint32_t>& second) {
    if (first.empty() || first.size() != second.size())
        return false;
    auto sortedFirst = first;
    auto sortedSecond = second;
    std::sort(sortedFirst.begin(), sortedFirst.end());
    std::sort(sortedSecond.begin(), sortedSecond.end());
    return sortedFirst == sortedSecond;
}

bool possibleEarlyWorkerCandidate(const ArmyControlGroupEdit& edit) {
    if (edit.scope != ArmyControlGroupScope::Army ||
        edit.operation != ArmyControlGroupOperation::Assign ||
        edit.operationActiveMs >= scoutingUnitCutoffMs ||
        edit.selectedUnitTags.size() != 1)
        return false;
    if (edit.selectedUnitTypes.empty())
        return true;
    return std::all_of(edit.selectedUnitTypes.begin(), edit.selectedUnitTypes.end(),
                       [](const std::string& type) {
                           return type == "Probe" || type == "SCV" ||
                                  type == "Drone";
                       });
}

double squaredDistance(double firstX, double firstY,
                       double secondX, double secondY) noexcept {
    const double dx = firstX - secondX;
    const double dy = firstY - secondY;
    return dx * dx + dy * dy;
}

bool scoutLikeCommand(const ScoutingUnitCommandEvidence& evidence) noexcept {
    return squaredDistance(evidence.targetX, evidence.targetY,
                           evidence.enemySpawnX, evidence.enemySpawnY) <
           squaredDistance(evidence.targetX, evidence.targetY,
                           evidence.ownSpawnX, evidence.ownSpawnY);
}

double homeRadius(const ScoutingUnitCommandEvidence& evidence) noexcept {
    const double spawnDistance = std::sqrt(squaredDistance(
        evidence.ownSpawnX, evidence.ownSpawnY,
        evidence.enemySpawnX, evidence.enemySpawnY));
    return std::clamp(spawnDistance * scoutingHomeRadiusSpawnFraction,
                      scoutingHomeRadiusMinPixels,
                      scoutingHomeRadiusMaxPixels);
}

bool homeLikeCommand(const ScoutingUnitCommandEvidence& evidence) noexcept {
    const double radius = homeRadius(evidence);
    return squaredDistance(evidence.targetX, evidence.targetY,
                           evidence.ownSpawnX, evidence.ownSpawnY) <=
           radius * radius;
}

std::vector<const ScoutingUnitCommandEvidence*> scoutingCommands(
    const std::vector<ScoutingUnitCommandEvidence>& evidence,
    std::uint32_t unitTag, double assignedActiveMs) {
    std::vector<const ScoutingUnitCommandEvidence*> commands;
    for (const auto& command : evidence) {
        if (command.unitTag == unitTag &&
            command.commandActiveMs >= assignedActiveMs)
            commands.push_back(&command);
    }
    std::sort(commands.begin(), commands.end(), [](const auto* first,
                                                   const auto* second) {
        if (first->commandActiveMs != second->commandActiveMs)
            return first->commandActiveMs < second->commandActiveMs;
        if (first->targetX != second->targetX)
            return first->targetX < second->targetX;
        return first->targetY < second->targetY;
    });
    commands.erase(
        std::unique(commands.begin(), commands.end(), [](const auto* first,
                                                        const auto* second) {
            return first->commandActiveMs == second->commandActiveMs &&
                   first->targetX == second->targetX &&
                   first->targetY == second->targetY;
        }),
        commands.end());
    return commands;
}

std::optional<std::size_t> scoutingEpisodeEndIndex(
    const std::vector<const ScoutingUnitCommandEvidence*>& commands) {
    std::optional<std::size_t> lastScoutLike;
    for (std::size_t index = 0; index < commands.size(); ++index) {
        if (scoutLikeCommand(*commands[index]))
            lastScoutLike = index;
    }
    if (!lastScoutLike)
        return std::nullopt;

    // A return home ends scouting only when there is no later enemy-side
    // excursion. Because lastScoutLike is the final enemy-side command, the
    // first home-region command after it is a confirmed return. Commands after
    // that are treated as post-scout usage (for example returning to mining).
    for (std::size_t index = *lastScoutLike + 1; index < commands.size(); ++index) {
        if (homeLikeCommand(*commands[index]))
            return index;
    }

    // If the worker never returns home, the last command we can attribute to the
    // unit is the defensible end of observed scouting. Brood War replays do not
    // provide an authoritative unit-death event.
    return commands.empty() ? std::nullopt
                            : std::optional<std::size_t>(commands.size() - 1);
}

std::optional<double> longestCommandGap(
    const std::vector<double>& commandActiveMs) noexcept {
    if (commandActiveMs.size() < 2)
        return std::nullopt;
    double longest = 0.0;
    for (std::size_t index = 1; index < commandActiveMs.size(); ++index)
        longest = std::max(longest,
                           commandActiveMs[index] - commandActiveMs[index - 1]);
    return longest;
}

const MechanicalInputEvent* nearestPhysicalRightClick(
    const AnalysisResult& result, double commandActiveMs) noexcept {
    const MechanicalInputEvent* best = nullptr;
    double bestDistance = scoutingPhysicalCommandMatchWindowMs + 1.0;
    for (const auto& event : result.mechanicalEvents) {
        if (event.type != MechanicalInputType::MouseRightDown)
            continue;
        const double distance = std::abs(event.activeMs - commandActiveMs);
        if (distance <= scoutingPhysicalCommandMatchWindowMs &&
            distance < bestDistance) {
            best = &event;
            bestDistance = distance;
        }
    }
    return best;
}

bool finishScoutingCandidate(
    ArmyControlGroupAnalysis& analysis,
    const ScoutingCandidate& candidate,
    const std::vector<ScoutingUnitCommandEvidence>& commandEvidence) {
    const auto commands = scoutingCommands(commandEvidence, candidate.unitTag,
                                           candidate.assignedActiveMs);
    const auto episodeEnd = scoutingEpisodeEndIndex(commands);
    if (!episodeEnd) {
        for (const auto editIndex : candidate.editIndices)
            analysis.edits[editIndex].scope = ArmyControlGroupScope::Uncertain;
        return false;
    }

    const double endActiveMs = commands[*episodeEnd]->commandActiveMs;
    for (auto& edit : analysis.edits) {
        if (edit.scope == ArmyControlGroupScope::ProductionBuilding ||
            edit.operationActiveMs < candidate.assignedActiveMs ||
            edit.operationActiveMs > endActiveMs ||
            edit.selectedUnitTags.size() != 1 ||
            edit.selectedUnitTags.front() != candidate.unitTag)
            continue;
        edit.scope = ArmyControlGroupScope::ScoutingUnit;
    }
    return true;
}

bool confirmsLegacyScoutTravel(
    const LegacyScoutingCandidate& candidate,
    const std::vector<ScoutingUnitTravelEvidence>& travelEvidence) {
    for (const auto& evidence : travelEvidence) {
        if (std::find(candidate.editIndices.begin(), candidate.editIndices.end(),
                      evidence.assignmentEditIndex) == candidate.editIndices.end())
            continue;
        const double axisX = evidence.mapCenterX - evidence.startX;
        const double axisY = evidence.mapCenterY - evidence.startY;
        const double axisLengthSquared = axisX * axisX + axisY * axisY;
        if (axisLengthSquared <= 0.0)
            continue;
        const double progress =
            ((evidence.targetX - evidence.startX) * axisX +
             (evidence.targetY - evidence.startY) * axisY) /
            axisLengthSquared;
        if (progress >= scoutingUnitTravelProgressThreshold)
            return true;
    }
    return false;
}

void finishLegacyScoutingCandidate(
    ArmyControlGroupAnalysis& analysis,
    std::optional<LegacyScoutingCandidate>& candidate,
    const std::vector<ScoutingUnitTravelEvidence>& travelEvidence,
    bool cancelled) {
    if (!candidate)
        return;
    const auto scope = !cancelled && confirmsLegacyScoutTravel(*candidate, travelEvidence)
                           ? ArmyControlGroupScope::ScoutingUnit
                           : ArmyControlGroupScope::Uncertain;
    for (const auto editIndex : candidate->editIndices)
        analysis.edits[editIndex].scope = scope;
    candidate.reset();
}

void analyzeLegacyScoutingUnitActivity(ArmyControlGroupAnalysis& analysis,
                                       const AnalysisResult& result,
                                       std::uint64_t qpcFrequency) {
    struct AssignmentGeneration {
        std::uint32_t number{};
        std::vector<std::uint32_t> unitTags;
        bool membershipValid{};
        bool activityCreated{};
    };
    std::array<AssignmentGeneration, 10> assignmentGenerations{};
    for (std::size_t editIndex = 0; editIndex < analysis.edits.size(); ++editIndex) {
        const auto& assignment = analysis.edits[editIndex];
        if (assignment.group < 0 || assignment.group > 9)
            continue;

        const auto groupIndex = static_cast<std::size_t>(assignment.group);
        auto& generationState = assignmentGenerations[groupIndex];
        if (assignment.operation == ArmyControlGroupOperation::Add) {
            generationState.membershipValid = false;
            generationState.activityCreated = false;
            continue;
        }
        const bool redundant =
            generationState.membershipValid &&
            sameUnitMembership(generationState.unitTags,
                               assignment.selectedUnitTags);
        if (!redundant) {
            ++generationState.number;
            generationState.unitTags = assignment.selectedUnitTags;
            generationState.membershipValid = !assignment.selectedUnitTags.empty();
            generationState.activityCreated = false;
        }
        if (assignment.scope != ArmyControlGroupScope::ScoutingUnit ||
            generationState.activityCreated)
            continue;
        generationState.activityCreated = true;

        std::optional<std::uint64_t> generationEndQpc;
        for (std::size_t laterIndex = editIndex + 1; laterIndex < analysis.edits.size();
             ++laterIndex) {
            const auto& later = analysis.edits[laterIndex];
            if (later.group != assignment.group)
                continue;
            if (later.operation == ArmyControlGroupOperation::Add ||
                (later.operation == ArmyControlGroupOperation::Assign &&
                 !sameUnitMembership(assignment.selectedUnitTags,
                                     later.selectedUnitTags))) {
                generationEndQpc = later.operationQpc;
                break;
            }
        }

        ScoutingUnitActivity activity;
        activity.group = assignment.group;
        activity.assignmentGeneration = generationState.number;
        if (assignment.selectedUnitTags.size() == 1)
            activity.unitTag = assignment.selectedUnitTags.front();
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
            if (event.type == MechanicalInputType::MouseLeftDown) {
                scoutSelectionActive = false;
                continue;
            }
            if (scoutSelectionActive &&
                event.type == MechanicalInputType::MouseRightDown) {
                if (!activity.firstCommandQpc) {
                    activity.firstCommandQpc = event.timestampTicks;
                    activity.firstCommandActiveMs = event.activeMs;
                }
                activity.lastCommandQpc = event.timestampTicks;
                activity.lastCommandActiveMs = event.activeMs;
                activity.commandActiveMs.push_back(event.activeMs);
                ++activity.commandCount;
            }
        }

        activity.longestCommandGapMs = longestCommandGap(activity.commandActiveMs);
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
                qpcMilliseconds(*activity.firstCommandQpc,
                                *activity.lastCommandQpc, qpcFrequency);
        analysis.scoutingUnitActivities.push_back(std::move(activity));
    }
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

void applyScoutingUnitClassification(
    ArmyControlGroupAnalysis& analysis,
    const std::vector<ScoutingUnitCommandEvidence>& commandEvidence) {
    analysis.scoutingUnitCommandEvidence = commandEvidence;
    analysis.scoutingUnitCommandEvidenceAvailable = true;
    analysis.scoutingUnitCandidateCount = 0;
    analysis.unconfirmedScoutingUnitCandidateCount = 0;

    // Candidates are keyed by replay unit identity, not by control-group binding.
    // Reassigning/overwriting a hotkey therefore does not end scouting by itself.
    std::vector<ScoutingCandidate> candidates;
    for (std::size_t index = 0; index < analysis.edits.size(); ++index) {
        const auto& assignment = analysis.edits[index];
        if (!possibleEarlyWorkerCandidate(assignment))
            continue;
        const auto unitTag = assignment.selectedUnitTags.front();
        const auto found = std::find_if(
            candidates.begin(), candidates.end(),
            [unitTag](const ScoutingCandidate& candidate) {
                return candidate.unitTag == unitTag;
            });
        if (found == candidates.end()) {
            candidates.push_back({{index}, unitTag, assignment.operationActiveMs});
        } else {
            found->editIndices.push_back(index);
            found->assignedActiveMs =
                std::min(found->assignedActiveMs, assignment.operationActiveMs);
        }
    }

    analysis.scoutingUnitCandidateCount = candidates.size();
    for (const auto& candidate : candidates) {
        if (!finishScoutingCandidate(analysis, candidate, commandEvidence))
            ++analysis.unconfirmedScoutingUnitCandidateCount;
    }
    rebuildArmyControlGroupStatistics(analysis);
}

void applyScoutingUnitClassification(
    ArmyControlGroupAnalysis& analysis,
    const std::vector<ScoutingUnitTravelEvidence>& travelEvidence) {
    analysis.scoutingUnitCommandEvidenceAvailable = false;
    analysis.scoutingUnitCandidateCount = 0;
    analysis.unconfirmedScoutingUnitCandidateCount = 0;
    std::array<std::optional<LegacyScoutingCandidate>, 10> candidates;
    for (std::size_t index = 0; index < analysis.edits.size(); ++index) {
        auto& assignment = analysis.edits[index];
        if (assignment.group < 0 || assignment.group > 9)
            continue;
        auto& candidate = candidates[static_cast<std::size_t>(assignment.group)];
        if (assignment.operation == ArmyControlGroupOperation::Add) {
            finishLegacyScoutingCandidate(analysis, candidate, travelEvidence, true);
            continue;
        }
        if (candidate && assignment.scope == ArmyControlGroupScope::Army &&
            sameUnitMembership(candidate->unitTags,
                               assignment.selectedUnitTags)) {
            candidate->editIndices.push_back(index);
            continue;
        }
        finishLegacyScoutingCandidate(analysis, candidate, travelEvidence, false);
        if (possibleEarlyWorkerCandidate(assignment))
            candidate = LegacyScoutingCandidate{{index}, assignment.selectedUnitTags};
    }
    for (auto& candidate : candidates)
        finishLegacyScoutingCandidate(analysis, candidate, travelEvidence, false);
    rebuildArmyControlGroupStatistics(analysis);
}

void analyzeScoutingUnitActivity(ArmyControlGroupAnalysis& analysis,
                                 const AnalysisResult& result,
                                 std::uint64_t qpcFrequency) {
    analysis.scoutingUnitActivities.clear();
    if (qpcFrequency == 0)
        return;

    // Synthetic/manual analyses created before the replay-command redesign do
    // not carry semantic command evidence. Keep their legacy activity behavior
    // isolated here; correlated production analyses use the unit-tag path below.
    if (!analysis.scoutingUnitCommandEvidenceAvailable) {
        analyzeLegacyScoutingUnitActivity(analysis, result, qpcFrequency);
        return;
    }

    struct ScoutIdentity {
        std::uint32_t unitTag{};
        std::size_t firstEditIndex{};
    };
    std::vector<ScoutIdentity> identities;
    for (std::size_t editIndex = 0; editIndex < analysis.edits.size(); ++editIndex) {
        const auto& edit = analysis.edits[editIndex];
        if (edit.scope != ArmyControlGroupScope::ScoutingUnit ||
            edit.selectedUnitTags.size() != 1)
            continue;
        const auto unitTag = edit.selectedUnitTags.front();
        if (std::none_of(identities.begin(), identities.end(),
                         [unitTag](const ScoutIdentity& identity) {
                             return identity.unitTag == unitTag;
                         }))
            identities.push_back({unitTag, editIndex});
    }
    std::sort(identities.begin(), identities.end(),
              [](const ScoutIdentity& first, const ScoutIdentity& second) {
                  return first.firstEditIndex < second.firstEditIndex;
              });

    std::array<std::uint32_t, 10> groupGenerations{};
    for (const auto& identity : identities) {
        const auto& assignment = analysis.edits[identity.firstEditIndex];
        const auto commands = scoutingCommands(analysis.scoutingUnitCommandEvidence,
                                               identity.unitTag,
                                               assignment.operationActiveMs);
        const auto episodeEnd = scoutingEpisodeEndIndex(commands);
        if (!episodeEnd || commands.empty())
            continue;

        ScoutingUnitActivity activity;
        activity.group = assignment.group;
        if (assignment.group >= 0 && assignment.group <= 9)
            activity.assignmentGeneration =
                ++groupGenerations[static_cast<std::size_t>(assignment.group)];
        activity.unitTag = identity.unitTag;
        activity.assignedQpc = assignment.operationQpc;
        activity.assignedActiveMs = assignment.operationActiveMs;
        activity.outcomeAvailable = true;
        activity.commandActiveMs.reserve(*episodeEnd + 1);
        for (std::size_t index = 0; index <= *episodeEnd; ++index)
            activity.commandActiveMs.push_back(commands[index]->commandActiveMs);
        activity.commandCount = activity.commandActiveMs.size();
        activity.firstCommandActiveMs = activity.commandActiveMs.front();
        activity.lastCommandActiveMs = activity.commandActiveMs.back();
        activity.longestCommandGapMs = longestCommandGap(activity.commandActiveMs);

        std::optional<std::size_t> lastScoutLike;
        for (std::size_t index = 0; index <= *episodeEnd; ++index) {
            if (scoutLikeCommand(*commands[index]))
                lastScoutLike = index;
        }
        if (lastScoutLike) {
            activity.returnedHome =
                *episodeEnd > *lastScoutLike &&
                homeLikeCommand(*commands[*episodeEnd]);
            bool seenScoutLike = false;
            for (std::size_t index = 0; index < *lastScoutLike; ++index) {
                if (scoutLikeCommand(*commands[index])) {
                    seenScoutLike = true;
                    continue;
                }
                if (seenScoutLike && homeLikeCommand(*commands[index])) {
                    activity.resumedAfterTemporaryReturn = true;
                    break;
                }
            }
        }

        const auto* firstPhysical =
            nearestPhysicalRightClick(result, *activity.firstCommandActiveMs);
        const auto* lastPhysical =
            nearestPhysicalRightClick(result, *activity.lastCommandActiveMs);
        if (firstPhysical)
            activity.firstCommandQpc = firstPhysical->timestampTicks;
        if (lastPhysical)
            activity.lastCommandQpc = lastPhysical->timestampTicks;

        if (activity.lastCommandQpc) {
            activity.assignmentToLastCommandMs =
                qpcMilliseconds(activity.assignedQpc, *activity.lastCommandQpc,
                                qpcFrequency);
        }
        if (!activity.assignmentToLastCommandMs)
            activity.assignmentToLastCommandMs =
                std::max(0.0, *activity.lastCommandActiveMs -
                                  activity.assignedActiveMs);
        activity.scoutingActivityDurationMs = activity.assignmentToLastCommandMs;

        if (activity.firstCommandQpc && activity.lastCommandQpc) {
            activity.firstToLastCommandMs =
                qpcMilliseconds(*activity.firstCommandQpc,
                                *activity.lastCommandQpc, qpcFrequency);
        }
        if (!activity.firstToLastCommandMs)
            activity.firstToLastCommandMs =
                std::max(0.0, *activity.lastCommandActiveMs -
                                  *activity.firstCommandActiveMs);

        // Control-group recall counts remain descriptive, but command attribution
        // no longer depends on them. Stop counting recalls once the original group
        // is overwritten/expanded or the scouting episode has ended.
        double selectionWindowEndActiveMs = *activity.lastCommandActiveMs;
        for (std::size_t laterIndex = identity.firstEditIndex + 1;
             laterIndex < analysis.edits.size(); ++laterIndex) {
            const auto& later = analysis.edits[laterIndex];
            if (later.group != assignment.group)
                continue;
            if (later.operation == ArmyControlGroupOperation::Add ||
                !sameUnitMembership(assignment.selectedUnitTags,
                                    later.selectedUnitTags)) {
                selectionWindowEndActiveMs =
                    std::min(selectionWindowEndActiveMs,
                             later.operationActiveMs);
                break;
            }
        }
        for (const auto& event : result.mechanicalEvents) {
            if (event.activeMs <= assignment.operationActiveMs)
                continue;
            if (event.activeMs > selectionWindowEndActiveMs)
                break;
            if (event.type != MechanicalInputType::ControlGroupSelect ||
                event.value != assignment.group)
                continue;
            if (!activity.firstSelectionQpc) {
                activity.firstSelectionQpc = event.timestampTicks;
                activity.firstSelectionActiveMs = event.activeMs;
            }
            activity.lastSelectionQpc = event.timestampTicks;
            activity.lastSelectionActiveMs = event.activeMs;
            ++activity.selectionCount;
        }
        if (activity.lastSelectionQpc)
            activity.assignmentToLastSelectionMs =
                qpcMilliseconds(activity.assignedQpc,
                                *activity.lastSelectionQpc, qpcFrequency);

        analysis.scoutingUnitActivities.push_back(std::move(activity));
    }
}

} // namespace smp
