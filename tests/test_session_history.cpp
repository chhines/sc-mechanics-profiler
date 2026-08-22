#include "test_framework.h"

#include "app/session_trend_data.h"
#include "cli/automatic_session_stats.h"
#include "cli/session_summary_paths.h"
#include "util/json.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

std::filesystem::path temporaryRoot() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("starcraft-mechanics-profiler-session-history-" +
                       std::to_string(nonce));
    std::filesystem::create_directories(root);
    return root;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

smp::ProductMacroCycleAnalysis timedCycles(
    smp::MacroProductType type,
    std::initializer_list<std::pair<double, double>> timesSeconds) {
    std::vector<smp::MacroCycle> cycles;
    for (const auto& [startSeconds, endSeconds] : timesSeconds) {
        smp::MacroCycle cycle;
        cycle.productType = type;
        cycle.startActiveMs = startSeconds * 1000.0;
        cycle.endActiveMs = endSeconds * 1000.0;
        cycle.durationMs = (endSeconds - startSeconds) * 1000.0;
        cycles.push_back(cycle);
    }
    return smp::summarizeProductMacroCycles(type, std::move(cycles), {});
}

smp::ProductionAnalysis production(
    smp::ProductMacroCycleAnalysis worker,
    smp::ProductMacroCycleAnalysis army) {
    smp::ProductionAnalysis result;
    result.visitsAvailable = true;
    result.workerMacroCycles = std::move(worker);
    result.armyMacroCycles = std::move(army);
    return result;
}

} // namespace

TEST_CASE("automatic session history persists JSON without touching readable text") {
    const auto root = temporaryRoot();
    const auto data = root / "sessionSummaries" /
                      "2026-08-17_210000_session.json";
    auto text = data;
    text.replace_extension(".txt");
    std::filesystem::create_directories(text.parent_path());
    {
        std::ofstream sentinel(text, std::ios::binary | std::ios::trunc);
        sentinel << "pre-existing readable export";
    }

    smp::AnalysisResult analysis;
    analysis.activeDurationSeconds = 60.0;
    analysis.navigationEvents.push_back(
        {1000, 1000.0, smp::CameraNavigationType::ControlGroupJump, 1});

    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, analysis));
    smp::writeSeparatedAutomaticSessionHistory(data, session);

    REQUIRE(std::filesystem::is_regular_file(data));
    REQUIRE(std::filesystem::is_regular_file(text));
    REQUIRE(readText(text) == "pre-existing readable export");

    const auto encoded = smp::json::parseFile(data);
    REQUIRE(encoded["schema_version"].asInt() == 2);
    REQUIRE(encoded["session_id"].asString() == "2026-08-17_210000");
    REQUIRE(encoded["overall"]["games"].asInt() == 1);
    REQUIRE(encoded["overall"]["navigation"]["total_transitions"].asInt() == 1);
    REQUIRE(encoded["overall"]["navigation"]["transitions_per_minute"].asNumber() == 1.0);
    REQUIRE(encoded["games"].asArray().size() == 1);
    REQUIRE(!encoded["games"].asArray().front()["matchup"].asString().empty());
    REQUIRE(encoded["matchups"].isObject());

    std::filesystem::remove_all(root);
}

