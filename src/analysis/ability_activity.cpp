#include "analysis/ability_activity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>
#include <utility>

namespace smp {
namespace {

using AbilityName = std::pair<std::string_view, std::string_view>;

constexpr std::array targetedAbilities{
    AbilityName{"CastPsionicStorm", "Psionic Storm"},
    AbilityName{"CastHallucination", "Hallucination"},
    AbilityName{"CastRecall", "Recall"},
    AbilityName{"CastStasisField", "Stasis Field"},
    AbilityName{"CastDisruptionWeb", "Disruption Web"},
    AbilityName{"CastMindControl", "Mind Control"},
    AbilityName{"CastFeedback", "Feedback"},
    AbilityName{"CastMaelstrom", "Maelstrom"},
    AbilityName{"FireYamatoGun", "Yamato Gun"},
    AbilityName{"CastLockdown", "Lockdown"},
    AbilityName{"CastEMPShockwave", "EMP Shockwave"},
    AbilityName{"CastDefensiveMatrix", "Defensive Matrix"},
    AbilityName{"CastIrradiate", "Irradiate"},
    AbilityName{"CastRestoration", "Restoration"},
    AbilityName{"CastOpticalFlare", "Optical Flare"},
    AbilityName{"CastNuclearStrike", "Nuclear Strike"},
    AbilityName{"CastScannerSweep", "Scanner Sweep"},
    AbilityName{"CastDarkSwarm", "Dark Swarm"},
    AbilityName{"CastPlague", "Plague"},
    AbilityName{"CastConsume", "Consume"},
    AbilityName{"CastEnsnare", "Ensnare"},
    AbilityName{"CastParasite", "Parasite"},
    AbilityName{"CastSpawnBroodlings", "Spawn Broodlings"},
    AbilityName{"CastInfestation", "Infestation"},
    AbilityName{"PlaceMine", "Spider Mine"},
};

constexpr std::array directAbilities{
    AbilityName{"Stim", "Stim"},
    AbilityName{"Siege", "Siege"},
    AbilityName{"Unsiege", "Unsiege"},
    AbilityName{"Cloack", "Cloak"},
    AbilityName{"Decloack", "Decloak"},
    AbilityName{"Burrow", "Burrow"},
    AbilityName{"Unburrow", "Unburrow"},
};

template <std::size_t Size>
std::optional<std::string_view> mappedName(
    std::string_view raw,
    const std::array<AbilityName, Size>& mappings) noexcept {
    const auto found = std::find_if(
        mappings.begin(), mappings.end(), [raw](const AbilityName& mapping) {
            return mapping.first == raw;
        });
    return found == mappings.end()
               ? std::nullopt
               : std::optional<std::string_view>(found->second);
}

} // namespace

std::size_t AbilityActivityAnalysis::totalUses() const noexcept {
    return observations.size();
}

std::optional<double>
AbilityActivityAnalysis::abilitiesPerMinute() const noexcept {
    if (!available || activeDurationSeconds <= 0.0)
        return std::nullopt;
    return static_cast<double>(totalUses()) /
           (activeDurationSeconds / 60.0);
}

std::optional<double> AbilityActivityAnalysis::usesPerMinute(
    std::string_view ability) const {
    if (!available || activeDurationSeconds <= 0.0)
        return std::nullopt;
    const auto found = usesByAbility.find(std::string(ability));
    const std::size_t uses =
        found == usesByAbility.end() ? 0 : found->second;
    return static_cast<double>(uses) / (activeDurationSeconds / 60.0);
}

std::vector<AbilityStatistics> AbilityActivityAnalysis::statistics() const {
    std::vector<AbilityStatistics> result;
    result.reserve(usesByAbility.size());
    for (const auto& [ability, uses] : usesByAbility) {
        if (uses > 0)
            result.push_back({ability, uses});
    }
    std::sort(result.begin(), result.end(), [](const auto& first,
                                               const auto& second) {
        return first.uses != second.uses
                   ? first.uses > second.uses
                   : first.ability < second.ability;
    });
    return result;
}

std::optional<std::string_view>
abilityCommandName(std::string_view kind, std::string_view order) noexcept {
    if (kind == "Targeted Order")
        return mappedName(order, targetedAbilities);
    if (!order.empty())
        return std::nullopt;
    return mappedName(kind, directAbilities);
}

AbilityActivityAnalysis analyzeAbilityActivity(
    std::vector<AbilityCommandCandidate> candidates,
    double activeDurationSeconds) {
    AbilityActivityAnalysis analysis;
    analysis.available = true;
    analysis.activeDurationSeconds = std::max(0.0, activeDurationSeconds);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& first, const auto& second) {
                         return std::tie(first.replayFrame,
                                         first.commandIndex) <
                                std::tie(second.replayFrame,
                                         second.commandIndex);
                     });

    const double activeDurationMs = analysis.activeDurationSeconds * 1000.0;
    for (const auto& candidate : candidates) {
        if (!std::isfinite(candidate.activeMs) || candidate.activeMs < 0.0 ||
            candidate.activeMs > activeDurationMs || candidate.ability.empty())
            continue;
        analysis.observations.push_back(
            {candidate.activeMs, candidate.ability});
        ++analysis.usesByAbility[candidate.ability];
    }
    return analysis;
}

} // namespace smp
