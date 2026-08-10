#include "test_framework.h"

#include "analysis/analyzer.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <windows.h>

namespace {

class Replay {
  public:
    Replay() {
        config.gameArea = {240, 0, 1679, 1079};
        config.viewport = {240, 0, 1679, 787};
        config.minimap = {300, 800, 520, 1040};
        config.commandCard = {1400, 800, 1650, 1040};
        config.edgeMarginPx = 5;
        config.edgeMinimumDwellMs = 20;
    }

    void start() {
        analyzer.emplace(config, 1000);
        send(0, smp::RawEventType::ForegroundGained);
    }

    void send(std::uint64_t ms, smp::RawEventType type, std::uint16_t key = 0, int x = 900, int y = 500) {
        smp::RawInputEvent event{};
        event.sequence = sequence++;
        event.timestampTicks = ms;
        event.type = type;
        event.virtualKey = key;
        event.cursorX = x;
        event.cursorY = y;
        analyzer->process(event);
    }

    void key(std::uint64_t downMs, std::uint64_t upMs, std::uint16_t key) {
        send(downMs, smp::RawEventType::KeyDown, key);
        send(upMs, smp::RawEventType::KeyUp, key);
    }

    const smp::AnalysisResult& finish(std::uint64_t ms) {
        analyzer->finalize(ms, 0);
        return analyzer->result();
    }

    smp::Config config;
    std::optional<smp::Analyzer> analyzer;
    std::uint64_t sequence{1};
};

std::size_t navigationCount(const smp::AnalysisResult& result, smp::CameraNavigationType type) {
    return static_cast<std::size_t>(std::count_if(result.navigationEvents.begin(), result.navigationEvents.end(),
                                                   [type](const auto& event) { return event.type == type; }));
}

std::size_t recenterCount(const smp::AnalysisResult& result, smp::CameraRecenterType type) {
    return static_cast<std::size_t>(std::count_if(result.recenters.begin(), result.recenters.end(),
                                                   [type](const auto& event) { return event.type == type; }));
}

} // namespace

TEST_CASE("control-group double taps transition once and repeated same-group taps recenter") {
    Replay replay;
    replay.start();
    replay.key(0, 40, '1');
    replay.key(150, 190, '1');
    REQUIRE(navigationCount(replay.analyzer->result(), smp::CameraNavigationType::ControlGroupJump) == 1);
    REQUIRE(replay.analyzer->cameraContext().type == smp::CameraContextType::ControlGroup);
    REQUIRE(replay.analyzer->cameraContext().id == 1);

    replay.key(300, 340, '1');
    replay.key(430, 470, '1');
    REQUIRE(navigationCount(replay.analyzer->result(), smp::CameraNavigationType::ControlGroupJump) == 1);
    REQUIRE(recenterCount(replay.analyzer->result(), smp::CameraRecenterType::ControlGroup) == 1);

    replay.key(700, 740, '4');
    replay.key(820, 860, '4');
    const auto& result = replay.finish(1000);
    REQUIRE(navigationCount(result, smp::CameraNavigationType::ControlGroupJump) == 2);
    REQUIRE(result.navigationEvents.back().id == 4);
    REQUIRE(replay.analyzer->cameraContext().id == 4);
}

TEST_CASE("held-key autorepeat cannot create control-group double taps") {
    Replay replay;
    replay.start();
    replay.send(10, smp::RawEventType::KeyDown, '1');
    replay.send(30, smp::RawEventType::KeyDown, '1');
    replay.send(50, smp::RawEventType::KeyDown, '1');
    replay.send(70, smp::RawEventType::KeyUp, '1');
    const auto& result = replay.finish(100);
    REQUIRE(result.navigationEvents.empty());
    REQUIRE(result.recenters.empty());
}

TEST_CASE("control-group assignments never count as camera navigation") {
    Replay replay;
    replay.start();
    replay.send(10, smp::RawEventType::KeyDown, VK_CONTROL);
    replay.key(20, 40, '2');
    replay.send(50, smp::RawEventType::KeyUp, VK_CONTROL);
    const auto& result = replay.finish(100);
    REQUIRE(result.navigationEvents.empty());
}

TEST_CASE("location recalls transition by context and shift assignments are excluded") {
    Replay replay;
    replay.start();
    replay.key(10, 20, VK_F2);
    replay.key(100, 110, VK_F2);
    replay.key(200, 210, VK_F3);
    replay.send(300, smp::RawEventType::KeyDown, VK_SHIFT);
    replay.key(310, 320, VK_F2);
    replay.send(330, smp::RawEventType::KeyUp, VK_SHIFT);
    const auto& result = replay.finish(400);
    REQUIRE(navigationCount(result, smp::CameraNavigationType::LocationHotkey) == 2);
    REQUIRE(recenterCount(result, smp::CameraRecenterType::LocationHotkey) == 1);
    REQUIRE(result.locationRecallCount == 3);
    REQUIRE(result.navigationEvents[0].id == 2);
    REQUIRE(result.navigationEvents[1].id == 3);
    REQUIRE(replay.analyzer->cameraContext().type == smp::CameraContextType::LocationHotkey);
    REQUIRE(replay.analyzer->cameraContext().id == 3);
}

TEST_CASE("minimap mouse-down creates an immediate jump and outside clicks do not") {
    Replay replay;
    replay.start();
    replay.send(100, smp::RawEventType::MouseLeftDown, 0, 350, 900);
    replay.send(110, smp::RawEventType::MouseLeftUp, 0, 350, 900);
    replay.send(200, smp::RawEventType::MouseLeftDown, 0, 900, 500);
    const auto& result = replay.finish(300);
    REQUIRE(navigationCount(result, smp::CameraNavigationType::MinimapJump) == 1);
    REQUIRE(result.navigationEvents[0].timestampTicks == 100);
    REQUIRE(result.navigationEvents[0].cursorX == 350);
    REQUIRE(result.navigationEvents[0].cursorY == 900);
    REQUIRE(replay.analyzer->cameraContext().type == smp::CameraContextType::Manual);
}

