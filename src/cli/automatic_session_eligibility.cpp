#include "cli/automatic_session_eligibility.h"

#include "util/json.h"

namespace smp {

AutomaticSessionEligibility automaticSessionEligibility(
    const AnalysisResult& recording, const ReplayExtractionResult& replay) {
    if (!replay.available)
        return {false, "Replay unavailable: " + replay.unavailableReason};
    const auto match = identifyReplayPlayer(recording.mechanicalEvents, replay.replay);
    return {match.available, match.unavailableReason};
}

bool accountAutomaticSessionGame(AutomaticSessionState& session, std::uint64_t generation,
                                const AnalysisResult& recording, const ProductionAnalysis& production,
                                const AutomaticSessionEligibility& eligibility) {
    return eligibility.included ? session.addFinalizedGame(generation, recording, production)
                                : session.markExcludedGeneration(generation);
}

std::string automaticGameExclusionDiagnostic(
    std::uint64_t generation, const AutomaticSessionEligibility& eligibility) {
    auto detail = json::stringify(json::Value(eligibility.reason));
    if (!detail.empty() && detail.back() == '\n') detail.pop_back();
    return "AUTO_GAME_EXCLUDED generation=" + std::to_string(generation) +
        " reason=player_unidentified detail=" + detail;
}

} // namespace smp
