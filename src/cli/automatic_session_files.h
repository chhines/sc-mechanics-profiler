#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace smp {

struct AutomaticRecordingArtifacts {
    std::filesystem::path navPath;
    std::filesystem::path jsonPath;
    std::filesystem::path rawPath;
};

struct AutomaticRecordingDiscardResult {
    std::size_t removedFiles{};
    std::vector<std::filesystem::path> failedPaths;
};

[[nodiscard]] AutomaticRecordingDiscardResult discardAbortedAutomaticRecordingFiles(
    const AutomaticRecordingArtifacts& artifacts);

[[nodiscard]] std::filesystem::path makeAutomaticSessionSummaryPath(
    const std::filesystem::path& sessionsRoot,
    std::chrono::system_clock::time_point sessionStart = std::chrono::system_clock::now());

void writeAutomaticSessionSummary(const std::filesystem::path& summaryPath,
                                  const std::string& report);

[[nodiscard]] std::optional<std::filesystem::path> findLatestAutomaticSessionSummary(
    const std::filesystem::path& sessionsRoot);

} // namespace smp
