#include "cli/session_summary_paths.h"

#include "cli/session_history_writer.h"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace smp {
namespace {

constexpr std::string_view jsonSuffix = "_session.json";
constexpr std::string_view textSuffix = "_session.txt";

bool isSessionData(const std::filesystem::path& path) {
    return path.filename().string().ends_with(jsonSuffix);
}

bool isLegacyReadableSummary(const std::filesystem::path& path) {
    return path.filename().string().ends_with(textSuffix);
}

std::string sessionBase(std::chrono::system_clock::time_point time) {
    const std::time_t value = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    localtime_s(&local, &value);
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d_%H%M%S");
    return output.str();
}

std::optional<std::filesystem::path>
findLatestSessionData(const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error)
        return std::nullopt;
    std::optional<std::filesystem::path> latest;
    std::string latestName;
    for (std::filesystem::directory_iterator iterator(
             root, std::filesystem::directory_options::skip_permission_denied,
             error), end;
         !error && iterator != end; iterator.increment(error)) {
        std::error_code entryError;
        if (!iterator->is_regular_file(entryError) || entryError ||
            !isSessionData(iterator->path()))
            continue;
        const auto filename = iterator->path().filename().string();
        if (!latest || filename > latestName) {
            latest = iterator->path();
            latestName = filename;
        }
    }
    return latest;
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
    std::filesystem::create_directories(summariesRoot);
    const auto base = sessionBase(sessionStart);
    auto candidate = summariesRoot / (base + std::string(jsonSuffix));
    for (std::size_t suffix = 1; std::filesystem::exists(candidate); ++suffix) {
        candidate = summariesRoot /
                    (base + "_" + std::to_string(suffix) +
                     std::string(jsonSuffix));
    }
    return candidate;
}

void writeSeparatedAutomaticSessionHistory(
    const std::filesystem::path& dataPath,
    const AutomaticSessionState& session,
    const ReportVisibilityProvider& currentReportVisibility) {
    (void)currentReportVisibility;
    writeAutomaticSessionHistoryJson(dataPath, session);
}

std::optional<std::filesystem::path>
findLatestSeparatedAutomaticSessionSummary(
    const std::filesystem::path& sessionsRoot) {
    migrateLegacyAutomaticSessionSummaries(sessionsRoot);
    const auto summariesRoot = sessionSummariesRootFromSessions(sessionsRoot);
    if (const auto latest = findLatestSessionData(summariesRoot))
        return latest;

    // Compatibility fallback for older history that could not be copied into
    // sessionSummaries\. Machine-readable JSON remains preferred whenever it
    // exists; readable text is read-only legacy support and is never migrated
    // or produced by the current automatic-session writer.
    std::error_code error;
    if (!std::filesystem::is_directory(sessionsRoot, error) || error)
        return std::nullopt;
    std::optional<std::filesystem::path> latestJson;
    std::string latestJsonName;
    std::optional<std::filesystem::path> latestText;
    std::string latestTextName;
    std::filesystem::recursive_directory_iterator iterator(
        sessionsRoot, std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        std::error_code entryError;
        if (iterator->is_regular_file(entryError) && !entryError) {
            const auto filename = iterator->path().filename().string();
            if (isSessionData(iterator->path())) {
                if (!latestJson || filename > latestJsonName) {
                    latestJson = iterator->path();
                    latestJsonName = filename;
                }
            } else if (isLegacyReadableSummary(iterator->path())) {
                if (!latestText || filename > latestTextName) {
                    latestText = iterator->path();
                    latestTextName = filename;
                }
            }
        }
        iterator.increment(error);
    }
    return latestJson ? latestJson : latestText;
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
                isSessionData(entry.path())) {
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
        // Legacy migration is a convenience path. New history writing must not
        // fail just because an old file could not be copied.
    }
}

} // namespace smp
