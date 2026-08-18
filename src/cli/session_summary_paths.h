#pragma once

#include <chrono>
#include <filesystem>
#include <optional>

namespace smp {

[[nodiscard]] std::filesystem::path sessionSummariesRootFromSessions(
    const std::filesystem::path& sessionsRoot);

// Compatibility wrappers used by commands.cpp / windows_application.cpp through
// source-local compile definitions. Individual-game files remain in sessions\,
// while automatic-session aggregates live in the sibling sessionSummaries\ folder.
[[nodiscard]] std::filesystem::path makeSeparatedAutomaticSessionSummaryPath(
    const std::filesystem::path& sessionsRoot,
    std::chrono::system_clock::time_point sessionStart =
        std::chrono::system_clock::now());

[[nodiscard]] std::optional<std::filesystem::path>
findLatestSeparatedAutomaticSessionSummary(
    const std::filesystem::path& sessionsRoot);

// Best-effort, non-destructive compatibility migration. Existing legacy summary
// files are copied into sessionSummaries\ and left untouched in sessions\.
void migrateLegacyAutomaticSessionSummaries(
    const std::filesystem::path& sessionsRoot) noexcept;

} // namespace smp