TEST_CASE("late screen geometry enables minimap detection without a second focus transition") {
    smp::Config config;
    config.gameArea = {};
    config.viewport = {};
    config.minimap = {};
    smp::Analyzer analyzer(config, 1000);

    smp::RawInputEvent event{};
    event.timestampTicks = 0;
    event.type = smp::RawEventType::ForegroundGained;
    analyzer.process(event);

    event.timestampTicks = 100;
    event.type = smp::RawEventType::MouseLeftDown;
    event.cursorX = 373;
    event.cursorY = 871;
    analyzer.process(event);
    REQUIRE(navigationCount(analyzer.result(), smp::CameraNavigationType::MinimapJump) == 0);

    smp::ScreenRegions regions;
    regions.clientArea = {0, 0, 1919, 1079};
    regions.gameArea = {240, 0, 1679, 1079};
    regions.viewport = regions.gameArea;
    regions.minimap = {254, 783, 541, 1070};
    analyzer.setScreenRegions(regions);

    event.timestampTicks = 200;
    analyzer.process(event);
    REQUIRE(navigationCount(analyzer.result(), smp::CameraNavigationType::MinimapJump) == 1);
    analyzer.finalize(1000, 0);
    REQUIRE_NEAR(analyzer.result().activeDurationSeconds, 1.0, 0.001);
}

TEST_CASE("pillarbox boundaries define edge zones instead of physical monitor edges") {
    const smp::ScreenRect gameArea{240, 0, 1679, 1079};
    REQUIRE(smp::edgeDirectionAt(gameArea, 5, {242, 500}) == smp::EdgeDirection::Left);
    REQUIRE(smp::edgeDirectionAt(gameArea, 5, {1677, 500}) == smp::EdgeDirection::Right);
    REQUIRE(smp::edgeDirectionAt(gameArea, 5, {0, 500}) == smp::EdgeDirection::None);
    REQUIRE(smp::edgeDirectionAt(gameArea, 5, {500, 500}) == smp::EdgeDirection::None);
}

TEST_CASE("one continuous edge dwell is timestamped at the beginning of the scroll episode") {
    Replay replay;
    replay.start();
    replay.send(100, smp::RawEventType::MouseMove, 0, 500, 500);
    replay.send(200, smp::RawEventType::MouseMove, 0, 243, 500);
    replay.send(250, smp::RawEventType::MouseMove, 0, 242, 500);
    replay.send(300, smp::RawEventType::MouseMove, 0, 241, 500);
    replay.send(400, smp::RawEventType::MouseMove, 0, 500, 500);
    const auto& result = replay.finish(500);
    REQUIRE(navigationCount(result, smp::CameraNavigationType::EdgeScroll) == 1);
    const auto& edge = result.navigationEvents[0];
    REQUIRE(edge.edgeDirection == smp::EdgeDirection::Left);
    REQUIRE(edge.timestampTicks == 200);
    REQUIRE_NEAR(edge.activeMs, 200.0, 0.01);
    REQUIRE_NEAR(edge.durationMs, 200.0, 0.01);
    REQUIRE(edge.startCursorX == 243);
    REQUIRE(edge.cursorX == 500);
}

TEST_CASE("foreground pauses are excluded from active duration") {
    Replay replay;
    replay.start();
    replay.send(500, smp::RawEventType::ForegroundLost);
    replay.send(1500, smp::RawEventType::ForegroundGained);
    const auto& result = replay.finish(2000);
    REQUIRE_NEAR(result.activeDurationSeconds, 1.0, 0.001);
    REQUIRE_NEAR(result.pausedDurationSeconds, 1.0, 0.001);
}

TEST_CASE("waiting for first focus and foreground pauses do not shift active event time") {
    smp::Config config;
    config.minimap = {300, 800, 520, 1040};
    smp::Analyzer analyzer(config, 1000);

    smp::RawInputEvent event{};
    event.timestampTicks = 5000; // The recorder may have been waiting before this point.
    event.type = smp::RawEventType::ForegroundGained;
    analyzer.process(event);

    event.timestampTicks = 5100;
    event.type = smp::RawEventType::MouseLeftDown;
    event.cursorX = 350;
    event.cursorY = 900;
    analyzer.process(event);

    event.timestampTicks = 5500;
    event.type = smp::RawEventType::ForegroundLost;
    analyzer.process(event);
    event.timestampTicks = 6500;
    event.type = smp::RawEventType::ForegroundGained;
    analyzer.process(event);

    event.timestampTicks = 6600;
    event.type = smp::RawEventType::MouseLeftDown;
    event.cursorX = 360;
    analyzer.process(event);
    analyzer.finalize(7000, 0);

    REQUIRE(analyzer.result().navigationEvents.size() == 2);
    REQUIRE(analyzer.result().navigationEvents[0].timestampTicks == 5100);
    REQUIRE_NEAR(analyzer.result().navigationEvents[0].activeMs, 100.0, 0.001);
    REQUIRE(analyzer.result().navigationEvents[1].timestampTicks == 6600);
    REQUIRE_NEAR(analyzer.result().navigationEvents[1].activeMs, 600.0, 0.001);
    REQUIRE_NEAR(analyzer.result().activeDurationSeconds, 1.0, 0.001);
    REQUIRE_NEAR(analyzer.result().pausedDurationSeconds, 1.0, 0.001);
}
