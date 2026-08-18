#include "analysis/scouting_travel_gate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace smp {
namespace {

struct Candidate {
    std::vector<std::size_t> editIndices;
    std::uint32_t unitTag{};
    double assignedActiveMs{};
};

struct Episode {
    std::size_t confirmationIndex{};
    std::size_t endIndex{};
    bool returnedHome{};
    bool resumedAfterTemporaryReturn{};
};

std::optional<double> qpcMilliseconds(std::uint64_t start, std::uint64_t end,
                                      std::uint64_t frequency) noexcept {
    if (frequency == 0 || end < start)
        return std::nullopt;
    return static_cast<double>(static_cast<long double>(end - start) * 1000.0L /
                               static_cast<long double>(frequency));
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
                           return type == "Probe" || type == "SCV" || type == "Drone";
                       });
}

double squaredDistance(double firstX, double firstY,
                       double secondX, double secondY) noexcept {
    const double dx = firstX - secondX;
    const double dy = firstY - secondY;
    return dx * dx + dy * dy;
}

double spawnDistance(const ScoutingUnitCommandEvidence& evidence) noexcept {
    return std::sqrt(squaredDistance(evidence.ownSpawnX, evidence.ownSpawnY,
                                     evidence.enemySpawnX, evidence.enemySpawnY));
}

double homeRadius(const ScoutingUnitCommandEvidence& evidence) noexcept {
    return std::clamp(spawnDistance(evidence) * scoutingHomeRadiusSpawnFraction,
                      scoutingHomeRadiusMinPixels,
                      scoutingHomeRadiusMaxPixels);
}

bool homeLikeCommand(const ScoutingUnitCommandEvidence& evidence) noexcept {
    const double radius = homeRadius(evidence);
    return squaredDistance(evidence.targetX, evidence.targetY,
                           evidence.ownSpawnX, evidence.ownSpawnY) <=
           radius * radius;
}

bool scoutLikeCommand(const ScoutingUnitCommandEvidence& evidence) noexcept {
    return squaredDistance(evidence.targetX, evidence.targetY,
                           evidence.enemySpawnX, evidence.enemySpawnY) <
           squaredDistance(evidence.targetX, evidence.targetY,
                           evidence.ownSpawnX, evidence.ownSpawnY);
}

// We do not know the worker's continuous position. The most conservative useful
// bound is the shortest straight-line trip from the edge of the home region to
// the perpendicular midpoint between the two occupied starts. Using a speed
// ceiling faster than normal worker movement means this gate can confirm early,
// but never earlier than even that generous physical bound allows.
double minimumPlausibleTravelMs(const ScoutingUnitCommandEvidence& evidence) noexcept {
    const double distanceToEnemyHalf =
        std::max(0.0, spawnDistance(evidence) * 0.5 - homeRadius(evidence));
    return distanceToEnemyHalf / scoutingWorkerSpeedCeilingPixelsPerSecond * 1000.0;
}

std::vector<const ScoutingUnitCommandEvidence*> commandsFor(
    const std::vector<ScoutingUnitCommandEvidence>& evidence,
    std::uint32_t unitTag, double assignedActiveMs) {
    std::vector<const ScoutingUnitCommandEvidence*> commands;
    for (const auto& command : evidence) {
        if (command.unitTag == unitTag && command.commandActiveMs >= assignedActiveMs)
            commands.push_back(&command);
    }
    std::sort(commands.begin(), commands.end(), [](const auto* first, const auto* second) {
        if (first->commandActiveMs != second->commandActiveMs)
            return first->commandActiveMs < second->commandActiveMs;
        if (first->targetX != second->targetX)
            return first->targetX < second->targetX;
        return first->targetY < second->targetY;
    });
    commands.erase(
        std::unique(commands.begin(), commands.end(), [](const auto* first, const auto* second) {
            return first->commandActiveMs == second->commandActiveMs &&
                   first->targetX == second->targetX &&
                   first->targetY == second->targetY;
        }),
        commands.end());
    return commands;
}

