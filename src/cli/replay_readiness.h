#pragma once

#include "analysis/replay_analysis.h"
#include "platform/automatic_lifecycle.h"

#include <cstddef>
#include <functional>

namespace smp {

struct ReplayReadinessHooks {
    std::function<ReplayMetadata()> readMetadata;
    std::function<bool()> readable;
    std::function<ReplayExtractionResult()> parse;
    std::function<void()> wait;
};

[[nodiscard]] ReplayExtractionResult waitForReplayReadiness(
    const ReplayMetadata& observedChange, const ReplayReadinessHooks& hooks,
    std::size_t maximumChecks = 20, std::size_t requiredStableObservations = 2);

} // namespace smp
