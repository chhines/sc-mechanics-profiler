#include "test_framework.h"

#include "cli/commands.h"
#include "storage/session.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>

TEST_CASE("camera-only summary compare and export commands work across sessions") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root =
        std::filesystem::temp_directory_path() / ("starcraft-mechanics-profiler-cli-test-" + std::to_string(nonce));
    const auto sessions = root / "sessions";
    const auto first = sessions / "2026-01-01_000000.nav";
    const auto second = sessions / "2026-01-02_000000.nav";
    std::filesystem::create_directories(sessions);

    smp::AnalysisResult baseline;
    baseline.activeDurationSeconds = 60.0;
    baseline.navigationEvents.push_back(
        {1000, 1000.0, smp::CameraNavigationType::ControlGroupJump, 1, 900, 500, 0.0,
         smp::EdgeDirection::None, 900, 500});
    smp::writeNavSession(first, baseline, first.stem().string(), 1000, 1000);
    smp::AnalysisResult latest = baseline;
    latest.navigationEvents.push_back(
        {2000, 2000.0, smp::CameraNavigationType::MinimapJump, -1, 350, 900, 0.0,
         smp::EdgeDirection::None, 350, 900});
    smp::writeNavSession(second, latest, second.stem().string(), 1000, 2000);

    std::ostringstream captured;
    auto* previousBuffer = std::cout.rdbuf(captured.rdbuf());
    try {
        REQUIRE(smp::runCommand({"summary", "latest"}, root) == 0);
        REQUIRE(smp::runCommand({"compare", "last", "1"}, root) == 0);
        REQUIRE(smp::runCommand({"export", "latest", "--csv"}, root) == 0);
        std::cout.rdbuf(previousBuffer);
    } catch (...) {
        std::cout.rdbuf(previousBuffer);
        std::filesystem::remove_all(root);
        throw;
    }
    REQUIRE(captured.str().find("STARCRAFT MECHANICS PROFILER - CAMERA NAVIGATION") != std::string::npos);
    REQUIRE(captured.str().find("Transitions / min") != std::string::npos);
    REQUIRE(captured.str().find("Edge pans") != std::string::npos);
    REQUIRE(captured.str().find("Edge pan") != std::string::npos);
    REQUIRE(captured.str().find("EDGE PAN") != std::string::npos);
    REQUIRE(captured.str().find("Edge-scroll episodes") == std::string::npos);
    REQUIRE(captured.str().find("Edge scroll") == std::string::npos);
    REQUIRE(captured.str().find("Episodes") == std::string::npos);
    REQUIRE(captured.str().find("PAC") == std::string::npos);
    REQUIRE(captured.str().find("CONTROL GROUPS") == std::string::npos);
    REQUIRE(captured.str().find("LOCATION HOTKEYS") == std::string::npos);
    REQUIRE(captured.str().find("Repeated same-group") == std::string::npos);
    REQUIRE(captured.str().find("Repeated same-location") == std::string::npos);
    REQUIRE(captured.str().find("Median duration") == std::string::npos);
    REQUIRE(captured.str().find("P90 duration") == std::string::npos);
    REQUIRE(std::filesystem::exists(root / "exports" / "Starcraft Mechanics Profiler_2026-01-02_000000.csv"));
    std::filesystem::remove_all(root);
}

TEST_CASE("launching without arguments opens the camera-navigation menu") {
    const auto root = std::filesystem::temp_directory_path() / "starcraft-mechanics-profiler-menu-test";
    std::istringstream input("6\n\n0\n");
    std::ostringstream output;
    auto* previousInput = std::cin.rdbuf(input.rdbuf());
    auto* previousOutput = std::cout.rdbuf(output.rdbuf());
    try {
        REQUIRE(smp::runCommand({}, root) == 0);
        std::cin.rdbuf(previousInput);
        std::cout.rdbuf(previousOutput);
    } catch (...) {
        std::cin.rdbuf(previousInput);
        std::cout.rdbuf(previousOutput);
        throw;
    }
    REQUIRE(output.str().find("Automatic detector: OFF") != std::string::npos);
    REQUIRE(output.str().find("Turn automatic detector on") != std::string::npos);
    REQUIRE(output.str().find("Record camera navigation") == std::string::npos);
    REQUIRE(output.str().find("Record custom games automatically") == std::string::npos);
    REQUIRE(output.str().find("Calibrate minimap") != std::string::npos);
    REQUIRE(output.str().find("Test live detection (debug mode)") != std::string::npos);
    REQUIRE(output.str().find("Capture minimap loading frames (diagnostic)") == std::string::npos);
    REQUIRE(output.str().find("Starcraft Mechanics Profiler.exe\" debug") != std::string::npos);
    REQUIRE(output.str().find("Starcraft Mechanics Profiler.exe\" auto") != std::string::npos);
    REQUIRE(output.str().find("Starcraft Mechanics Profiler.exe\" test-minimap-start") == std::string::npos);
    REQUIRE(output.str().find("--debug-navigation") != std::string::npos);
    REQUIRE(output.str().find("--save-raw") != std::string::npos);
}
