#pragma once

#include "analysis/analyzer.h"
#include "analysis/production_visit.h"

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_set>

namespace smp {

struct ProductMacroSessionStats {
    std::uint64_t gamesAnalyzed{};
    std::uint64_t gamesUnavailable{};
    std::uint64_t cycles{};
    std::uint64_t productionVisits{};
    double totalDurationMs{};
    std::optional<double> bestDurationMs;
    std::optional<double> slowestDurationMs;
    std::array<std::uint64_t, 4> accessMethodCounts{};

    [[nodiscard]] std::optional<double> averageDurationMs() const noexcept;
    [[nodiscard]] double accessMethodPercentage(ProductionAccessMethod method) const noexcept;
};

struct AutomaticSessionStats {
    std::uint64_t games{};
    double activeSeconds{};

    std::uint64_t controlGroupJumps{};
    std::uint64_t locationHotkeyJumps{};
    std::uint64_t minimapJumps{};
    std::uint64_t edgePans{};

    std::uint64_t edgeLeft{};
    std::uint64_t edgeRight{};
    std::uint64_t edgeTop{};
    std::uint64_t edgeBottom{};
    std::uint64_t edgeCorners{};

    ProductMacroSessionStats workerMacro;
    ProductMacroSessionStats armyMacro;

    [[nodiscard]] std::uint64_t navigationTransitions() const noexcept;
    [[nodiscard]] double navigationTransitionsPerMinute() const noexcept;
    [[nodiscard]] double methodPercentage(std::uint64_t count) const noexcept;
};

class AutomaticSessionState {
  public:
    bool addFinalizedGame(std::uint64_t generation, const AnalysisResult& result);
    bool addFinalizedGame(std::uint64_t generation, const AnalysisResult& result,
                          const ProductionAnalysis& production);
    bool markAbortedGeneration(std::uint64_t generation);

    [[nodiscard]] const AutomaticSessionStats& stats() const noexcept {
        return stats_;
    }
    [[nodiscard]] const std::optional<AnalysisResult>& lastGame() const noexcept {
        return lastGame_;
    }
    [[nodiscard]] const std::optional<ProductionAnalysis>& lastGameProduction() const noexcept {
        return lastGameProduction_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return stats_.games == 0;
    }

  private:
    AutomaticSessionStats stats_;
    std::optional<AnalysisResult> lastGame_;
    std::optional<ProductionAnalysis> lastGameProduction_;
    std::unordered_set<std::uint64_t> accountedGenerations_;
};

AutomaticSessionStats automaticSessionStatsForGame(const AnalysisResult& result);
AutomaticSessionStats automaticSessionStatsForGame(const AnalysisResult& result,
                                                    const ProductionAnalysis& production);

} // namespace smp
