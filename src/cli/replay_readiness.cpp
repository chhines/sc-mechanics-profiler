#include "cli/replay_readiness.h"

#include <optional>
#include <string>

namespace smp {

ReplayExtractionResult waitForReplayReadiness(
    const ReplayMetadata& observedChange, const ReplayReadinessHooks& hooks,
    std::size_t maximumChecks, std::size_t requiredStableObservations) {
    ReplayExtractionResult unavailable;
    unavailable.parser = bundledReplayParserDiagnostic;
    unavailable.unavailableReason = "Replay file did not become stable and readable in time";
    if (!hooks.readMetadata || !hooks.readable || !hooks.parse || maximumChecks == 0 ||
        requiredStableObservations == 0)
        return unavailable;

    std::optional<ReplayMetadata> previous;
    std::size_t stableObservations = 0;
    std::optional<ReplayExtractionResult> lastParserFailure;
    for (std::size_t check = 0; check < maximumChecks; ++check) {
        const auto current = hooks.readMetadata();
        const bool metadataEligible =
            current.exists && current.size > 0 &&
            current.writeTimeUtc >= observedChange.writeTimeUtc;
        if (metadataEligible && hooks.readable()) {
            stableObservations = previous && *previous == current ? stableObservations + 1 : 1;
            previous = current;
            if (stableObservations >= requiredStableObservations) {
                auto parsed = hooks.parse();
                if (parsed.available)
                    return parsed;
                lastParserFailure = std::move(parsed);
            }
        } else {
            previous.reset();
            stableObservations = 0;
        }
        if (check + 1 < maximumChecks && hooks.wait)
            hooks.wait();
    }
    if (lastParserFailure) {
        unavailable.parser = lastParserFailure->parser.empty()
                                 ? bundledReplayParserDiagnostic
                                 : lastParserFailure->parser;
        unavailable.unavailableReason =
            "Replay did not become parseable before the readiness deadline";
        if (!lastParserFailure->unavailableReason.empty())
            unavailable.unavailableReason += ": " + lastParserFailure->unavailableReason;
    }
    return unavailable;
}

} // namespace smp
