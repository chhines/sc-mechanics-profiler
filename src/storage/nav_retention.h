#pragma once

#include "config/config.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace smp {

struct NavRetentionCandidate {
    std::filesystem::path navPath;
    std::int64_t chronology{};
    bool managedAutomatic{};
    bool completed{};
    bool derivedArtifactsPersisted{};
    bool currentlyOpen{};
};

struct NavRetentionPlan {
    std::vector<std::filesystem::path> protectedNavPaths;
    std::vector<std::filesystem::path> deletionPaths;
};

[[nodiscard]] NavRetentionPlan planNavRetention(
    const std::vector<NavRetentionCandidate>& candidates,
    NavRetentionPolicy policy,
    const std::optional<std::filesystem::path>& latestNavPath = std::nullopt);

using NavRetentionRemove = std::function<bool(
    const std::filesystem::path&, std::error_code&)>;

struct NavRetentionApplyResult {
    std::vector<std::filesystem::path> removedPaths;
    std::vector<std::filesystem::path> alreadyMissingPaths;
    std::vector<std::filesystem::path> failedPaths;
};

[[nodiscard]] NavRetentionApplyResult applyNavRetention(
    const NavRetentionPlan& plan);
[[nodiscard]] NavRetentionApplyResult applyNavRetention(
    const NavRetentionPlan& plan,
    const NavRetentionRemove& remove);

struct NavRetentionFinalizationState {
    bool automaticRecordingCompleted{};
    bool derivedAnalysisPersisted{};
    bool sessionHistoryPersisted{};
};

[[nodiscard]] bool canRunNavRetention(
    const NavRetentionFinalizationState& state) noexcept;

struct ManagedNavRetentionResult {
    bool registrationPersisted{};
    NavRetentionApplyResult cleanup;
    std::string warning;
};

// Records provenance only after a completed automatic game has both its paired
// derived JSON and automatic-session history on disk, then applies the policy.
[[nodiscard]] ManagedNavRetentionResult
recordFinalizedAutomaticNavAndApplyRetention(
    const std::filesystem::path& sessionsRoot,
    const std::filesystem::path& navPath,
    const std::filesystem::path& derivedJsonPath,
    const std::filesystem::path& sessionHistoryPath,
    NavRetentionPolicy policy) noexcept;

// Reuses the same provenance and artifact checks for a settings-initiated pass.
[[nodiscard]] ManagedNavRetentionResult applyManagedNavRetention(
    const std::filesystem::path& sessionsRoot,
    NavRetentionPolicy policy) noexcept;

} // namespace smp
