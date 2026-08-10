#pragma once

#include "analysis/analyzer.h"

#include <cstdint>
#include <optional>
#include <unordered_set>

namespace smp {

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

    [[nodiscard]] std::uint64_t navigationTransitions() const noexcept;
    [[nodiscard]] double navigationTransitionsPerMinute() const noexcept;
    [[nodiscard]] double methodPercentage(std::uint64_t count) const noexcept;
};

class AutomaticSessionState {
  public:
    bool addFinalizedGame(std::uint64_t generation, const AnalysisResult& result);

    [[nodiscard]] const AutomaticSessionStats& stats() const noexcept {
        return stats_;
    }
    [[nodiscard]] const std::optional<AnalysisResult>& lastGame() const noexcept {
        return lastGame_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return stats_.games == 0;
    }

  private:
    AutomaticSessionStats stats_;
    std::optional<AnalysisResult> lastGame_;
    std::unordered_set<std::uint64_t> accountedGenerations_;
};

AutomaticSessionStats automaticSessionStatsForGame(const AnalysisResult& result);

} // namespace smp
