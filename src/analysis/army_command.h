#pragma once

#include "analysis/army_control_group.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace smp {

struct ArmyCommandCandidate {
    std::int64_t replayFrame{};
    std::size_t commandIndex{};
    double activeMs{};
    std::string kind;
    std::string order;
    std::vector<std::uint32_t> selectedUnitTags;
};

struct ArmyCommandObservation {
    double activeMs{};
    std::string kind;
    std::string order;
};

struct ArmyCommandRoleEvidence {
    std::unordered_set<std::uint32_t> workerTags;
    std::unordered_set<std::uint32_t> productionBuildingTags;
    std::unordered_set<std::uint32_t> scoutingTags;
    std::unordered_set<std::uint32_t> armyTags;
};

struct ArmyCommandAnalysis {
    bool available{};
    std::string unavailableReason;
    std::size_t commandCount{};
    double activeDurationSeconds{};
    std::vector<ArmyCommandObservation> commands;
    std::vector<double> gapDurationsMs;
    std::size_t unresolvedSelectionCommands{};

    [[nodiscard]] std::optional<double> commandsPerMinute() const noexcept;
    [[nodiscard]] std::optional<double> medianGapMs() const;
    [[nodiscard]] std::optional<double> p90GapMs() const;
    [[nodiscard]] std::optional<double> longestGapMs() const;
};

[[nodiscard]] ArmyCommandRoleEvidence buildArmyCommandRoleEvidence(
    std::unordered_set<std::uint32_t> workerTags,
    std::unordered_set<std::uint32_t> productionBuildingTags,
    const ArmyControlGroupAnalysis& controlGroups);

[[nodiscard]] ArmyCommandAnalysis analyzeArmyCommands(
    std::vector<ArmyCommandCandidate> candidates,
    const ArmyCommandRoleEvidence& evidence, double activeDurationSeconds);

} // namespace smp
