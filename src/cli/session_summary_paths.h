#pragma once

#include "app/gui_preferences.h"
#include "cli/automatic_session_stats.h"

#include <chrono>
#include <filesystem>
#include <optional>

namespace smp {

[[nodiscard]] std::filesystem::path sessionSummariesRootFromSessions(
    const std::filesystem::path& sessionsRoot);

// Individual-game files remain in sessions\ while automatic-session history is
// persisted as machine-readable *_session.json files in sibling sessionSummaries\.
[[nodiscard]] std::filesystem::path makeSeparatedAutomaticSessionSummaryPath(
    const std::filesystem::path& sessionsRoot,
    std::chrono::system_clock::time_point sessionStart =
        std::chrono::system_clock::now());

// Runtime compatibility wrapper for commands.cpp. It writes the machine-readable
// history directly; it never creates or formats a readable .txt summary.
void writeSeparatedAutomaticSessionHistory(
    const std::filesystem::path& dataPath,
    const AutomaticSessionState& session,
    const ReportVisibilityProvider& currentReportVisibility = {});

[[nodiscard]] std::optional<std::filesystem::path>
findLatestSeparatedAutomaticSessionSummary(
    const std::filesystem::path& sessionsRoot);

// Best-effort, non-destructive compatibility migration. Only legacy machine-
// readable *_session.json files are copied into sessionSummaries\; readable
// text reports are intentionally left where they are.
void migrateLegacyAutomaticSessionSummaries(
    const std::filesystem::path& sessionsRoot) noexcept;

} // namespace smp
