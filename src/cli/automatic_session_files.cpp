#include "cli/automatic_session_files.h"

#include "cli/report.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <windows.h>

namespace smp {
namespace {

constexpr std::string_view automaticSessionSummarySuffix = "_session.txt";

std::string automaticSessionStartId(std::chrono::system_clock::time_point time) {
    const std::time_t value = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    localtime_s(&local, &value);
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d_%H%M%S");
    return output.str();
}

void addArtifact(std::set<std::filesystem::path>& paths, const std::filesystem::path& path) {
    if (!path.empty())
        paths.insert(path);
}

bool isAutomaticSessionSummary(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    return filename.size() > automaticSessionSummarySuffix.size() &&
           filename.ends_with(automaticSessionSummarySuffix);
}

std::string summarySortKey(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    return filename.substr(0, filename.size() - automaticSessionSummarySuffix.size());
}

} // namespace

AutomaticRecordingDiscardResult discardAbortedAutomaticRecordingFiles(
    const AutomaticRecordingArtifacts& artifacts) {
    AutomaticRecordingDiscardResult result;
    std::set<std::filesystem::path> paths;
    addArtifact(paths, artifacts.navPath);
    addArtifact(paths, artifacts.jsonPath);
    addArtifact(paths, artifacts.rawPath);
    if (!artifacts.navPath.empty()) {
        addArtifact(paths, std::filesystem::path(artifacts.navPath.string() + ".tmp"));
        auto derivedJson = artifacts.navPath;
        derivedJson.replace_extension(".json");
        addArtifact(paths, derivedJson);
        addArtifact(paths, std::filesystem::path(derivedJson.string() + ".tmp"));
    }
    if (!artifacts.jsonPath.empty())
        addArtifact(paths, std::filesystem::path(artifacts.jsonPath.string() + ".tmp"));

    for (const auto& path : paths) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            result.failedPaths.push_back(path);
            continue;
        }
        if (!exists)
            continue;
        if (!std::filesystem::is_regular_file(path, error) || error ||
            !std::filesystem::remove(path, error) || error) {
            result.failedPaths.push_back(path);
            continue;
        }
        ++result.removedFiles;
    }
    return result;
}

std::filesystem::path makeAutomaticSessionSummaryPath(
    const std::filesystem::path& sessionsRoot,
    std::chrono::system_clock::time_point sessionStart) {
    const auto base = automaticSessionStartId(sessionStart);
    auto candidate = sessionsRoot / (base + std::string(automaticSessionSummarySuffix));
    for (std::size_t suffix = 1; std::filesystem::exists(candidate); ++suffix)
        candidate = sessionsRoot / (base + "_" + std::to_string(suffix) +
                                    std::string(automaticSessionSummarySuffix));
    return candidate;
}

void writeAutomaticSessionSummary(const std::filesystem::path& summaryPath,
                                  const std::string& report) {
    std::filesystem::create_directories(summaryPath.parent_path());
    const auto temporary = std::filesystem::path(summaryPath.string() + ".tmp");
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Unable to create automatic session summary");
        output.write(report.data(), static_cast<std::streamsize>(report.size()));
        output.flush();
        if (!output)
            throw std::runtime_error("Unable to write automatic session summary");
        output.close();
        if (!MoveFileExW(temporary.c_str(), summaryPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Unable to finalize automatic session summary: " +
                                     std::system_category().message(
                                         static_cast<int>(GetLastError())));
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void writeAutomaticSessionSummary(
    const std::filesystem::path& summaryPath,
    const AutomaticSessionState& session,
    const ReportVisibilityProvider& currentReportVisibility) {
    const ReportGroupVisibility visibility =
        currentReportVisibility ? currentReportVisibility()
                                : ReportGroupVisibility{};
    writeAutomaticSessionSummary(
        summaryPath, formatAutomaticSessionReport(session, visibility));
}

std::optional<std::filesystem::path> findLatestAutomaticSessionSummary(
    const std::filesystem::path& sessionsRoot) {
    std::error_code error;
    if (!std::filesystem::is_directory(sessionsRoot, error) || error)
        return std::nullopt;

    std::optional<std::filesystem::path> latest;
    std::string latestKey;
    std::filesystem::recursive_directory_iterator iterator(
        sessionsRoot, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto& entry = *iterator;
        std::error_code entryError;
        if (entry.is_regular_file(entryError) && !entryError &&
            isAutomaticSessionSummary(entry.path())) {
            const auto key = summarySortKey(entry.path());
            if (!latest || key > latestKey || (key == latestKey && entry.path() > *latest)) {
                latest = entry.path();
                latestKey = key;
            }
        }
        iterator.increment(error);
    }
    return latest;
}

} // namespace smp
