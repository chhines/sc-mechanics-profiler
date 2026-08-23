#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smp {

struct AbilityCommandCandidate {
    std::int64_t replayFrame{};
    std::size_t commandIndex{};
    double activeMs{};
    std::string ability;
};

struct AbilityObservation {
    double activeMs{};
    std::string ability;
};

struct AbilityStatistics {
    std::string ability;
    std::size_t uses{};
};

struct AbilityActivityAnalysis {
    bool available{};
    std::string unavailableReason;
    double activeDurationSeconds{};
    std::vector<AbilityObservation> observations;
    std::map<std::string, std::size_t> usesByAbility;

    [[nodiscard]] std::size_t totalUses() const noexcept;
    [[nodiscard]] std::optional<double> abilitiesPerMinute() const noexcept;
    [[nodiscard]] std::optional<double>
    usesPerMinute(std::string_view ability) const;
    [[nodiscard]] std::vector<AbilityStatistics> statistics() const;
};

[[nodiscard]] std::optional<std::string_view>
abilityCommandName(std::string_view kind, std::string_view order) noexcept;

[[nodiscard]] AbilityActivityAnalysis analyzeAbilityActivity(
    std::vector<AbilityCommandCandidate> candidates,
    double activeDurationSeconds);

} // namespace smp
