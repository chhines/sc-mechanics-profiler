#include "test_framework.h"

#include "cli/automatic_session_stats.h"
#include "cli/session_summary_paths.h"
#include "util/json.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    REQUIRE(encoded["schema_version"].asInt() == 1);
    REQUIRE(encoded["session_id"].asString() == "2026-08-17_210000");
    REQUIRE(encoded["overall"]["games"].asInt() == 1);
    REQUIRE(encoded["overall"]["navigation"]["total_transitions"].asInt() == 1);
    REQUIRE(encoded["overall"]["navigation"]["transitions_per_minute"].asNumber() == 1.0);
    REQUIRE(encoded["games"].asArray().size() == 1);
    REQUIRE(!encoded["games"].asArray().front()["matchup"].asString().empty());
    REQUIRE(encoded["matchups"].isObject());

    std::filesystem::remove_all(root);
}
