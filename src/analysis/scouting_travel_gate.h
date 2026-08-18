#pragma once

#include "analysis/army_control_group.h"

namespace smp {

// This is intentionally a generous physical ceiling rather than an estimate of
// exact Brood War worker movement. It keeps the arrival gate conservative while
// still rejecting commands that could not plausibly represent an arrived scout.
inline constexpr double scoutingWorkerSpeedCeilingPixelsPerSecond = 200.0;

void applyTravelGatedScoutingUnitClassification(
    ArmyControlGroupAnalysis& analysis,
    const std::vector<ScoutingUnitCommandEvidence>& commandEvidence = {});

void analyzeTravelGatedScoutingUnitActivity(ArmyControlGroupAnalysis& analysis,
                                            const AnalysisResult& result,
                                            std::uint64_t qpcFrequency);

} // namespace smp
