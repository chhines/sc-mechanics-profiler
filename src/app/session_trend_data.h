#pragma once

#include "util/json.h"

#include <optional>

namespace smp {

struct SessionMacroGapTrendValues {
    std::optional<double> workerMedianMs;
    std::optional<double> workerP90Ms;
    std::optional<double> armyMedianMs;
    std::optional<double> armyP90Ms;
};

[[nodiscard]] inline SessionMacroGapTrendValues
decodeSessionMacroGapTrendValues(const json::Value& stats) {
    const auto numeric = [](const json::Value& value) {
        return value.isNumber() ? std::optional<double>(value.asNumber())
                                : std::nullopt;
    };
    return {
        numeric(stats["worker_macro"]["median_gap_ms"]),
        numeric(stats["worker_macro"]["p90_gap_ms"]),
        numeric(stats["army_macro"]["median_gap_ms"]),
        numeric(stats["army_macro"]["p90_gap_ms"]),
    };
}

} // namespace smp
