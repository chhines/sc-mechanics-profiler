#include "test_framework.h"

#include "capture/collector.h"

TEST_CASE("stable raw input foreground checks never request geometry refresh") {
    int geometryRefreshes = 0;
    for (int packet = 0; packet < 1000; ++packet) {
        const auto decision = smp::collectorForegroundDecision(true, true, false);
        REQUIRE(decision.transition ==
                smp::CollectorForegroundTransition::None);
        if (decision.refreshGeometry)
            ++geometryRefreshes;
    }
    REQUIRE(geometryRefreshes == 0);
}

TEST_CASE("stable foreground timer checks request one geometry refresh per tick") {
    constexpr int timerTicks = 25;
    int geometryRefreshes = 0;
    for (int tick = 0; tick < timerTicks; ++tick) {
        const auto decision = smp::collectorForegroundDecision(true, true, true);
        REQUIRE(decision.transition ==
                smp::CollectorForegroundTransition::None);
        if (decision.refreshGeometry)
            ++geometryRefreshes;
    }
    REQUIRE(geometryRefreshes == timerTicks);
}

TEST_CASE("foreground gain refreshes immediately and foreground loss does not") {
    const auto gained = smp::collectorForegroundDecision(false, true, false);
    REQUIRE(gained.transition == smp::CollectorForegroundTransition::Gained);
    REQUIRE(gained.refreshGeometry);

    const auto lost = smp::collectorForegroundDecision(true, false, true);
    REQUIRE(lost.transition == smp::CollectorForegroundTransition::Lost);
    REQUIRE(!lost.refreshGeometry);

    const auto remainsInactive =
        smp::collectorForegroundDecision(false, false, true);
    REQUIRE(remainsInactive.transition ==
            smp::CollectorForegroundTransition::None);
    REQUIRE(!remainsInactive.refreshGeometry);
}

TEST_CASE("foreground transitions retain their caller observation timestamp") {
    constexpr std::uint64_t observationTimestampTicks = 42'000;
    const auto gained = smp::makeCollectorForegroundTransitionEvent(
        smp::CollectorForegroundTransition::Gained,
        observationTimestampTicks, 640, 480);
    REQUIRE(gained.type == smp::RawEventType::ForegroundGained);
    REQUIRE(gained.timestampTicks == observationTimestampTicks);
    REQUIRE(gained.cursorX == 640);
    REQUIRE(gained.cursorY == 480);

    const auto lost = smp::makeCollectorForegroundTransitionEvent(
        smp::CollectorForegroundTransition::Lost,
        observationTimestampTicks, 641, 481);
    REQUIRE(lost.type == smp::RawEventType::ForegroundLost);
    REQUIRE(lost.timestampTicks == observationTimestampTicks);
}
