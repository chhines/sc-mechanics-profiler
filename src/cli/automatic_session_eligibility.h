#pragma once

#include "analysis/replay_analysis.h"
#include "cli/automatic_session_stats.h"

namespace smp {

struct AutomaticSessionEligibility {
    bool included{};
    std::string reason;
};

// Player identity alone gates automatic pooling. Metric availability and duration
// are deliberately not inputs to this policy.
[[nodiscard]] AutomaticSessionEligibility automaticSessionEligibility(
    const AnalysisResult& recording, const ReplayExtractionResult& replay);

// Returns false for a generation already accounted as included, aborted or excluded.
bool accountAutomaticSessionGame(AutomaticSessionState& session, std::uint64_t generation,
                                const AnalysisResult& recording, const ProductionAnalysis& production,
                                const AutomaticSessionEligibility& eligibility);

[[nodiscard]] std::string automaticGameExclusionDiagnostic(
    std::uint64_t generation, const AutomaticSessionEligibility& eligibility);

} // namespace smp
