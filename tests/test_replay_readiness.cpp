#include "test_framework.h"

#include "cli/replay_readiness.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace {

struct FakeClock {
    smp::ReplayReadinessHooks::Clock::time_point current{};

    void advance(std::chrono::milliseconds duration) { current += duration; }
};

smp::ReplayReadinessPolicy policy(std::chrono::milliseconds timeout) {
    smp::ReplayReadinessPolicy result;
    result.timeout = timeout;
    result.pollInterval = std::chrono::milliseconds(100);
    result.maximumParserAttempt = std::chrono::milliseconds(1000);
    result.requiredStableObservations = 2;
    result.maximumChecks = 64;
    return result;
}

} // namespace

TEST_CASE("replay readiness retries a parser failure and later succeeds") {
    const smp::ReplayMetadata observed{true, 100, 200};
    std::size_t parseAttempts = 0;
    std::size_t waits = 0;
    FakeClock clock;
    smp::ReplayReadinessHooks hooks;
    hooks.now = [&]() { return clock.current; };
    hooks.readMetadata = [&]() { return observed; };
    hooks.readable = []() { return true; };
    hooks.parse = [&](std::chrono::milliseconds) {
        ++parseAttempts;
        smp::ReplayExtractionResult result;
        result.parser = smp::bundledReplayParserDiagnostic;
        result.available = parseAttempts >= 2;
        if (!result.available)
            result.unavailableReason = "temporary partial replay";
        return result;
    };
    hooks.wait = [&](std::chrono::milliseconds duration) {
        ++waits;
        clock.advance(duration);
    };

    const auto result =
        smp::waitForReplayReadiness(observed, hooks, policy(std::chrono::milliseconds(300)));
    REQUIRE(result.available);
    REQUIRE(parseAttempts == 2);
    REQUIRE(waits == 2);
}

TEST_CASE("replay readiness stops at its bounded deadline when parsing never succeeds") {
    const smp::ReplayMetadata observed{true, 100, 200};
    std::size_t parseAttempts = 0;
    FakeClock clock;
    smp::ReplayReadinessHooks hooks;
    hooks.now = [&]() { return clock.current; };
    hooks.readMetadata = [&]() { return observed; };
    hooks.readable = []() { return true; };
    hooks.parse = [&](std::chrono::milliseconds) {
        ++parseAttempts;
        smp::ReplayExtractionResult result;
        result.parser = smp::bundledReplayParserDiagnostic;
        result.unavailableReason = "still incomplete";
        return result;
    };
    hooks.wait = [&](std::chrono::milliseconds duration) { clock.advance(duration); };

    const auto result =
        smp::waitForReplayReadiness(observed, hooks, policy(std::chrono::milliseconds(400)));
    REQUIRE(!result.available);
    REQUIRE(parseAttempts == 3);
    REQUIRE(clock.current.time_since_epoch() == std::chrono::milliseconds(400));
    REQUIRE(result.unavailableReason.find("deadline") != std::string::npos);
    REQUIRE(result.unavailableReason.find("still incomplete") != std::string::npos);
}

TEST_CASE("replay readiness expires after one parser consumes nearly all remaining time") {
    const smp::ReplayMetadata observed{true, 100, 200};
    FakeClock clock;
    std::size_t parseAttempts = 0;
    smp::ReplayReadinessHooks hooks;
    hooks.now = [&]() { return clock.current; };
    hooks.readMetadata = [&]() { return observed; };
    hooks.readable = []() { return true; };
    hooks.parse = [&](std::chrono::milliseconds timeout) {
        ++parseAttempts;
        REQUIRE(timeout == std::chrono::milliseconds(1000));
        clock.advance(std::chrono::milliseconds(2800));
        smp::ReplayExtractionResult result;
        result.parser = smp::bundledReplayParserDiagnostic;
        result.unavailableReason = "simulated slow parser failure";
        return result;
    };
    hooks.wait = [&](std::chrono::milliseconds duration) { clock.advance(duration); };

    const auto result =
        smp::waitForReplayReadiness(observed, hooks, policy(std::chrono::milliseconds(3000)));
    REQUIRE(!result.available);
    REQUIRE(parseAttempts == 1);
    REQUIRE(clock.current.time_since_epoch() == std::chrono::milliseconds(3000));
}

TEST_CASE("replay parser attempt timeout never exceeds remaining readiness time") {
    const smp::ReplayMetadata observed{true, 100, 200};
    FakeClock clock;
    std::chrono::milliseconds suppliedTimeout{};
    bool firstRead = true;
    smp::ReplayReadinessHooks hooks;
    hooks.now = [&]() { return clock.current; };
    hooks.readMetadata = [&]() {
        if (firstRead) {
            firstRead = false;
            clock.advance(std::chrono::milliseconds(2600));
        }
        return observed;
    };
    hooks.readable = []() { return true; };
    hooks.parse = [&](std::chrono::milliseconds timeout) {
        suppliedTimeout = timeout;
        smp::ReplayExtractionResult result;
        result.available = true;
        return result;
    };
    hooks.wait = [&](std::chrono::milliseconds duration) { clock.advance(duration); };
    auto settings = policy(std::chrono::milliseconds(3000));
    settings.requiredStableObservations = 1;

    const auto result = smp::waitForReplayReadiness(observed, hooks, settings);
    REQUIRE(result.available);
    REQUIRE(suppliedTimeout == std::chrono::milliseconds(400));
}

TEST_CASE("replay metadata change resets stability before another parser attempt") {
    const smp::ReplayMetadata observed{true, 100, 200};
    const smp::ReplayMetadata changed{true, 120, 300};
    const std::vector<smp::ReplayMetadata> sequence{observed, observed, changed, changed};
    FakeClock clock;
    std::size_t readIndex = 0;
    std::size_t parseAttempts = 0;
    std::vector<std::size_t> parseReadCounts;
    smp::ReplayReadinessHooks hooks;
    hooks.now = [&]() { return clock.current; };
    hooks.readMetadata = [&]() {
        const auto index = std::min(readIndex, sequence.size() - 1);
        ++readIndex;
        return sequence[index];
    };
    hooks.readable = []() { return true; };
    hooks.parse = [&](std::chrono::milliseconds) {
        ++parseAttempts;
        parseReadCounts.push_back(readIndex);
        smp::ReplayExtractionResult result;
        result.available = parseAttempts == 2;
        if (!result.available)
            result.unavailableReason = "metadata A was incomplete";
        return result;
    };
    hooks.wait = [&](std::chrono::milliseconds duration) { clock.advance(duration); };

    const auto result =
        smp::waitForReplayReadiness(observed, hooks, policy(std::chrono::milliseconds(1000)));
    REQUIRE(result.available);
    REQUIRE(parseAttempts == 2);
    REQUIRE(parseReadCounts.size() == 2);
    REQUIRE(parseReadCounts[0] == 2);
    REQUIRE(parseReadCounts[1] == 4);
}

TEST_CASE("bundled replay parser diagnostic names screp v1.13.3") {
    REQUIRE(std::string(smp::bundledReplayParserDiagnostic) == "screp-v1.13.3");
}
