#include "cli/automatic_session_stats.h"

namespace smp {
namespace {

bool isCorner(EdgeDirection direction) noexcept {
    return direction == EdgeDirection::TopLeft || direction == EdgeDirection::TopRight ||
           direction == EdgeDirection::BottomLeft || direction == EdgeDirection::BottomRight;
}

} // namespace

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

std::optional<double> AutomaticSessionStats::macroAverageDurationMs() const noexcept {
    return macroCycles > 0 ? std::optional<double>(macroTotalDurationMs / static_cast<double>(macroCycles))
                           : std::nullopt;
}

AutomaticSessionStats automaticSessionStatsForGame(const AnalysisResult& result) {
    MacroCycleAnalysis unavailable;
    unavailable.available = false;
    unavailable.unavailableReason = "Macro-cycle analysis was not performed";
    return automaticSessionStatsForGame(result, unavailable);
}

AutomaticSessionStats automaticSessionStatsForGame(const AnalysisResult& result,
                                                    const MacroCycleAnalysis& macroCycles) {
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
    if (!macroCycles.available) {
        stats.macroGamesUnavailable = 1;
    } else {
        stats.macroGamesAnalyzed = 1;
        stats.macroCycles = static_cast<std::uint64_t>(macroCycles.cycles.size());
        for (const auto& cycle : macroCycles.cycles)
            stats.macroTotalDurationMs += cycle.durationMs;
        stats.macroBestDurationMs = macroCycles.bestDurationMs;
        stats.macroSlowestDurationMs = macroCycles.slowestDurationMs;
    }
    return stats;
}

bool AutomaticSessionState::addFinalizedGame(std::uint64_t generation, const AnalysisResult& result) {
    MacroCycleAnalysis unavailable;
    unavailable.available = false;
    unavailable.unavailableReason = "Macro-cycle analysis was not performed";
    return addFinalizedGame(generation, result, unavailable);
}

bool AutomaticSessionState::addFinalizedGame(std::uint64_t generation, const AnalysisResult& result,
                                             const MacroCycleAnalysis& macroCycles) {
    if (!accountedGenerations_.insert(generation).second)
        return false;

    const auto game = automaticSessionStatsForGame(result, macroCycles);
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
    stats_.macroGamesAnalyzed += game.macroGamesAnalyzed;
    stats_.macroGamesUnavailable += game.macroGamesUnavailable;
    stats_.macroCycles += game.macroCycles;
    stats_.macroTotalDurationMs += game.macroTotalDurationMs;
    if (game.macroBestDurationMs &&
        (!stats_.macroBestDurationMs || *game.macroBestDurationMs < *stats_.macroBestDurationMs))
        stats_.macroBestDurationMs = game.macroBestDurationMs;
    if (game.macroSlowestDurationMs &&
        (!stats_.macroSlowestDurationMs || *game.macroSlowestDurationMs > *stats_.macroSlowestDurationMs))
        stats_.macroSlowestDurationMs = game.macroSlowestDurationMs;
    lastGame_ = result;
    lastGameMacroCycles_ = macroCycles;
    return true;
}

} // namespace smp
