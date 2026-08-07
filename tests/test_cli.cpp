#include "test_framework.h"

#include "cli/commands.h"
#include "storage/session.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>

TEST_CASE("summary compare and export commands work across stored sessions") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("scmechanics-cli-test-" + std::to_string(nonce));
    const auto first = root / "sessions" / "2026-01-01_000000";
    const auto second = root / "sessions" / "2026-01-02_000000";
    std::filesystem::create_directories(first);
    std::filesystem::create_directories(second);

    scm::AnalysisResult baseline;
    baseline.activeDurationSeconds = 60.0;
    baseline.pacFirstAction.median = 200.0;
    baseline.controlGroupSwitch.median = 150.0;
    baseline.commandTarget.median = 100.0;
    scm::writeSessionSummary(first, baseline, first.filename().string());
    scm::AnalysisResult latest = baseline;
    latest.pacFirstAction.median = 180.0;
    scm::writeSessionSummary(second, latest, second.filename().string());

    std::ostringstream captured;
    auto* previousBuffer = std::cout.rdbuf(captured.rdbuf());
    try {
        REQUIRE(scm::runCommand({"summary", "latest"}, root) == 0);
        REQUIRE(scm::runCommand({"compare", "last", "1"}, root) == 0);
        REQUIRE(scm::runCommand({"export", "latest", "--csv"}, root) == 0);
        std::cout.rdbuf(previousBuffer);
    } catch (...) {
        std::cout.rdbuf(previousBuffer);
        std::filesystem::remove_all(root);
        throw;
    }
    REQUIRE(captured.str().find("SCMECHANICS SESSION") != std::string::npos);
    REQUIRE(captured.str().find("PAC latency") != std::string::npos);
    REQUIRE(std::filesystem::exists(root / "exports" / "scmechanics_2026-01-02_000000.csv"));
    std::filesystem::remove_all(root);
}

TEST_CASE("launching without arguments opens a persistent interactive menu") {
    const auto root = std::filesystem::temp_directory_path() / "scmechanics-menu-test";
    std::istringstream input("5\n\n0\n");
    std::ostringstream output;
    auto* previousInput = std::cin.rdbuf(input.rdbuf());
    auto* previousOutput = std::cout.rdbuf(output.rdbuf());
    try {
        REQUIRE(scm::runCommand({}, root) == 0);
        std::cin.rdbuf(previousInput);
        std::cout.rdbuf(previousOutput);
    } catch (...) {
        std::cin.rdbuf(previousInput);
        std::cout.rdbuf(previousOutput);
        throw;
    }
    REQUIRE(output.str().find("Choose an option") != std::string::npos);
    REQUIRE(output.str().find("Show command-line help") != std::string::npos);
}