std::optional<std::size_t> confirmationIndex(
    const std::vector<const ScoutingUnitCommandEvidence*>& commands,
    double assignedActiveMs) {
    if (commands.empty())
        return std::nullopt;

    double homeDepartureAnchorMs = assignedActiveMs;
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const auto& command = *commands[index];
        if (homeLikeCommand(command)) {
            // A command back into the home region before confirmation means any
            // earlier outbound click was an aborted departure. A later attempt
            // must earn a fresh travel-time budget from here.
            homeDepartureAnchorMs = command.commandActiveMs;
            continue;
        }
        if (!scoutLikeCommand(command))
            continue;
        if (command.commandActiveMs - homeDepartureAnchorMs >=
            minimumPlausibleTravelMs(command))
            return index;
    }
    return std::nullopt;
}

Episode episodeFor(const std::vector<const ScoutingUnitCommandEvidence*>& commands,
                   std::size_t confirmedIndex) {
    Episode episode;
    episode.confirmationIndex = confirmedIndex;
    episode.endIndex = commands.empty() ? 0 : commands.size() - 1;
    if (commands.empty() || confirmedIndex >= commands.size())
        return episode;

    std::optional<std::size_t> returnOrderIndex;
    for (std::size_t index = confirmedIndex + 1; index < commands.size(); ++index) {
        const auto& command = *commands[index];
        if (scoutLikeCommand(command)) {
            if (returnOrderIndex) {
                episode.resumedAfterTemporaryReturn = true;
                returnOrderIndex.reset();
            }
            continue;
        }
        if (!homeLikeCommand(command))
            continue;

        if (!returnOrderIndex) {
            // A home-target order means Returning, not already home.
            returnOrderIndex = index;
            continue;
        }

        const auto& returnOrder = *commands[*returnOrderIndex];
        if (command.commandActiveMs - returnOrder.commandActiveMs >=
            minimumPlausibleTravelMs(returnOrder)) {
            episode.returnedHome = true;
            episode.endIndex = index;
            return episode;
        }
    }
    return episode;
}

std::optional<double> longestCommandGap(const std::vector<double>& commandActiveMs) noexcept {
    if (commandActiveMs.size() < 2)
        return std::nullopt;
    double longest = 0.0;
    for (std::size_t index = 1; index < commandActiveMs.size(); ++index)
        longest = std::max(longest, commandActiveMs[index] - commandActiveMs[index - 1]);
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
        if (distance <= scoutingPhysicalCommandMatchWindowMs && distance < bestDistance) {
            best = &event;
            bestDistance = distance;
        }
    }
    return best;
}

std::optional<Episode> episodeForCandidate(
    const Candidate& candidate,
    const std::vector<ScoutingUnitCommandEvidence>& commandEvidence) {
    const auto commands = commandsFor(commandEvidence, candidate.unitTag,
                                     candidate.assignedActiveMs);
    const auto confirmed = confirmationIndex(commands, candidate.assignedActiveMs);
    if (!confirmed)
        return std::nullopt;
    return episodeFor(commands, *confirmed);
}

} // namespace

void applyTravelGatedScoutingUnitClassification(
    ArmyControlGroupAnalysis& analysis,
    const std::vector<ScoutingUnitCommandEvidence>& commandEvidence) {
    analysis.scoutingUnitCommandEvidence = commandEvidence;
    analysis.scoutingUnitCommandEvidenceAvailable = true;
    analysis.scoutingUnitCandidateCount = 0;
    analysis.unconfirmedScoutingUnitCandidateCount = 0;

    std::vector<Candidate> candidates;
    for (std::size_t index = 0; index < analysis.edits.size(); ++index) {
        const auto& assignment = analysis.edits[index];
        if (!possibleEarlyWorkerCandidate(assignment))
            continue;
        const auto unitTag = assignment.selectedUnitTags.front();
        const auto found = std::find_if(candidates.begin(), candidates.end(),
                                        [unitTag](const Candidate& candidate) {
                                            return candidate.unitTag == unitTag;
                                        });
        if (found == candidates.end()) {
            candidates.push_back({{index}, unitTag, assignment.operationActiveMs});
        } else {
            found->editIndices.push_back(index);
            found->assignedActiveMs = std::min(found->assignedActiveMs,
                                               assignment.operationActiveMs);
        }
    }

    analysis.scoutingUnitCandidateCount = candidates.size();
    for (const auto& candidate : candidates) {
        const auto commands = commandsFor(commandEvidence, candidate.unitTag,
                                          candidate.assignedActiveMs);
        const auto confirmed = confirmationIndex(commands, candidate.assignedActiveMs);
        if (!confirmed) {
            for (const auto editIndex : candidate.editIndices)
                analysis.edits[editIndex].scope = ArmyControlGroupScope::Uncertain;
            ++analysis.unconfirmedScoutingUnitCandidateCount;
            continue;
        }

        const auto episode = episodeFor(commands, *confirmed);
        const double endActiveMs = commands[episode.endIndex]->commandActiveMs;
        for (auto& edit : analysis.edits) {
            if (edit.scope == ArmyControlGroupScope::ProductionBuilding ||
                edit.operationActiveMs < candidate.assignedActiveMs ||
                edit.operationActiveMs > endActiveMs ||
                edit.selectedUnitTags.size() != 1 ||
                edit.selectedUnitTags.front() != candidate.unitTag)
                continue;
            edit.scope = ArmyControlGroupScope::ScoutingUnit;
        }
    }
    rebuildArmyControlGroupStatistics(analysis);
}

