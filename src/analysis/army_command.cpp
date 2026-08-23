#include "analysis/army_command.h"

#include "analysis/macro_gap.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace smp {
namespace {

bool containsAny(const std::vector<std::uint32_t>& tags,
                 const std::unordered_set<std::uint32_t>& evidence) {
    return std::any_of(tags.begin(), tags.end(), [&](std::uint32_t tag) {
        return evidence.contains(tag);
    });
}

bool cleanArmySelection(const std::vector<std::uint32_t>& tags,
                        const ArmyCommandRoleEvidence& evidence) {
    if (tags.empty() || containsAny(tags, evidence.workerTags) ||
        containsAny(tags, evidence.productionBuildingTags) ||
        containsAny(tags, evidence.scoutingTags))
        return false;
    return std::all_of(tags.begin(), tags.end(), [&](std::uint32_t tag) {
        return evidence.armyTags.contains(tag);
    });
}

} // namespace

std::optional<double> ArmyCommandAnalysis::commandsPerMinute() const noexcept {
    if (!available || activeDurationSeconds <= 0.0)
        return std::nullopt;
    return static_cast<double>(commandCount) /
           (activeDurationSeconds / 60.0);
}

std::optional<double> ArmyCommandAnalysis::medianGapMs() const {
    if (!available || gapDurationsMs.empty())
        return std::nullopt;
    return interpolatedPercentile(gapDurationsMs, 0.50);
}

std::optional<double> ArmyCommandAnalysis::p90GapMs() const {
    if (!available || gapDurationsMs.empty())
        return std::nullopt;
    return interpolatedPercentile(gapDurationsMs, 0.90);
}

std::optional<double> ArmyCommandAnalysis::longestGapMs() const {
    if (!available || gapDurationsMs.empty())
        return std::nullopt;
    return *std::max_element(gapDurationsMs.begin(), gapDurationsMs.end());
}

ArmyCommandRoleEvidence buildArmyCommandRoleEvidence(
    std::unordered_set<std::uint32_t> workerTags,
    std::unordered_set<std::uint32_t> productionBuildingTags,
    const ArmyControlGroupAnalysis& controlGroups) {
    ArmyCommandRoleEvidence evidence;
    evidence.workerTags = std::move(workerTags);
    evidence.productionBuildingTags = std::move(productionBuildingTags);

    for (const auto& edit : controlGroups.edits) {
        if (!edit.replayConfirmed ||
            edit.bindingConfidence !=
                ArmyControlGroupBindingConfidence::ReplayConfirmed ||
            edit.scope != ArmyControlGroupScope::ScoutingUnit)
            continue;
        evidence.scoutingTags.insert(edit.selectedUnitTags.begin(),
                                     edit.selectedUnitTags.end());
    }

    for (const auto& edit : controlGroups.edits) {
        if (!edit.replayConfirmed ||
            edit.bindingConfidence !=
                ArmyControlGroupBindingConfidence::ReplayConfirmed ||
            edit.scope != ArmyControlGroupScope::Army ||
            edit.selectedUnitTags.empty() ||
            containsAny(edit.selectedUnitTags, evidence.workerTags) ||
            containsAny(edit.selectedUnitTags,
                        evidence.productionBuildingTags) ||
            containsAny(edit.selectedUnitTags, evidence.scoutingTags))
            continue;
        evidence.armyTags.insert(edit.selectedUnitTags.begin(),
                                 edit.selectedUnitTags.end());
    }
    return evidence;
}

ArmyCommandAnalysis analyzeArmyCommands(
    std::vector<ArmyCommandCandidate> candidates,
    const ArmyCommandRoleEvidence& evidence, double activeDurationSeconds) {
    ArmyCommandAnalysis analysis;
    analysis.available = true;
    analysis.activeDurationSeconds = std::max(0.0, activeDurationSeconds);

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& first, const auto& second) {
                         return std::tie(first.replayFrame,
                                         first.commandIndex) <
                                std::tie(second.replayFrame,
                                         second.commandIndex);
                     });
    const double activeDurationMs = analysis.activeDurationSeconds * 1000.0;
    for (const auto& candidate : candidates) {
        if (!std::isfinite(candidate.activeMs) || candidate.activeMs < 0.0 ||
            candidate.activeMs > activeDurationMs)
            continue;
        if (!cleanArmySelection(candidate.selectedUnitTags, evidence)) {
            ++analysis.unresolvedSelectionCommands;
            continue;
        }
        analysis.commands.push_back(
            {candidate.activeMs, candidate.kind, candidate.order});
    }
    analysis.commandCount = analysis.commands.size();
    if (analysis.commands.size() < 2)
        return analysis;

    analysis.gapDurationsMs.reserve(analysis.commands.size() - 1);
    for (std::size_t index = 1; index < analysis.commands.size(); ++index) {
        analysis.gapDurationsMs.push_back(std::max(
            0.0, analysis.commands[index].activeMs -
                     analysis.commands[index - 1].activeMs));
    }
    return analysis;
}

} // namespace smp
