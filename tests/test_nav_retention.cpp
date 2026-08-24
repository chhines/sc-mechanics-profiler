#include "test_framework.h"

#include "cli/automatic_session_stats.h"
#include "cli/session_summary_paths.h"
#include "storage/nav_retention.h"
#include "storage/session.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::filesystem::path temporaryRoot(const char* label) {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("starcraft-mechanics-profiler-") + label +
                       "-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    return root;
}

smp::NavRetentionCandidate candidate(const char* filename,
                                     std::int64_t chronology,
                                     bool derived = true) {
    return {std::filesystem::path(filename), chronology, true, true, derived,
            false};
}

void writeText(const std::filesystem::path& path, const char* text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

struct PersistedGame {
    std::filesystem::path nav;
    std::filesystem::path json;
};

PersistedGame writePersistedGame(const std::filesystem::path& sessions,
                                 const std::string& id,
                                 std::int64_t chronology) {
    smp::AnalysisResult analysis;
    analysis.activeDurationSeconds = 60.0;
    const auto nav = sessions / (id + ".nav");
    smp::writeNavSession(nav, analysis, id, 1000, chronology);
    const auto json = smp::writeAnalysisJson(
        nav, smp::analysisToJson(analysis, id));
    return {nav, json};
}

} // namespace

TEST_CASE("keep all protects every completed automatic nav") {
    const std::vector<smp::NavRetentionCandidate> candidates{
        candidate("a.nav", 1), candidate("b.nav", 2),
        candidate("c.nav", 3)};
    const auto plan = smp::planNavRetention(
        candidates, {smp::NavRetentionMode::KeepAll, 1});
    REQUIRE(plan.deletionPaths.empty());
    REQUIRE(plan.protectedNavPaths.size() == 3);
}

TEST_CASE("keep last one and keep last N pack the retained chronology exactly") {
    const std::vector<smp::NavRetentionCandidate> candidates{
        candidate("a.nav", 1), candidate("b.nav", 2),
        candidate("c.nav", 3), candidate("d.nav", 4)};

    const auto keepOne = smp::planNavRetention(
        candidates, {smp::NavRetentionMode::KeepLastGames, 1});
    REQUIRE(keepOne.protectedNavPaths.size() == 1);
    REQUIRE(keepOne.protectedNavPaths[0] == std::filesystem::path("d.nav"));
    REQUIRE(keepOne.deletionPaths.size() == 3);

    const auto keepThree = smp::planNavRetention(
        candidates, {smp::NavRetentionMode::KeepLastGames, 3});
    REQUIRE(keepThree.protectedNavPaths.size() == 3);
    REQUIRE(keepThree.deletionPaths.size() == 1);
    REQUIRE(keepThree.deletionPaths[0] == std::filesystem::path("a.nav"));

    const auto keepMoreThanAvailable = smp::planNavRetention(
        candidates, {smp::NavRetentionMode::KeepLastGames, 10});
    REQUIRE(keepMoreThanAvailable.deletionPaths.empty());
}

TEST_CASE("retention excludes unsafe candidates and always protects the latest") {
    auto incomplete = candidate("incomplete.nav", 1);
    incomplete.completed = false;
    auto manual = candidate("manual.nav", 2);
    manual.managedAutomatic = false;
    auto missingDerived = candidate("missing-derived.nav", 3, false);
    auto open = candidate("open.nav", 4);
    open.currentlyOpen = true;
    const std::vector<smp::NavRetentionCandidate> candidates{
        incomplete, manual, missingDerived, open, candidate("latest.nav", 5)};

    const auto plan = smp::planNavRetention(
        candidates, {smp::NavRetentionMode::KeepLastGames, 1});
    REQUIRE(plan.deletionPaths.empty());

    const std::vector<smp::NavRetentionCandidate> unusual{
        candidate("bookkeeping-latest.nav", 1), candidate("middle.nav", 2),
        candidate("chronology-latest.nav", 3)};
    const auto explicitlyProtected = smp::planNavRetention(
        unusual, {smp::NavRetentionMode::KeepLastGames, 1},
        std::filesystem::path("bookkeeping-latest.nav"));
    REQUIRE(explicitlyProtected.deletionPaths.size() == 1);
    REQUIRE(explicitlyProtected.deletionPaths[0] ==
            std::filesystem::path("middle.nav"));
}

