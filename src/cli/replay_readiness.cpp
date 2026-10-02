#include "cli/replay_readiness.h"

#include <algorithm>
#include <optional>
#include <string>

namespace smp {

ReplayExtractionResult waitForReplayReadiness(
    const ReplayMetadata& observedChange, const ReplayReadinessHooks& hooks,
    const ReplayReadinessPolicy& policy) {
    ReplayExtractionResult unavailable;
    unavailable.parser = bundledReplayParserDiagnostic;
    unavailable.unavailableReason = "Replay file did not become stable and readable in time";
    if (!hooks.now || !hooks.readMetadata || !hooks.readable || !hooks.parse || !hooks.wait ||
        policy.timeout <= std::chrono::milliseconds::zero() ||
        policy.pollInterval <= std::chrono::milliseconds::zero() ||
        policy.maximumParserAttempt <= std::chrono::milliseconds::zero() ||
        policy.maximumChecks == 0 || policy.requiredStableObservations == 0)
        return unavailable;

    const auto deadline = hooks.now() + policy.timeout;
    std::optional<ReplayMetadata> previous;
    std::size_t stableObservations = 0;
    std::optional<ReplayExtractionResult> lastParserFailure;
    for (std::size_t check = 0;
         check < policy.maximumChecks && hooks.now() < deadline; ++check) {
        const auto current = hooks.readMetadata();
        const bool metadataEligible =
            current.exists && current.size > 0 &&
            current.writeTimeUtc >= observedChange.writeTimeUtc;
        if (metadataEligible && hooks.readable()) {
            stableObservations = previous && *previous == current ? stableObservations + 1 : 1;
            previous = current;
            if (stableObservations >= policy.requiredStableObservations) {
                const auto now = hooks.now();
                if (now >= deadline)
                    break;
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now);
                if (remaining <= std::chrono::milliseconds::zero())
                    break;
                auto parsed = hooks.parse(std::min(policy.maximumParserAttempt, remaining));
                if (parsed.available)
                    return parsed;
                lastParserFailure = std::move(parsed);
            }
        } else {
            previous.reset();
            stableObservations = 0;
        }
        const auto now = hooks.now();
        if (now >= deadline)
            break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto waitDuration = std::min(policy.pollInterval, remaining);
        if (waitDuration <= std::chrono::milliseconds::zero())
            break;
        hooks.wait(waitDuration);
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
