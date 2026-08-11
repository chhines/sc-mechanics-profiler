#include "test_framework.h"

#include "cli/replay_readiness.h"

#include <cstddef>
#include <string>

TEST_CASE("replay readiness retries a parser failure and later succeeds") {
    const smp::ReplayMetadata observed{true, 100, 200};
    std::size_t parseAttempts = 0;
    std::size_t waits = 0;
    smp::ReplayReadinessHooks hooks;
    hooks.readMetadata = [&]() { return observed; };
    hooks.readable = []() { return true; };
    hooks.parse = [&]() {
        ++parseAttempts;
        smp::ReplayExtractionResult result;
        result.parser = smp::bundledReplayParserDiagnostic;
        result.available = parseAttempts >= 2;
        if (!result.available)
            result.unavailableReason = "temporary partial replay";
        return result;
    };
    hooks.wait = [&]() { ++waits; };

    const auto result = smp::waitForReplayReadiness(observed, hooks, 3, 2);
    REQUIRE(result.available);
    REQUIRE(parseAttempts == 2);
    REQUIRE(waits == 2);
}

TEST_CASE("replay readiness stops at its bounded deadline when parsing never succeeds") {
    const smp::ReplayMetadata observed{true, 100, 200};
    std::size_t parseAttempts = 0;
    std::size_t waits = 0;
    smp::ReplayReadinessHooks hooks;
    hooks.readMetadata = [&]() { return observed; };
    hooks.readable = []() { return true; };
    hooks.parse = [&]() {
        ++parseAttempts;
        smp::ReplayExtractionResult result;
        result.parser = smp::bundledReplayParserDiagnostic;
        result.unavailableReason = "still incomplete";
        return result;
    };
    hooks.wait = [&]() { ++waits; };

    const auto result = smp::waitForReplayReadiness(observed, hooks, 4, 2);
    REQUIRE(!result.available);
    REQUIRE(parseAttempts == 3);
    REQUIRE(waits == 3);
    REQUIRE(result.unavailableReason.find("deadline") != std::string::npos);
    REQUIRE(result.unavailableReason.find("still incomplete") != std::string::npos);
}

TEST_CASE("bundled replay parser diagnostic names screp v1.13.3") {
    REQUIRE(std::string(smp::bundledReplayParserDiagnostic) == "screp-v1.13.3");
}
