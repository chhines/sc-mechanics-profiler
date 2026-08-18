#include "test_framework.h"

#include "cli/session_summary_paths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path temporaryRoot() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("starcraft-mechanics-profiler-summary-paths-" +
                       std::to_string(nonce));
    std::filesystem::create_directories(root);
    return root;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

} // namespace

TEST_CASE("automatic session summaries use sibling sessionSummaries folder") {
    const auto root = temporaryRoot();
    const auto sessions = root / "sessions";
    std::filesystem::create_directories(sessions);

    const auto summaries = smp::sessionSummariesRootFromSessions(sessions);
    REQUIRE(summaries == root / "sessionSummaries");

    const auto start = std::chrono::system_clock::time_point(
        std::chrono::seconds(1'786'435'200));
    const auto summary =
        smp::makeSeparatedAutomaticSessionSummaryPath(sessions, start);
    REQUIRE(summary.parent_path() == summaries);
    REQUIRE(summary.filename().string().ends_with("_session.txt"));

    std::filesystem::remove_all(root);
}

TEST_CASE("legacy automatic summaries are copied non-destructively into sessionSummaries") {
    const auto root = temporaryRoot();
    const auto sessions = root / "sessions";
    const auto legacyFolder = sessions / "2026-08-17";
    const auto legacyText = legacyFolder / "2026-08-17_210000_session.txt";
    const auto legacyJson = legacyFolder / "2026-08-17_210000_session.json";
    writeText(legacyText, "legacy text");
    writeText(legacyJson, "{\"legacy\":true}");

    smp::migrateLegacyAutomaticSessionSummaries(sessions);

    const auto summaries = root / "sessionSummaries";
    REQUIRE(std::filesystem::is_regular_file(
        summaries / legacyText.filename()));
    REQUIRE(std::filesystem::is_regular_file(
        summaries / legacyJson.filename()));
    REQUIRE(std::filesystem::is_regular_file(legacyText));
    REQUIRE(std::filesystem::is_regular_file(legacyJson));

    const auto latest =
        smp::findLatestSeparatedAutomaticSessionSummary(sessions);
    REQUIRE(latest.has_value());
    REQUIRE(latest->parent_path() == summaries);
    REQUIRE(latest->filename() == legacyText.filename());

    std::filesystem::remove_all(root);
}