TEST_CASE("automatic session history round-trips exact macro gap observations") {
    const auto root = temporaryRoot();
    const auto data = root / "sessionSummaries" /
                      "2026-08-17_220000_session.json";
    smp::AnalysisResult analysis;
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(
        1, analysis,
        production(timedCycles(smp::MacroProductType::Worker,
                               {{0.0, 4.0}, {12.0, 15.0}, {40.0, 43.0}}),
                   timedCycles(smp::MacroProductType::Army,
                               {{0.0, 3.0}}))));
    smp::writeSeparatedAutomaticSessionHistory(data, session);

    REQUIRE(session.addFinalizedGame(
        2, analysis,
        production(timedCycles(smp::MacroProductType::Worker,
                               {{0.0, 12.0}, {10.0, 13.0}}),
                   timedCycles(smp::MacroProductType::Army,
                               {{0.0, 2.0}, {12.0, 14.0}}))));
    smp::writeSeparatedAutomaticSessionHistory(data, session);

    const auto encoded = smp::json::parseFile(data);
    REQUIRE(encoded["schema_version"].asInt() == 2);
    const auto& games = encoded["games"].asArray();
    REQUIRE(games.size() == 2);
    const auto& firstWorkerGaps =
        games[0]["stats"]["worker_macro"]["gap_durations_ms"].asArray();
    REQUIRE(firstWorkerGaps.size() == 2);
    REQUIRE_NEAR(firstWorkerGaps[0].asNumber(), 8000.0, 0.001);
    REQUIRE_NEAR(firstWorkerGaps[1].asNumber(), 25000.0, 0.001);
    REQUIRE(games[0]["stats"]["army_macro"]["gap_durations_ms"]
                .asArray()
                .empty());
    REQUIRE(games[0]["stats"]["army_macro"]["median_gap_ms"].isNull());
    REQUIRE(games[0]["stats"]["army_macro"]["p90_gap_ms"].isNull());
    const auto& secondWorkerGaps =
        games[1]["stats"]["worker_macro"]["gap_durations_ms"].asArray();
    REQUIRE(secondWorkerGaps.size() == 1);
    REQUIRE_NEAR(secondWorkerGaps[0].asNumber(), 0.0, 0.001);

    const auto& worker = encoded["overall"]["worker_macro"];
    REQUIRE(worker["gap_durations_ms"].asArray().size() == 3);
    REQUIRE_NEAR(worker["median_gap_ms"].asNumber(), 8000.0, 0.001);
    REQUIRE_NEAR(worker["p90_gap_ms"].asNumber(), 21600.0, 0.001);
    const auto& army = encoded["overall"]["army_macro"];
    REQUIRE(army["gap_durations_ms"].asArray().size() == 1);
    REQUIRE_NEAR(army["median_gap_ms"].asNumber(), 10000.0, 0.001);
    REQUIRE_NEAR(army["p90_gap_ms"].asNumber(), 10000.0, 0.001);

    const auto trendValues =
        smp::decodeSessionMacroGapTrendValues(encoded["overall"]);
    REQUIRE_NEAR(*trendValues.workerMedianMs, 8000.0, 0.001);
    REQUIRE_NEAR(*trendValues.workerP90Ms, 21600.0, 0.001);
    REQUIRE_NEAR(*trendValues.armyMedianMs, 10000.0, 0.001);
    REQUIRE_NEAR(*trendValues.armyP90Ms, 10000.0, 0.001);

    REQUIRE(encoded["matchups"].asObject().size() == 1);
    const auto& matchupStats =
        encoded["matchups"].asObject().begin()->second;
    REQUIRE_NEAR(matchupStats["worker_macro"]["median_gap_ms"].asNumber(),
                 8000.0, 0.001);
    REQUIRE_NEAR(matchupStats["worker_macro"]["p90_gap_ms"].asNumber(),
                 21600.0, 0.001);

    std::filesystem::remove_all(root);
}

TEST_CASE("legacy session JSON leaves macro gap trend values unavailable") {
    smp::json::Value legacy(smp::json::Value::Object{});
    legacy["worker_macro"] = smp::json::Value::Object{
        {"average_duration_ms", 1000.0},
    };
    legacy["army_macro"] = smp::json::Value::Object{
        {"average_duration_ms", 1500.0},
    };

    const auto decoded = smp::decodeSessionMacroGapTrendValues(legacy);
    REQUIRE(!decoded.workerMedianMs.has_value());
    REQUIRE(!decoded.workerP90Ms.has_value());
    REQUIRE(!decoded.armyMedianMs.has_value());
    REQUIRE(!decoded.armyP90Ms.has_value());
}