void analyzeTravelGatedScoutingUnitActivity(ArmyControlGroupAnalysis& analysis,
                                            const AnalysisResult& result,
                                            std::uint64_t qpcFrequency) {
    analysis.scoutingUnitActivities.clear();
    if (qpcFrequency == 0)
        return;

    if (!analysis.scoutingUnitCommandEvidenceAvailable) {
        analyzeScoutingUnitActivity(analysis, result, qpcFrequency);
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
        const auto commands = commandsFor(analysis.scoutingUnitCommandEvidence,
                                          identity.unitTag,
                                          assignment.operationActiveMs);
        const auto confirmed = confirmationIndex(commands, assignment.operationActiveMs);
        if (!confirmed)
            continue;
        const auto episode = episodeFor(commands, *confirmed);

        ScoutingUnitActivity activity;
        activity.group = assignment.group;
        if (assignment.group >= 0 && assignment.group <= 9)
            activity.assignmentGeneration =
                ++groupGenerations[static_cast<std::size_t>(assignment.group)];
        activity.unitTag = identity.unitTag;
        activity.assignedQpc = assignment.operationQpc;
        activity.assignedActiveMs = assignment.operationActiveMs;
        activity.outcomeAvailable = true;
        activity.returnedHome = episode.returnedHome;
        activity.resumedAfterTemporaryReturn = episode.resumedAfterTemporaryReturn;

        activity.commandActiveMs.reserve(episode.endIndex - episode.confirmationIndex + 1);
        for (std::size_t index = episode.confirmationIndex;
             index <= episode.endIndex; ++index)
            activity.commandActiveMs.push_back(commands[index]->commandActiveMs);
        activity.commandCount = activity.commandActiveMs.size();
        if (activity.commandActiveMs.empty())
            continue;
        activity.firstCommandActiveMs = activity.commandActiveMs.front();
        activity.lastCommandActiveMs = activity.commandActiveMs.back();
        activity.longestCommandGapMs = longestCommandGap(activity.commandActiveMs);

        const auto* firstPhysical =
            nearestPhysicalRightClick(result, *activity.firstCommandActiveMs);
        const auto* lastPhysical =
            nearestPhysicalRightClick(result, *activity.lastCommandActiveMs);
        if (firstPhysical)
            activity.firstCommandQpc = firstPhysical->timestampTicks;
        if (lastPhysical)
            activity.lastCommandQpc = lastPhysical->timestampTicks;

        if (activity.lastCommandQpc)
            activity.assignmentToLastCommandMs =
                qpcMilliseconds(activity.assignedQpc, *activity.lastCommandQpc,
                                qpcFrequency);
        if (!activity.assignmentToLastCommandMs)
            activity.assignmentToLastCommandMs =
                std::max(0.0, *activity.lastCommandActiveMs - activity.assignedActiveMs);

        if (activity.firstCommandQpc && activity.lastCommandQpc)
            activity.firstToLastCommandMs =
                qpcMilliseconds(*activity.firstCommandQpc,
                                *activity.lastCommandQpc, qpcFrequency);
        if (!activity.firstToLastCommandMs)
            activity.firstToLastCommandMs =
                std::max(0.0, *activity.lastCommandActiveMs -
                                  *activity.firstCommandActiveMs);
        // Duration now begins at confirmed arrival evidence, not the original
        // hotkey assignment or outbound travel order.
        activity.scoutingActivityDurationMs = activity.firstToLastCommandMs;

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
                    std::min(selectionWindowEndActiveMs, later.operationActiveMs);
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
