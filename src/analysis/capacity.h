#pragma once

#include <array>
#include <optional>
#include <vector>

namespace scm {

struct CapacityBinInput {
    double lowerEdgeEapm{};
    std::array<std::optional<double>, 3> coreLatencyMedians;
};

// The baseline is the lowest bin with at least two populated core latency metrics.
// A breakpoint is the first higher bin where at least two comparable metrics rise by 20%.
std::optional<double> estimateCapacityBreakpoint(const std::vector<CapacityBinInput>& bins);
int mechanicalLoadBin(double eapm) noexcept;

} // namespace scm
