#include "test_framework.h"

#include "storage/session.h"

#include <chrono>
#include <filesystem>

TEST_CASE("session summary and CSV contain the required stable fields") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("scmechanics-test-" + std::to_string(nonce));
    std::filesystem::create_directories(directory);
    scm::AnalysisResult result;
    result.activeDurationSeconds = 60.0;
    result.rawApm = 200.0;
    result.effectiveApm = 150.0;
    result.droppedEventCount = 2;
    scm::writeSessionSummary(directory, result, "test-session");
    REQUIRE(std::filesystem::exists(directory / "summary.json"));
    REQUIRE(std::filesystem::exists(directory / "metrics.csv"));
    const auto summary = scm::json::parseFile(directory / "summary.json");
    REQUIRE(summary["schema_version"].asInt() == 1);
    REQUIRE(summary["analysis_version"].asString() == "0.1.0");
    REQUIRE(summary["session"]["id"].asString() == "test-session");
    REQUIRE(summary["session"]["dropped_event_count"].asInt() == 2);
    REQUIRE(summary["camera_navigation"]["edge_scroll"].isObject());
    REQUIRE(summary["macro"]["worker"].isObject());
    REQUIRE(summary["pace"]["rolling_eapm_10s"].isArray());
    REQUIRE(summary["pac"]["observations"].isArray());
    REQUIRE(summary["box_selection"]["observations"].isArray());
    std::filesystem::remove_all(directory);
}

TEST_CASE("storage writer drains raw and logical queues before shutdown") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("scmechanics-writer-test-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    std::filesystem::path directory;
    {
        scm::SessionWriter writer(root, 1000, true, 10);
        directory = writer.directory();
        scm::RawInputEvent raw{};
        raw.sequence = 1;
        raw.timestampTicks = 100;
        raw.type = scm::RawEventType::KeyDown;
        REQUIRE(writer.submitRaw(raw));
        scm::LogicalEvent logical{};
        logical.timestampTicks = 100;
        logical.sourceSequence = 1;
        logical.type = scm::LogicalEventType::KeyAction;
        REQUIRE(writer.submitLogical(logical));
        writer.stop();
        REQUIRE(writer.droppedEvents() == 0);
    }
    REQUIRE(std::filesystem::file_size(directory / "events.bin") > sizeof(scm::RawInputEvent));
    REQUIRE(std::filesystem::file_size(directory / "logical_events.bin") > sizeof(scm::LogicalEvent));
    std::filesystem::remove_all(root);
}
