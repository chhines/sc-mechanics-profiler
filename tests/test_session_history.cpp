#include "test_framework.h"

#include "cli/automatic_session_files.h"
#include "cli/automatic_session_stats.h"
#include "util/json.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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

} // namespace

TEST_CASE("automatic session summary writes machine-readable history beside text") {
    const auto root = temporaryRoot();
    const auto summary = root / "sessions" / "2026-08-17_210000_session.txt";

    smp::AnalysisResult analysis;
    analysis.activeDurationSeconds = 60.0;
    analysis.navigationEvents.push_back(
        {1000, 1000.0, smp::CameraNavigationType::ControlGroupJump, 1});

    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, analysis));
    smp::writeAutomaticSessionSummary(summary, session);

    auto data = summary;
    data.replace_extension(".json");
    REQUIRE(std::filesystem::is_regular_file(summary));
    REQUIRE(std::filesystem::is_regular_file(data));

    const auto encoded = smp::json::parseFile(data);
    REQUIRE(encoded["schema_version"].asInt() == 1);
    REQUIRE(encoded["session_id"].asString() == "2026-08-17_210000");
    REQUIRE(encoded["overall"]["games"].asInt() == 1);
    REQUIRE(encoded["overall"]["navigation"]["total_transitions"].asInt() == 1);
    REQUIRE(encoded["overall"]["navigation"]["transitions_per_minute"].asNumber() == 1.0);
    REQUIRE(encoded["games"].asArray().size() == 1);
    REQUIRE(!encoded["games"].asArray().front()["matchup"].asString().empty());
    REQUIRE(encoded["matchups"].isObject());

    const auto text = readText(summary);
    REQUIRE(text.find("SESSION SUMMARY") != std::string::npos);
    REQUIRE(text.find("MATCHUP BREAKDOWN") != std::string::npos);

    std::filesystem::remove_all(root);
}