TEST_CASE("zero retention counts still protect one latest game") {
    const std::vector<smp::NavRetentionCandidate> candidates{
        candidate("old.nav", 1), candidate("latest.nav", 2)};
    const auto plan = smp::planNavRetention(
        candidates, {smp::NavRetentionMode::KeepLastGames, 0});
    REQUIRE(plan.protectedNavPaths.size() == 1);
    REQUIRE(plan.protectedNavPaths[0] ==
            std::filesystem::path("latest.nav"));
    REQUIRE(plan.deletionPaths.size() == 1);
    REQUIRE(plan.deletionPaths[0] == std::filesystem::path("old.nav"));
}

TEST_CASE("cleanup requires completed automatic finalization and both artifacts") {
    REQUIRE(smp::canRunNavRetention({true, true, true}));
    REQUIRE(!smp::canRunNavRetention({false, true, true}));
    REQUIRE(!smp::canRunNavRetention({true, false, true}));
    REQUIRE(!smp::canRunNavRetention({true, true, false}));
}

TEST_CASE("retention deletion is idempotent and preserves summaries") {
    const auto root = temporaryRoot("nav-retention-apply");
    const auto nav = root / "old.nav";
    const auto summary = root / "old.json";
    writeText(nav, "nav");
    writeText(summary, "summary");
    smp::NavRetentionPlan plan;
    plan.deletionPaths.push_back(nav);

    const auto first = smp::applyNavRetention(plan);
    REQUIRE(first.removedPaths.size() == 1);
    REQUIRE(!std::filesystem::exists(nav));
    REQUIRE(std::filesystem::is_regular_file(summary));

    const auto second = smp::applyNavRetention(plan);
    REQUIRE(second.failedPaths.empty());
    REQUIRE(second.alreadyMissingPaths.size() == 1);
    REQUIRE(std::filesystem::is_regular_file(summary));
    std::filesystem::remove_all(root);
}

TEST_CASE("one deletion failure does not prevent another deletion") {
    smp::NavRetentionPlan plan;
    plan.deletionPaths = {std::filesystem::path("blocked.nav"),
                          std::filesystem::path("removed.nav")};
    const auto result = smp::applyNavRetention(
        plan, [](const std::filesystem::path& path, std::error_code& error) {
            if (path == std::filesystem::path("blocked.nav")) {
                error = std::make_error_code(std::errc::permission_denied);
                return false;
            }
            return true;
        });
    REQUIRE(result.failedPaths.size() == 1);
    REQUIRE(result.failedPaths[0] ==
            std::filesystem::path("blocked.nav"));
    REQUIRE(result.removedPaths.size() == 1);
    REQUIRE(result.removedPaths[0] ==
            std::filesystem::path("removed.nav"));
}

TEST_CASE("managed existing-data cleanup deletes only indexed automatic navs") {
    const auto root = temporaryRoot("managed-nav-retention");
    const auto sessions = root / "sessions";
    const auto histories = root / "sessionSummaries";
    const auto history = histories / "2026-08-23_120000_session.json";
    writeText(history, "{\"overall\":{},\"games\":[{}]}");
    const auto first = writePersistedGame(sessions, "first", 1000);
    const auto second = writePersistedGame(sessions, "second", 2000);
    const auto latest = writePersistedGame(sessions, "latest", 3000);
    const auto manual = writePersistedGame(sessions, "manual", 4000);

    for (const auto* game : {&first, &second, &latest}) {
        const auto registration =
            smp::recordFinalizedAutomaticNavAndApplyRetention(
                sessions, game->nav, game->json, history,
                {smp::NavRetentionMode::KeepAll, 10});
        REQUIRE(registration.registrationPersisted);
        REQUIRE(registration.warning.empty());
    }

    const auto cleanup = smp::applyManagedNavRetention(
        sessions, {smp::NavRetentionMode::KeepLastGames, 2});
    REQUIRE(cleanup.warning.empty());
    REQUIRE(cleanup.cleanup.removedPaths.size() == 1);
    REQUIRE(!std::filesystem::exists(first.nav));
    REQUIRE(std::filesystem::is_regular_file(first.json));
    REQUIRE(std::filesystem::is_regular_file(second.nav));
    REQUIRE(std::filesystem::is_regular_file(latest.nav));
    REQUIRE(std::filesystem::is_regular_file(manual.nav));
    REQUIRE(std::filesystem::is_regular_file(history));

    const auto repeated = smp::applyManagedNavRetention(
        sessions, {smp::NavRetentionMode::KeepLastGames, 2});
    REQUIRE(repeated.cleanup.failedPaths.empty());
    REQUIRE(repeated.cleanup.removedPaths.empty());
    std::filesystem::remove_all(root);
}

