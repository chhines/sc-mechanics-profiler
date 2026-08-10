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

    void send(std::uint64_t ms, smp::RawEventType type, std::uint16_t key = 0, int x = 900, int y = 500,
              std::uint16_t scanCode = 0, std::int16_t wheelDelta = 0) {
        smp::RawInputEvent event{};
        event.sequence = sequence++;
        event.timestampTicks = ms;
        event.type = type;
        event.virtualKey = key;
        event.scanCode = scanCode;
        event.cursorX = x;
        event.cursorY = y;
        event.wheelDelta = wheelDelta;
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

std::size_t mechanicalCount(const smp::AnalysisResult& result, smp::MechanicalInputType type) {
    return static_cast<std::size_t>(std::count_if(result.mechanicalEvents.begin(), result.mechanicalEvents.end(),
                                                   [type](const auto& event) { return event.type == type; }));
}

} // namespace

TEST_CASE("single control-group selection is retained without a camera jump") {
    Replay replay;
    replay.start();
    replay.key(10, 20, '5');
    const auto& result = replay.finish(50);
    REQUIRE(result.navigationEvents.empty());
    REQUIRE(result.mechanicalEvents.size() == 1);
    REQUIRE(result.mechanicalEvents[0].type == smp::MechanicalInputType::ControlGroupSelect);
    REQUIRE(result.mechanicalEvents[0].value == 5);
}

TEST_CASE("control-group double taps transition once and repeated same-group taps recenter") {
    Replay replay;
    replay.start();
    replay.key(0, 40, '1');
    replay.key(150, 190, '1');
    REQUIRE(navigationCount(replay.analyzer->result(), smp::CameraNavigationType::ControlGroupJump) == 1);
    REQUIRE(mechanicalCount(replay.analyzer->result(), smp::MechanicalInputType::ControlGroupSelect) == 2);
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
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::ControlGroupSelect) == 1);
}

TEST_CASE("control-group assignments never count as camera navigation") {
    Replay replay;
    replay.start();
    replay.send(10, smp::RawEventType::KeyDown, VK_CONTROL);
    replay.key(20, 40, '2');
    replay.send(50, smp::RawEventType::KeyUp, VK_CONTROL);
    const auto& result = replay.finish(100);
    REQUIRE(result.navigationEvents.empty());
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::ControlGroupAssign) == 1);
    const auto assignment = std::find_if(result.mechanicalEvents.begin(), result.mechanicalEvents.end(),
                                         [](const auto& event) {
                                             return event.type == smp::MechanicalInputType::ControlGroupAssign;
                                         });
    REQUIRE(assignment != result.mechanicalEvents.end());
    REQUIRE(assignment->value == 2);
    REQUIRE((assignment->modifiers & smp::ModifierCtrl) != 0);
    REQUIRE(std::none_of(result.mechanicalEvents.begin(), result.mechanicalEvents.end(), [](const auto& event) {
        return event.type == smp::MechanicalInputType::KeyPress && event.virtualKey == '2';
    }));
}

TEST_CASE("macro-like key sequence preserves every accepted mechanical action") {
    Replay replay;
    replay.start();
    replay.key(10, 20, '5');
    replay.key(30, 40, 'D');
    replay.key(50, 60, 'D');
    replay.key(70, 80, 'D');
    replay.key(90, 100, '1');
    const auto& result = replay.finish(120);
    REQUIRE(result.navigationEvents.empty());
    REQUIRE(result.mechanicalEvents.size() == 5);
    REQUIRE(result.mechanicalEvents[0].type == smp::MechanicalInputType::ControlGroupSelect);
    REQUIRE(result.mechanicalEvents[0].value == 5);
    REQUIRE(result.mechanicalEvents[1].type == smp::MechanicalInputType::KeyPress);
    REQUIRE(result.mechanicalEvents[1].virtualKey == 'D');
    REQUIRE(result.mechanicalEvents[2].virtualKey == 'D');
    REQUIRE(result.mechanicalEvents[3].virtualKey == 'D');
    REQUIRE(result.mechanicalEvents[4].type == smp::MechanicalInputType::ControlGroupSelect);
    REQUIRE(result.mechanicalEvents[4].value == 1);
}

