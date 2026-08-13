#include "cli/automatic_session_stats.h"

#include <algorithm>

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
    return production;
}

void collectProductMacro(ProductMacroSessionStats& stats,
                         const ProductMacroCycleAnalysis& analysis) {
    if (!analysis.available) {
        stats.gamesUnavailable = 1;
        return;
    }
    stats.gamesAnalyzed = 1;
    stats.cycles = static_cast<std::uint64_t>(analysis.cycles.size());
    stats.productionVisits = static_cast<std::uint64_t>(analysis.productionVisitCount);
    for (const auto& cycle : analysis.cycles)
        stats.totalDurationMs += cycle.durationMs;
    for (const auto& cycle : analysis.cycles)
        stats.accessStyleDurationsMs[macroAccessStyleIndex(cycle.macroAccessStyle)]
            .push_back(cycle.durationMs);
    stats.bestDurationMs = analysis.bestDurationMs;
    stats.slowestDurationMs = analysis.slowestDurationMs;
    for (std::size_t index = 0; index < stats.accessMethodCounts.size(); ++index)
        stats.accessMethodCounts[index] = static_cast<std::uint64_t>(analysis.accessMethodCounts[index]);
}

void mergeProductMacro(ProductMacroSessionStats& target, const ProductMacroSessionStats& game) {
    target.gamesAnalyzed += game.gamesAnalyzed;
    target.gamesUnavailable += game.gamesUnavailable;
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
    collectProductMacro(stats.workerMacro, production.workerMacroCycles);
    collectProductMacro(stats.armyMacro, production.armyMacroCycles);
    stats.armyControlGroups = production.armyControlGroupManagement;
    stats.armyControlGroups.activeDurationSeconds = result.activeDurationSeconds;
    rebuildArmyControlGroupStatistics(stats.armyControlGroups);
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
    mergeArmyControlGroups(stats_.armyControlGroups, game.armyControlGroups);
    lastGame_ = result;
    lastGameProduction_ = production;
    return true;
}

bool AutomaticSessionState::markAbortedGeneration(std::uint64_t generation) {
    return generation != 0 && accountedGenerations_.insert(generation).second;
}

} // namespace smp
