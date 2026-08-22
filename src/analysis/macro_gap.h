#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace smp {

struct MacroGapObservation {
    double previousCycleEndMs{};
    double nextCycleStartMs{};
    double durationMs{};
};

template <typename MacroCycle>
[[nodiscard]] std::vector<MacroGapObservation>
macroGapObservations(const std::vector<MacroCycle>& cycles) {
    std::vector<MacroGapObservation> gaps;
    if (cycles.size() < 2)
        return gaps;

    gaps.reserve(cycles.size() - 1);
    for (std::size_t index = 1; index < cycles.size(); ++index) {
        const double previousEndMs =
            std::max(0.0, cycles[index - 1].endActiveMs);
        const double nextStartMs =
            std::max(0.0, cycles[index].startActiveMs);
        gaps.push_back({previousEndMs, nextStartMs,
                        std::max(0.0, nextStartMs - previousEndMs)});
    }
    return gaps;
}

[[nodiscard]] inline double interpolatedPercentile(
    std::vector<double> values, double probability) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const double position =
        probability * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = std::min(lower + 1, values.size() - 1);
    const double fraction = position - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

[[nodiscard]] inline std::optional<double>
medianMacroGapMs(const std::vector<double>& gapDurationsMs) {
    return gapDurationsMs.empty()
               ? std::nullopt
               : std::optional<double>(
                     interpolatedPercentile(gapDurationsMs, 0.50));
}

[[nodiscard]] inline std::optional<double>
p90MacroGapMs(const std::vector<double>& gapDurationsMs) {
    return gapDurationsMs.empty()
               ? std::nullopt
               : std::optional<double>(
                     interpolatedPercentile(gapDurationsMs, 0.90));
}

} // namespace smp