TEST_CASE("ordinary key autorepeat emits one mechanical key press") {
    Replay replay;
    replay.start();
    replay.send(10, smp::RawEventType::KeyDown, 'D', 900, 500, 0x20);
    replay.send(20, smp::RawEventType::KeyDown, 'D', 900, 500, 0x20);
    replay.send(30, smp::RawEventType::KeyDown, 'D', 900, 500, 0x20);
    replay.send(40, smp::RawEventType::KeyUp, 'D', 900, 500, 0x20);
    const auto& result = replay.finish(50);
    REQUIRE(result.mechanicalEvents.size() == 1);
    REQUIRE(result.mechanicalEvents[0].type == smp::MechanicalInputType::KeyPress);
    REQUIRE(result.mechanicalEvents[0].virtualKey == 'D');
    REQUIRE(result.mechanicalEvents[0].scanCode == 0x20);
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
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::LocationRecall) == 3);
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::LocationAssign) == 1);
    const auto assignment = std::find_if(result.mechanicalEvents.begin(), result.mechanicalEvents.end(),
                                         [](const auto& event) {
                                             return event.type == smp::MechanicalInputType::LocationAssign;
                                         });
    REQUIRE(assignment != result.mechanicalEvents.end());
    REQUIRE(assignment->value == 2);
    REQUIRE((assignment->modifiers & smp::ModifierShift) != 0);
    REQUIRE(std::none_of(result.mechanicalEvents.begin(), result.mechanicalEvents.end(), [](const auto& event) {
        return event.type == smp::MechanicalInputType::KeyPress && event.virtualKey == VK_F2;
    }));
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
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::MouseLeftDown) == 2);
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::MouseLeftUp) == 1);
    REQUIRE(replay.analyzer->cameraContext().type == smp::CameraContextType::Manual);
}

TEST_CASE("mouse buttons and wheel retain discrete actions and coordinates") {
    Replay replay;
    replay.start();
    replay.send(10, smp::RawEventType::MouseLeftDown, 0, 600, 700);
    replay.send(20, smp::RawEventType::MouseLeftUp, 0, 601, 701);
    replay.send(30, smp::RawEventType::MouseRightDown, 0, 602, 702);
    replay.send(40, smp::RawEventType::MouseRightUp, 0, 603, 703);
    replay.send(50, smp::RawEventType::MouseMiddleDown, 0, 604, 704);
    replay.send(60, smp::RawEventType::MouseMiddleUp, 0, 605, 705);
    replay.send(70, smp::RawEventType::MouseWheel, 0, 606, 706, 0, -120);
    const auto& result = replay.finish(80);
    REQUIRE(result.mechanicalEvents.size() == 7);
    REQUIRE(result.mechanicalEvents[0].type == smp::MechanicalInputType::MouseLeftDown);
    REQUIRE(result.mechanicalEvents[0].cursorX == 600);
    REQUIRE(result.mechanicalEvents[0].cursorY == 700);
    REQUIRE(result.mechanicalEvents[5].type == smp::MechanicalInputType::MouseMiddleUp);
    REQUIRE(result.mechanicalEvents[6].type == smp::MechanicalInputType::MouseWheel);
    REQUIRE(result.mechanicalEvents[6].value == -120);
    REQUIRE(result.mechanicalEvents[6].cursorX == 606);
    REQUIRE(result.mechanicalEvents[6].cursorY == 706);
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
    REQUIRE(result.mechanicalEvents.empty());
}

TEST_CASE("high-frequency mouse movement creates no mechanical records") {
    Replay replay;
    replay.start();
    for (std::uint64_t tick = 1; tick <= 10'000; ++tick)
        replay.send(tick, smp::RawEventType::MouseMove, 0, 900, 500);
    const auto& result = replay.finish(10'001);
    REQUIRE(result.mechanicalEvents.empty());
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
    event.timestampTicks = 6000;
    event.type = smp::RawEventType::KeyDown;
    event.virtualKey = 'X';
    analyzer.process(event);
    event.timestampTicks = 6010;
    event.type = smp::RawEventType::KeyUp;
    analyzer.process(event);
    event.timestampTicks = 6500;
    event.type = smp::RawEventType::ForegroundGained;
    analyzer.process(event);

    event.timestampTicks = 6550;
    event.type = smp::RawEventType::KeyDown;
    event.virtualKey = 'D';
    analyzer.process(event);
    event.timestampTicks = 6560;
    event.type = smp::RawEventType::KeyUp;
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
    REQUIRE(analyzer.result().mechanicalEvents.size() == 3);
    REQUIRE(analyzer.result().mechanicalEvents[0].timestampTicks == 5100);
    REQUIRE_NEAR(analyzer.result().mechanicalEvents[0].activeMs, 100.0, 0.001);
    REQUIRE(analyzer.result().mechanicalEvents[1].type == smp::MechanicalInputType::KeyPress);
    REQUIRE(analyzer.result().mechanicalEvents[1].virtualKey == 'D');
    REQUIRE(analyzer.result().mechanicalEvents[1].timestampTicks == 6550);
    REQUIRE_NEAR(analyzer.result().mechanicalEvents[1].activeMs, 550.0, 0.001);
    REQUIRE(analyzer.result().mechanicalEvents[2].timestampTicks == 6600);
    REQUIRE_NEAR(analyzer.result().mechanicalEvents[2].activeMs, 600.0, 0.001);
    REQUIRE_NEAR(analyzer.result().activeDurationSeconds, 1.0, 0.001);
    REQUIRE_NEAR(analyzer.result().pausedDurationSeconds, 1.0, 0.001);
}
