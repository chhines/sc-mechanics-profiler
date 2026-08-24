#pragma once

#include <cstddef>
#include <optional>

namespace smp::analysis_insights {

[[nodiscard]] inline bool hasAbilityActivityForDisplay(
    const std::optional<std::size_t>& totalAbilityUses) noexcept {
    return totalAbilityUses && *totalAbilityUses > 0;
}

} // namespace smp::analysis_insights