TEST_CASE("missing derived artifacts prevent automatic nav registration") {
    const auto root = temporaryRoot("nav-retention-missing-derived");
    const auto sessions = root / "sessions";
    const auto history =
        root / "sessionSummaries" / "2026-08-23_130000_session.json";
    writeText(history, "{\"overall\":{},\"games\":[{}]}");
    smp::AnalysisResult analysis;
    const auto nav = sessions / "unpersisted.nav";
    smp::writeNavSession(nav, analysis, "unpersisted", 1000, 1000);
    const auto missingJson = sessions / "unpersisted.json";

    const auto registration =
        smp::recordFinalizedAutomaticNavAndApplyRetention(
            sessions, nav, missingJson, history,
            {smp::NavRetentionMode::KeepLastGames, 1});
    REQUIRE(!registration.registrationPersisted);
    REQUIRE(!registration.warning.empty());
    REQUIRE(std::filesystem::is_regular_file(nav));
    std::filesystem::remove_all(root);
}

TEST_CASE("production automatic history path registers and prunes nav files") {
    const auto root = temporaryRoot("nav-retention-production-wiring");
    const auto sessions = root / "sessions";
    const auto history = smp::makeSeparatedAutomaticSessionSummaryPath(
        sessions, std::chrono::system_clock::time_point(
                      std::chrono::seconds(1'787'488'000)));
    REQUIRE(history.parent_path() == root / "sessionSummaries");
    REQUIRE(history.filename().string().ends_with("_session.json"));

    smp::AutomaticSessionState automaticSession;
    smp::AnalysisResult analysis;
    analysis.activeDurationSeconds = 60.0;
    REQUIRE(automaticSession.addFinalizedGame(1, analysis));
    smp::writeSeparatedAutomaticSessionHistory(history, automaticSession);
    REQUIRE(std::filesystem::is_regular_file(history));

    const auto older = writePersistedGame(sessions, "older", 1000);
    const auto olderRegistration =
        smp::recordFinalizedAutomaticNavAndApplyRetention(
            sessions, older.nav, older.json, history,
            {smp::NavRetentionMode::KeepAll, 10});
    REQUIRE(olderRegistration.registrationPersisted);

    REQUIRE(automaticSession.addFinalizedGame(2, analysis));
    smp::writeSeparatedAutomaticSessionHistory(history, automaticSession);
    const auto latest = writePersistedGame(sessions, "latest", 2000);
    const auto latestRegistration =
        smp::recordFinalizedAutomaticNavAndApplyRetention(
            sessions, latest.nav, latest.json, history,
            {smp::NavRetentionMode::KeepLastGames, 1});
    REQUIRE(latestRegistration.registrationPersisted);
    REQUIRE(latestRegistration.cleanup.removedPaths.size() == 1);
    REQUIRE(!std::filesystem::exists(older.nav));
    REQUIRE(std::filesystem::is_regular_file(older.json));
    REQUIRE(std::filesystem::is_regular_file(latest.nav));
    REQUIRE(std::filesystem::is_regular_file(latest.json));
    REQUIRE(std::filesystem::is_regular_file(history));
    REQUIRE(std::filesystem::is_regular_file(
        root / "sessionSummaries" / "nav_retention_index.json"));
    std::filesystem::remove_all(root);
}

TEST_CASE("failed production history write cannot register or delete a nav") {
    const auto root = temporaryRoot("nav-retention-history-write-failure");
    const auto sessions = root / "sessions";
    const auto history = smp::makeSeparatedAutomaticSessionSummaryPath(
        sessions, std::chrono::system_clock::time_point(
                      std::chrono::seconds(1'787'491'600)));
    std::filesystem::remove_all(history.parent_path());
    writeText(history.parent_path(), "blocks the history directory");

    smp::AutomaticSessionState automaticSession;
    smp::AnalysisResult analysis;
    analysis.activeDurationSeconds = 60.0;
    REQUIRE(automaticSession.addFinalizedGame(1, analysis));
    bool writeFailed = false;
    try {
        smp::writeSeparatedAutomaticSessionHistory(history, automaticSession);
    } catch (...) {
        writeFailed = true;
    }
    REQUIRE(writeFailed);

    const auto game = writePersistedGame(sessions, "retained", 1000);
    const auto registration =
        smp::recordFinalizedAutomaticNavAndApplyRetention(
            sessions, game.nav, game.json, history,
            {smp::NavRetentionMode::KeepLastGames, 1});
    REQUIRE(!registration.registrationPersisted);
    REQUIRE(std::filesystem::is_regular_file(game.nav));
    REQUIRE(std::filesystem::is_regular_file(game.json));
    std::filesystem::remove_all(root);
}
