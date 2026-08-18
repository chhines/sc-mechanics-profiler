#include "cli/session_summary_paths.h"

#include "cli/automatic_session_files.h"

#include <string_view>
#include <system_error>

namespace smp {
namespace {

constexpr std::string_view textSuffix = "_session.txt";
constexpr std::string_view jsonSuffix = "_session.json";

bool isSummaryArtifact(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    return filename.ends_with(textSuffix) || filename.ends_with(jsonSuffix);
}

} // namespace

std::filesystem::path sessionSummariesRootFromSessions(
    const std::filesystem::path& sessionsRoot) {
    const auto parent = sessionsRoot.parent_path();
    return parent.empty() ? std::filesystem::path("sessionSummaries")
                          : parent / "sessionSummaries";
}

std::filesystem::path makeSeparatedAutomaticSessionSummaryPath(
    const std::filesystem::path& sessionsRoot,
    std::chrono::system_clock::time_point sessionStart) {
    const auto summariesRoot = sessionSummariesRootFromSessions(sessionsRoot);
    migrateLegacyAutomaticSessionSummaries(sessionsRoot);
    return makeAutomaticSessionSummaryPath(summariesRoot, sessionStart);
}

std::optional<std::filesystem::path>
findLatestSeparatedAutomaticSessionSummary(
    const std::filesystem::path& sessionsRoot) {
    migrateLegacyAutomaticSessionSummaries(sessionsRoot);
    const auto summariesRoot = sessionSummariesRootFromSessions(sessionsRoot);
    if (const auto latest = findLatestAutomaticSessionSummary(summariesRoot))
        return latest;
    return findLatestAutomaticSessionSummary(sessionsRoot);
}

void migrateLegacyAutomaticSessionSummaries(
    const std::filesystem::path& sessionsRoot) noexcept {
    try {
        std::error_code error;
        if (!std::filesystem::is_directory(sessionsRoot, error) || error)
            return;

        const auto summariesRoot = sessionSummariesRootFromSessions(sessionsRoot);
        std::filesystem::create_directories(summariesRoot, error);
        if (error)
            return;

        std::filesystem::recursive_directory_iterator iterator(
            sessionsRoot,
            std::filesystem::directory_options::skip_permission_denied,
            error);
        const std::filesystem::recursive_directory_iterator end;
        while (!error && iterator != end) {
            const auto entry = *iterator;
            std::error_code entryError;
            if (entry.is_regular_file(entryError) && !entryError &&
                isSummaryArtifact(entry.path())) {
                const auto destination = summariesRoot / entry.path().filename();
                std::error_code copyError;
                if (!std::filesystem::exists(destination, copyError) && !copyError) {
                    std::filesystem::copy_file(
                        entry.path(), destination,
                        std::filesystem::copy_options::skip_existing,
                        copyError);
                }
            }
            iterator.increment(error);
        }
    } catch (...) {
        // Legacy migration is a convenience path. New summary writing must not
        // fail just because an old file could not be copied.
    }
}

} // namespace smp
