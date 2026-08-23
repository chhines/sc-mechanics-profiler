#include "cli/automatic_session_stats.h"

#include "analysis/macro_gap.h"

#include <algorithm>
#include <cmath>

namespace smp {
namespace {

bool isCorner(EdgeDirection direction) noexcept {
    return direction == EdgeDirection::TopLeft || direction == EdgeDirection::TopRight ||
           direction == EdgeDirection::BottomLeft || direction == EdgeDirection::BottomRight;
}

ProductionAnalysis unavailableProduction() {
    ProductionAnalysis production;
    production.workerMacroCycles.productType = MacroProductType::Worker;
    production.workerMacroCycles.unavailableReason = "Replay correlation was not performed";
    production.armyMacroCycles.productType = MacroProductType::Army;
    production.armyMacroCycles.unavailableReason = "Replay correlation was not performed";
    production.armyControlGroupManagement.unavailableReason =
        "Replay correlation was not performed";
    production.armyCommandActivity.unavailableReason =
        "Replay correlation was not performed";
    production.abilityActivity.unavailableReason =
        "Replay correlation was not performed";
    return production;
}

void collectProductMacro(ProductMacroSessionStats& stats,
                         const ProductMacroCycleAnalysis& analysis,
                         double activeDurationSeconds) {
    if (!analysis.available) {
        stats.gamesUnavailable = 1;
        return;
    }
    stats.gamesAnalyzed = 1;
    stats.analyzedActiveSeconds = std::max(0.0, activeDurationSeconds);
    stats.cycles = static_cast<std::uint64_t>(analysis.cycles.size());
    stats.productionVisits = static_cast<std::uint64_t>(analysis.productionVisitCount);
    for (const auto& cycle : analysis.cycles)
        stats.totalDurationMs += cycle.durationMs;
    for (const auto& cycle : analysis.cycles)
        stats.accessStyleDurationsMs[macroAccessStyleIndex(cycle.macroAccessStyle)]
            .push_back(cycle.durationMs);
    const auto gaps = macroGapObservations(analysis.cycles);
    stats.gapDurationsMs.reserve(gaps.size());
    for (const auto& gap : gaps)
        stats.gapDurationsMs.push_back(gap.durationMs);
    stats.bestDurationMs = analysis.bestDurationMs;
    stats.slowestDurationMs = analysis.slowestDurationMs;
    for (std::size_t index = 0; index < stats.accessMethodCounts.size(); ++index)
        stats.accessMethodCounts[index] = static_cast<std::uint64_t>(analysis.accessMethodCounts[index]);
}

void mergeProductMacro(ProductMacroSessionStats& target, const ProductMacroSessionStats& game) {
    target.gamesAnalyzed += game.gamesAnalyzed;
    target.gamesUnavailable += game.gamesUnavailable;
    target.analyzedActiveSeconds += game.analyzedActiveSeconds;
    target.cycles += game.cycles;
    target.productionVisits += game.productionVisits;
    target.totalDurationMs += game.totalDurationMs;
    if (game.bestDurationMs && (!target.bestDurationMs || *game.bestDurationMs < *target.bestDurationMs))
        target.bestDurationMs = game.bestDurationMs;
    if (game.slowestDurationMs &&
        (!target.slowestDurationMs || *game.slowestDurationMs > *target.slowestDurationMs))
        target.slowestDurationMs = game.slowestDurationMs;
    for (std::size_t index = 0; index < target.accessMethodCounts.size(); ++index)
        target.accessMethodCounts[index] += game.accessMethodCounts[index];
    for (std::size_t index = 0; index < target.accessStyleDurationsMs.size(); ++index) {
        target.accessStyleDurationsMs[index].insert(
            target.accessStyleDurationsMs[index].end(),
            game.accessStyleDurationsMs[index].begin(),
            game.accessStyleDurationsMs[index].end());
    }
    target.gapDurationsMs.insert(target.gapDurationsMs.end(),
                                 game.gapDurationsMs.begin(),
                                 game.gapDurationsMs.end());
}

void collectArmyCommands(ArmyCommandSessionStats& stats,
                         const ArmyCommandAnalysis& analysis,
                         double activeDurationSeconds) {
    if (!analysis.available) {
        stats.gamesUnavailable = 1;
        return;
    }
    stats.gamesAnalyzed = 1;
    stats.analyzedActiveSeconds = std::max(0.0, activeDurationSeconds);
    stats.commandCount = static_cast<std::uint64_t>(analysis.commandCount);
    stats.gapDurationsMs = analysis.gapDurationsMs;
}

void mergeArmyCommands(ArmyCommandSessionStats& target,
                       const ArmyCommandSessionStats& game) {
    target.gamesAnalyzed += game.gamesAnalyzed;
    target.gamesUnavailable += game.gamesUnavailable;
    target.analyzedActiveSeconds += game.analyzedActiveSeconds;
    target.commandCount += game.commandCount;
    target.gapDurationsMs.insert(target.gapDurationsMs.end(),
                                 game.gapDurationsMs.begin(),
                                 game.gapDurationsMs.end());
}

void collectAbilityActivity(AbilityActivitySessionStats& stats,
                            const AbilityActivityAnalysis& analysis,
                            double activeDurationSeconds) {
    if (!analysis.available) {
        stats.gamesUnavailable = 1;
        return;
    }
    stats.gamesAnalyzed = 1;
    stats.analyzedActiveSeconds = std::max(0.0, activeDurationSeconds);
    stats.totalUses = static_cast<std::uint64_t>(analysis.totalUses());
}

void mergeAbilityActivity(AbilityActivitySessionStats& target,
                          const AbilityActivitySessionStats& game) {
    target.gamesAnalyzed += game.gamesAnalyzed;
    target.gamesUnavailable += game.gamesUnavailable;
    target.analyzedActiveSeconds += game.analyzedActiveSeconds;
    target.totalUses += game.totalUses;
}

MultitaskingActivityTimestamps multitaskingActivity(
    const AnalysisResult& result, const ProductionAnalysis& production) {
    MultitaskingActivityTimestamps activity;
    for (const auto& event : result.navigationEvents) {
        activity.activeMs[static_cast<std::size_t>(
            MultitaskingMechanicClass::Camera)].push_back(event.activeMs);
    }
    if (production.workerMacroCycles.available) {
        for (const auto& cycle : production.workerMacroCycles.cycles) {
            activity.activeMs[static_cast<std::size_t>(
                MultitaskingMechanicClass::WorkerMacro)]
                .push_back(cycle.startActiveMs);
        }
    }
    if (production.armyMacroCycles.available) {
        for (const auto& cycle : production.armyMacroCycles.cycles) {
            activity.activeMs[static_cast<std::size_t>(
                MultitaskingMechanicClass::ArmyMacro)]
                .push_back(cycle.startActiveMs);
        }
    }
    if (production.armyControlGroupManagement.available) {
        for (const auto& edit : production.armyControlGroupManagement.edits) {
            if (edit.scope != ArmyControlGroupScope::Army)
                continue;
            activity.activeMs[static_cast<std::size_t>(
                MultitaskingMechanicClass::ControlGroupEdit)]
                .push_back(edit.operationActiveMs);
        }
        for (const auto& scout :
             production.armyControlGroupManagement.scoutingUnitActivities) {
            auto& commands = activity.activeMs[static_cast<std::size_t>(
                MultitaskingMechanicClass::ScoutCommand)];
            commands.insert(commands.end(), scout.commandActiveMs.begin(),
                            scout.commandActiveMs.end());
        }
    }
    return activity;
}

bool multitaskingInputsAvailable(
    const AnalysisResult& result,
    const ProductionAnalysis& production) noexcept {
    return std::isfinite(result.activeDurationSeconds) &&
           result.activeDurationSeconds > 0.0 &&
           production.workerMacroCycles.available &&
           production.armyMacroCycles.available &&
           production.armyControlGroupManagement.available;
}

void collectMultitasking(MultitaskingSessionStats& stats,
                         const AnalysisResult& result,
                         const ProductionAnalysis& production) {
    if (!multitaskingInputsAvailable(result, production)) {
        stats.gamesUnavailable = 1;
        return;
    }
    stats.gamesAnalyzed = 1;
    const auto windows = summarizeMultitaskingWindows(
        result.activeDurationSeconds * 1000.0,
        multitaskingActivity(result, production));
    stats.totalDiversityAcrossActiveWindows =
        windows.totalDiversityAcrossActiveWindows;
    stats.activeWindowCount = windows.activeWindowCount;
    stats.peakDiversity = windows.peakDiversity;
}

void mergeMultitasking(MultitaskingSessionStats& target,
                       const MultitaskingSessionStats& game) {
    target.gamesAnalyzed += game.gamesAnalyzed;
    target.gamesUnavailable += game.gamesUnavailable;
    target.totalDiversityAcrossActiveWindows +=
        game.totalDiversityAcrossActiveWindows;
    target.activeWindowCount += game.activeWindowCount;
    target.peakDiversity = std::max(target.peakDiversity, game.peakDiversity);
}

void mergeArmyControlGroups(ArmyControlGroupAnalysis& target,
                            const ArmyControlGroupAnalysis& game) {
    if (!game.available) {
        if (target.unavailableReason.empty())
            target.unavailableReason = game.unavailableReason;
        return;
    }
    target.available = true;
    target.unavailableReason.clear();
    target.activeDurationSeconds += game.activeDurationSeconds;
    target.edits.insert(target.edits.end(), game.edits.begin(), game.edits.end());
    target.scoutingUnitActivities.insert(target.scoutingUnitActivities.end(),
                                         game.scoutingUnitActivities.begin(),
                                         game.scoutingUnitActivities.end());
    rebuildArmyControlGroupStatistics(target);
}

} // namespace

std::optional<double> ProductMacroSessionStats::averageDurationMs() const noexcept {
    return cycles > 0 ? std::optional<double>(totalDurationMs / static_cast<double>(cycles))
                      : std::nullopt;
}

std::optional<double> ProductMacroSessionStats::cyclesPerMinute() const noexcept {
    if (gamesAnalyzed == 0 || analyzedActiveSeconds <= 0.0)
        return std::nullopt;
    return static_cast<double>(cycles) / (analyzedActiveSeconds / 60.0);
}

std::optional<double> ProductMacroSessionStats::medianGapMs() const {
    return medianMacroGapMs(gapDurationsMs);
}

std::optional<double> ProductMacroSessionStats::p90GapMs() const {
    return p90MacroGapMs(gapDurationsMs);
}

std::optional<double> ProductMacroSessionStats::longestGapMs() const {
    if (gapDurationsMs.empty())
        return std::nullopt;
    return *std::max_element(gapDurationsMs.begin(), gapDurationsMs.end());
}

std::optional<double> ProductMacroSessionStats::gapsOverPerGame(
    double thresholdMs) const noexcept {
    if (gamesAnalyzed == 0)
        return std::nullopt;
    const auto count = std::count_if(
        gapDurationsMs.begin(), gapDurationsMs.end(),
        [thresholdMs](double durationMs) { return durationMs > thresholdMs; });
    return static_cast<double>(count) / static_cast<double>(gamesAnalyzed);
}

std::optional<double> ArmyCommandSessionStats::commandsPerMinute() const noexcept {
    if (gamesAnalyzed == 0 || analyzedActiveSeconds <= 0.0)
        return std::nullopt;
    return static_cast<double>(commandCount) /
           (analyzedActiveSeconds / 60.0);
}

std::optional<double> ArmyCommandSessionStats::medianGapMs() const {
    return gapDurationsMs.empty()
               ? std::nullopt
               : std::optional<double>(
                     interpolatedPercentile(gapDurationsMs, 0.50));
}

std::optional<double> ArmyCommandSessionStats::p90GapMs() const {
    return gapDurationsMs.empty()
               ? std::nullopt
               : std::optional<double>(
                     interpolatedPercentile(gapDurationsMs, 0.90));
}

std::optional<double> ArmyCommandSessionStats::longestGapMs() const {
    if (gapDurationsMs.empty())
        return std::nullopt;
    return *std::max_element(gapDurationsMs.begin(), gapDurationsMs.end());
}

std::optional<double>
AbilityActivitySessionStats::abilitiesPerMinute() const noexcept {
    if (gamesAnalyzed == 0 || analyzedActiveSeconds <= 0.0)
        return std::nullopt;
    return static_cast<double>(totalUses) /
           (analyzedActiveSeconds / 60.0);
}

std::optional<double>
MultitaskingSessionStats::averageActiveDiversity() const noexcept {
    if (gamesAnalyzed == 0 || activeWindowCount == 0)
        return std::nullopt;
    return static_cast<double>(totalDiversityAcrossActiveWindows) /
           static_cast<double>(activeWindowCount);
}

std::optional<double> MultitaskingSessionStats::peak() const noexcept {
    return gamesAnalyzed > 0
               ? std::optional<double>(static_cast<double>(peakDiversity))
               : std::nullopt;
}

double ProductMacroSessionStats::accessMethodPercentage(ProductionAccessMethod method) const noexcept {
    const auto index = static_cast<std::size_t>(method);
    return productionVisits > 0 && index < accessMethodCounts.size()
               ? static_cast<double>(accessMethodCounts[index]) * 100.0 /
                     static_cast<double>(productionVisits)
               : 0.0;
}

MacroAccessStyleStatistics
ProductMacroSessionStats::accessStyleStatistics(MacroAccessStyle style) const {
    const auto index = macroAccessStyleIndex(style);
    return index < accessStyleDurationsMs.size()
               ? summarizeMacroAccessStyleDurations(accessStyleDurationsMs[index])
               : MacroAccessStyleStatistics{};
}

double ProductMacroSessionStats::accessStylePercentage(MacroAccessStyle style) const noexcept {
    const auto index = macroAccessStyleIndex(style);
    return cycles > 0 && index < accessStyleDurationsMs.size()
               ? static_cast<double>(accessStyleDurationsMs[index].size()) * 100.0 /
                     static_cast<double>(cycles)
               : 0.0;
}

std::uint64_t AutomaticSessionStats::navigationTransitions() const noexcept {
    return controlGroupJumps + locationHotkeyJumps + minimapJumps + edgePans;
}

double AutomaticSessionStats::navigationTransitionsPerMinute() const noexcept {
    return activeSeconds > 0.0 ? static_cast<double>(navigationTransitions()) / (activeSeconds / 60.0) : 0.0;
}

double AutomaticSessionStats::methodPercentage(std::uint64_t count) const noexcept {
    const auto total = navigationTransitions();
    return total > 0 ? static_cast<double>(count) * 100.0 / static_cast<double>(total) : 0.0;
}

AutomaticSessionStats automaticSessionStatsForGame(const AnalysisResult& result) {
    return automaticSessionStatsForGame(result, unavailableProduction());
}

AutomaticSessionStats automaticSessionStatsForGame(const AnalysisResult& result,
                                                    const ProductionAnalysis& production) {
    AutomaticSessionStats stats;
    stats.games = 1;
    stats.activeSeconds = result.activeDurationSeconds;
    for (const auto& event : result.navigationEvents) {
        switch (event.type) {
        case CameraNavigationType::ControlGroupJump:
            ++stats.controlGroupJumps;
            break;
        case CameraNavigationType::LocationHotkey:
            ++stats.locationHotkeyJumps;
            break;
        case CameraNavigationType::MinimapJump:
            ++stats.minimapJumps;
            break;
        case CameraNavigationType::EdgeScroll:
            ++stats.edgePans;
            if (event.edgeDirection == EdgeDirection::Left)
                ++stats.edgeLeft;
            else if (event.edgeDirection == EdgeDirection::Right)
                ++stats.edgeRight;
            else if (event.edgeDirection == EdgeDirection::Top)
                ++stats.edgeTop;
            else if (event.edgeDirection == EdgeDirection::Bottom)
                ++stats.edgeBottom;
            else if (isCorner(event.edgeDirection))
                ++stats.edgeCorners;
            break;
        }
    }
    collectProductMacro(stats.workerMacro, production.workerMacroCycles,
                        result.activeDurationSeconds);
    collectProductMacro(stats.armyMacro, production.armyMacroCycles,
                        result.activeDurationSeconds);
    stats.armyControlGroups = production.armyControlGroupManagement;
    if (stats.armyControlGroups.available) {
        stats.armyControlGroupGamesAnalyzed = 1;
        stats.armyControlGroups.activeDurationSeconds =
            std::max(0.0, result.activeDurationSeconds);
        rebuildArmyControlGroupStatistics(stats.armyControlGroups);
    } else {
        stats.armyControlGroupGamesUnavailable = 1;
        stats.armyControlGroups.activeDurationSeconds = 0.0;
    }
    collectArmyCommands(stats.armyCommands, production.armyCommandActivity,
                        result.activeDurationSeconds);
    collectAbilityActivity(stats.abilityActivity, production.abilityActivity,
                           result.activeDurationSeconds);
    collectMultitasking(stats.multitasking, result, production);
    return stats;
}

bool AutomaticSessionState::addFinalizedGame(std::uint64_t generation, const AnalysisResult& result) {
    return addFinalizedGame(generation, result, unavailableProduction());
}

bool AutomaticSessionState::addFinalizedGame(std::uint64_t generation, const AnalysisResult& result,
                                             const ProductionAnalysis& production) {
    if (!accountedGenerations_.insert(generation).second)
        return false;

    const auto game = automaticSessionStatsForGame(result, production);
    ++stats_.games;
    stats_.activeSeconds += game.activeSeconds;
    stats_.controlGroupJumps += game.controlGroupJumps;
    stats_.locationHotkeyJumps += game.locationHotkeyJumps;
    stats_.minimapJumps += game.minimapJumps;
    stats_.edgePans += game.edgePans;
    stats_.edgeLeft += game.edgeLeft;
    stats_.edgeRight += game.edgeRight;
    stats_.edgeTop += game.edgeTop;
    stats_.edgeBottom += game.edgeBottom;
    stats_.edgeCorners += game.edgeCorners;
    mergeProductMacro(stats_.workerMacro, game.workerMacro);
    mergeProductMacro(stats_.armyMacro, game.armyMacro);
    stats_.armyControlGroupGamesAnalyzed +=
        game.armyControlGroupGamesAnalyzed;
    stats_.armyControlGroupGamesUnavailable +=
        game.armyControlGroupGamesUnavailable;
    mergeArmyControlGroups(stats_.armyControlGroups, game.armyControlGroups);
    mergeArmyCommands(stats_.armyCommands, game.armyCommands);
    mergeAbilityActivity(stats_.abilityActivity, game.abilityActivity);
    mergeMultitasking(stats_.multitasking, game.multitasking);
    lastGame_ = result;
    lastGameProduction_ = production;
    return true;
}

bool AutomaticSessionState::markAbortedGeneration(std::uint64_t generation) {
    return generation != 0 && accountedGenerations_.insert(generation).second;
}

} // namespace smp
