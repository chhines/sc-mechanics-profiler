#include "test_framework.h"

#include "app/session_trend_history.h"
#include "cli/session_summary_paths.h"
#include "storage/nav_retention.h"
#include "storage/session.h"

#include <chrono>
#include <fstream>
#include <iterator>

namespace {

struct TemporaryHistory {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("smp-session-trends-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TemporaryHistory() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("session trends skip zero game JSON history without affecting persistence or NAV retention") {
    TemporaryHistory files;
    const auto summaries = files.root / "sessionSummaries";
    const auto sessions = files.root / "sessions";
    smp::AnalysisResult recording;
    recording.activeDurationSeconds = 60.0;

    // Create real persisted histories, including an all-excluded automatic run.
    for (const auto& [id, games] :
         {std::pair{"A", 2}, std::pair{"B", 0}, std::pair{"C", 3}}) {
        smp::AutomaticSessionState session;
        if (games == 0)
            REQUIRE(session.markExcludedGeneration(1));
        for (int generation = 1; generation <= games; ++generation)
            REQUIRE(session.addFinalizedGame(generation, recording));
        smp::writeSeparatedAutomaticSessionHistory(
            summaries / (std::string(id) + "_session.json"), session);
    }
    const auto excludedPath = summaries / "B_session.json";
    const auto excludedBytes = readText(excludedPath);

    const auto history = smp::loadSessionTrendHistory(summaries);
    REQUIRE(history.points.size() == 2);
    REQUIRE(history.jsonSessions == 2);
    REQUIRE(history.legacyTextSessions == 0);
    REQUIRE(history.points[0].sessionId == "A");
    REQUIRE(history.points[0].overall.games == 2);
    REQUIRE(history.points[1].sessionId == "C");
    REQUIRE(history.points[1].overall.games == 3);

    // Test the same projection used by the chart, not just its axis labels.
    const auto series = smp::sessionKpiTrendSeries(
        history, "All matchups", smp::SessionKpi::NavigationTransitionsPerMinute);
    REQUIRE(series.xs == std::vector<double>({1.0, 2.0}));
    REQUIRE(series.ys == std::vector<double>({0.0, 0.0}));
    REQUIRE(series.sourceIndices == std::vector<std::size_t>({0, 1}));
    REQUIRE(smp::sessionTrendTickValues(history.points.size()) == series.xs);

    std::filesystem::create_directories(sessions);
    const auto nav = smp::writeNavSession(sessions / "B.nav", recording,
                                         "B", 1000, 1000);
    const auto derivedJson = smp::writeAnalysisJson(
        nav, smp::analysisToJson(recording, "B"));
    const auto retention = smp::recordFinalizedAutomaticNavAndApplyRetention(
        sessions, nav, derivedJson, excludedPath,
        {smp::NavRetentionMode::KeepAll, 10});
    REQUIRE(retention.registrationPersisted);
    REQUIRE(retention.warning.empty());
    REQUIRE(retention.cleanup.removedPaths.empty());
    REQUIRE(std::filesystem::is_regular_file(nav));

    // Filtering leaves the source readable for export/debug and bookkeeping.
    REQUIRE(std::filesystem::is_regular_file(excludedPath));
    REQUIRE(readText(excludedPath) == excludedBytes);
    const auto excluded = smp::json::parseFile(excludedPath);
    REQUIRE(excluded["schema_version"].asInt() == 3);
    REQUIRE(excluded["overall"]["games"].asInt() == 0);
    REQUIRE(excluded["games"].asArray().empty());
    REQUIRE(smp::decodeSessionTrendStats(excluded["overall"]).games == 0);
}
