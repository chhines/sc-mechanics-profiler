#include "test_framework.h"

#include "analysis/multitasking_density.h"

TEST_CASE("multitasking windows preserve Latest Game five-second semantics") {
    smp::MultitaskingActivityTimestamps activity;
    activity.activeMs[static_cast<std::size_t>(
        smp::MultitaskingMechanicClass::Camera)] = {1000.0, 6000.0};
    activity.activeMs[static_cast<std::size_t>(
        smp::MultitaskingMechanicClass::WorkerMacro)] = {1000.0};
    activity.activeMs[static_cast<std::size_t>(
        smp::MultitaskingMechanicClass::ArmyMacro)] = {1000.0};

    const auto windows =
        smp::summarizeMultitaskingWindows(10000.0, activity);
    REQUIRE(windows.diversity.size() == 2);
    REQUIRE(windows.diversity[0] == 3);
    REQUIRE(windows.diversity[1] == 1);
    REQUIRE(windows.totalDiversityAcrossActiveWindows == 4);
    REQUIRE(windows.activeWindowCount == 2);
    REQUIRE(windows.peakDiversity == 3);
    REQUIRE_NEAR(*windows.averageActiveDiversity(), 2.0, 0.001);
}

TEST_CASE("multitasking empty activity distinguishes no active windows from zero peak") {
    const auto windows = smp::summarizeMultitaskingWindows(
        5000.0, smp::MultitaskingActivityTimestamps{});
    REQUIRE(windows.diversity.size() == 1);
    REQUIRE(windows.activeWindowCount == 0);
    REQUIRE(windows.peakDiversity == 0);
    REQUIRE(!windows.averageActiveDiversity().has_value());
}
