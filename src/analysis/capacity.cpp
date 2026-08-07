#include "analysis/capacity.h"

namespace scm {

int mechanicalLoadBin(double eapm) noexcept {
    if (eapm < 100.0)
        return 0;
    if (eapm < 150.0)
        return 1;
    if (eapm < 200.0)
        return 2;
    if (eapm < 250.0)
        return 3;
    if (eapm < 300.0)
        return 4;
    return 5;
}

std::optional<double> estimateCapacityBreakpoint(const std::vector<CapacityBinInput>& bins) {
    const CapacityBinInput* baseline = nullptr;
    for (const auto& bin : bins) {
        int populated = 0;
        for (const auto& metric : bin.coreLatencyMedians)
            populated += metric.has_value();
        if (populated >= 2) {
            baseline = &bin;
            break;
        }
    }
    if (!baseline)
        return std::nullopt;

    for (const auto& bin : bins) {
        if (bin.lowerEdgeEapm <= baseline->lowerEdgeEapm)
            continue;
        int degraded = 0;
        for (std::size_t i = 0; i < baseline->coreLatencyMedians.size(); ++i) {
            const auto& base = baseline->coreLatencyMedians[i];
            const auto& current = bin.coreLatencyMedians[i];
            if (base && current && *base > 0.0 && *current >= *base * 1.20)
                ++degraded;
        }
        if (degraded >= 2)
            return bin.lowerEdgeEapm;
    }
    return std::nullopt;
}

} // namespace scm
