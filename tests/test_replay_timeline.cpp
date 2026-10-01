#include "analysis/replay_timeline.h"
#include "test_framework.h"
#include <limits>
#include <random>

namespace {
void requireOrdered(const std::vector<smp::detail::TimelineAnchor>& anchors, std::int64_t first, std::int64_t last) {
    auto previous = smp::detail::replayFrameToActiveMs(first, anchors);
    REQUIRE(std::isfinite(previous));
    for (auto frame = first + 1; frame <= last; ++frame) {
        const auto current = smp::detail::replayFrameToActiveMs(frame, anchors);
        REQUIRE(std::isfinite(current));
        REQUIRE(previous <= current);
        previous = current;
    }
}
} // namespace

TEST_CASE("replay timeline shallow anchor regression is monotonic") {
    requireOrdered({{0, 0.0}, {100, 100.0}}, 0, 100);
}

TEST_CASE("replay timeline respects normal anchors and interpolation") {
    const std::vector<smp::detail::TimelineAnchor> anchors{{100, 1000}, {200, 5200}};
    REQUIRE_NEAR(smp::detail::replayFrameToActiveMs(150, anchors), 3100, 1e-9);
    REQUIRE_NEAR(smp::detail::replayFrameToActiveMs(100, anchors), 1000, 1e-9);
    REQUIRE_NEAR(smp::detail::replayFrameToActiveMs(200, anchors), 5200, 1e-9);
    requireOrdered(anchors, 0, 300);
}

TEST_CASE("replay timeline steep and shallow segments meet their anchors") {
    for (double slope : {0.001, 1.0, 4.99, 5.0, 42.0, 80.0, 80.01, 500.0}) {
        const std::vector<smp::detail::TimelineAnchor> anchors{
            {100, 1000}, {200, 1000 + 100 * slope}, {300, 5200 + 100 * slope}};
        requireOrdered(anchors, 0, 400);
        REQUIRE_NEAR(smp::detail::replayFrameToActiveMs(199, anchors), 1000 + 99 * slope, 1e-8);
        REQUIRE_NEAR(smp::detail::replayFrameToActiveMs(200, anchors), 1000 + 100 * slope, 1e-8);
        REQUIRE_NEAR(smp::detail::replayFrameToActiveMs(201, anchors), 1042 + 100 * slope, 1e-8);
        const double bounded = std::clamp(slope, 5.0, 80.0);
        REQUIRE_NEAR(smp::detail::replayFrameToActiveMs(99, anchors), 1000 - bounded, 1e-8);
        REQUIRE_NEAR(smp::detail::replayFrameToActiveMs(301, anchors), 5242 + 100 * slope, 1e-8);
        const std::vector<smp::detail::TimelineAnchor> pair{anchors[0], anchors[1]};
        REQUIRE_NEAR(smp::detail::replayFrameToActiveMs(201, pair), 1000 + 100 * slope + bounded, 1e-8);
        requireOrdered(pair, 0, 300);
    }
}

TEST_CASE("saved session army command reversal remains ordered across real anchors") {
    // Recovered from 2026-08-23_185246.nav and its matching replay. NAV stores
    // microsecond precision, explaining sub-microsecond differences from JSON.
    const std::vector<smp::detail::TimelineAnchor> anchors{
        {17724, 744709.898}, {17737, 758638.949}, {18060, 758832.029}, {18063, 758982.950}, {18361, 771479.925}};
    requireOrdered(anchors, 17700, 18400);
    const double before = smp::detail::replayFrameToActiveMs(18051, anchors);
    const double after = smp::detail::replayFrameToActiveMs(18079, anchors);
    REQUIRE_NEAR(before, 758826.649061920, 1e-6);
    REQUIRE_NEAR(after, 759653.928523490, 1e-6);
    REQUIRE(before <= after);
}

TEST_CASE("replay timeline rejects duplicate contradictory and nonfinite anchors") {
    std::vector<smp::detail::TimelineAnchor> anchors;
    REQUIRE(!smp::detail::appendTimelineAnchor(anchors, {0, NAN}));
    REQUIRE(smp::detail::appendTimelineAnchor(anchors, {100, 1000}));
    REQUIRE(!smp::detail::appendTimelineAnchor(anchors, {100, 1001}));
    REQUIRE(!smp::detail::appendTimelineAnchor(anchors, {101, 1000}));
    REQUIRE(!smp::detail::appendTimelineAnchor(anchors, {99, 1100}));
    REQUIRE(!smp::detail::appendTimelineAnchor(anchors, {101, 999}));
    REQUIRE(!smp::detail::appendTimelineAnchor(anchors, {101, INFINITY}));
    REQUIRE(smp::detail::appendTimelineAnchor(anchors, {101, std::nextafter(1000.0, 2000.0)}));
    REQUIRE(smp::detail::appendTimelineAnchor(anchors, {102, 2000}));
    requireOrdered(anchors, 0, 200);
}

TEST_CASE("replay timeline nominal fallbacks and int64 extremes stay ordered") {
    requireOrdered({}, -10, 100);
    requireOrdered({{100, 1000}}, -10, 200);
    REQUIRE_NEAR(smp::detail::replayFrameToActiveMs(101, {{100, 1000}}), 1042, 1e-9);
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    const std::vector<smp::detail::TimelineAnchor> anchors{{low, 0}, {high, 1000}};
    double previous = -INFINITY;
    for (const auto frame : {low, low + 1, std::int64_t{-1}, std::int64_t{0}, high - 1, high}) {
        const double mapped = smp::detail::replayFrameToActiveMs(frame, anchors);
        REQUIRE(std::isfinite(mapped));
        REQUIRE(previous <= mapped);
        previous = mapped;
    }
    REQUIRE(smp::detail::replayFrameToActiveMs(low, {{high, 1000}}) < 1000);
    REQUIRE(smp::detail::replayFrameToActiveMs(high, {{low, 1000}}) > 1000);
    REQUIRE_NEAR(smp::detail::replayFrameToActiveMs(high - 1, {{high - 2, 1000}, {high, 1084}}), 1042, 1e-9);
}

TEST_CASE("replay timeline randomized noisy anchor invariant") {
    std::mt19937 random(555);
    for (int trial = 0; trial < 100; ++trial) {
        std::vector<smp::detail::TimelineAnchor> anchors;
        for (int index = 0; index < 30; ++index) {
            const auto frame = index * 10 + static_cast<int>(random() % 21) - 10;
            const double active = index * 100.0 + static_cast<int>(random() % 1000) - 500;
            smp::detail::appendTimelineAnchor(anchors, {frame, active});
        }
        requireOrdered(anchors, -100, 500);
        // The transform is stateless: querying in reverse gives the same order.
        for (int frame = 500; frame > -100; --frame)
            REQUIRE(smp::detail::replayFrameToActiveMs(frame - 1, anchors) <=
                    smp::detail::replayFrameToActiveMs(frame, anchors));
    }
}
