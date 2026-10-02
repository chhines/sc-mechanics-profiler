#pragma once

#include "analysis/replay_analysis.h"
#include "platform/automatic_lifecycle.h"

#include <chrono>
#include <cstddef>
#include <functional>

namespace smp {

inline constexpr auto replayReadinessTimeout = std::chrono::milliseconds(3000);
inline constexpr auto replayReadinessPollInterval = std::chrono::milliseconds(100);
inline constexpr auto replayReadinessParserAttemptTimeout = std::chrono::milliseconds(1000);

struct ReplayReadinessHooks {
    using Clock = std::chrono::steady_clock;

    std::function<Clock::time_point()> now;
    std::function<ReplayMetadata()> readMetadata;
    std::function<bool()> readable;
    std::function<ReplayExtractionResult(std::chrono::milliseconds)> parse;
    std::function<void(std::chrono::milliseconds)> wait;
};

struct ReplayReadinessPolicy {
    std::chrono::milliseconds timeout{replayReadinessTimeout};
    std::chrono::milliseconds pollInterval{replayReadinessPollInterval};
    std::chrono::milliseconds maximumParserAttempt{replayReadinessParserAttemptTimeout};
    std::size_t requiredStableObservations{2};
    std::size_t maximumChecks{128};
};

[[nodiscard]] ReplayExtractionResult waitForReplayReadiness(
    const ReplayMetadata& observedChange, const ReplayReadinessHooks& hooks,
    const ReplayReadinessPolicy& policy = {});

} // namespace smp
