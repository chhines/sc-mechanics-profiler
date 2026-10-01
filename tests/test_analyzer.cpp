#include "test_framework.h"

#include "analysis/analyzer.h"
#include "analysis/production_visit.h"
#include "capture/ring_buffer.h"
#include <limits>

#include <algorithm>
#include <cstdint>
#include <cmath>
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

TEST_CASE("edge completion cannot overwrite a newer location recall away from the edge") {
    Replay replay;
    replay.start();
    replay.send(0, smp::RawEventType::MouseMove, 0, 242, 500);
    replay.key(100, 110, VK_F2);
    replay.send(300, smp::RawEventType::MouseMove);
    REQUIRE(replay.analyzer->cameraContext().type == smp::CameraContextType::LocationHotkey);
    replay.key(400, 410, VK_F2);
    const auto& result = replay.finish(500);
    REQUIRE(recenterCount(result, smp::CameraRecenterType::LocationHotkey) == 1);
    REQUIRE(result.navigationEvents[0].timestampTicks == 0);
    REQUIRE_NEAR(result.navigationEvents[0].durationMs, 100.0, 0.01);
}

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

TEST_CASE("intervening group selection prevents a stale control-group double tap") {
    Replay replay;
    replay.config.controlGroupDoubleTapMs = 350;
    replay.start();
    replay.key(0, 10, '1');
    replay.key(100, 110, '2');
    replay.key(200, 210, '1');
    const auto& result = replay.finish(250);
    REQUIRE(navigationCount(result, smp::CameraNavigationType::ControlGroupJump) == 0);
    REQUIRE(recenterCount(result, smp::CameraRecenterType::ControlGroup) == 0);
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::ControlGroupSelect) == 3);
    REQUIRE(replay.analyzer->cameraContext().type == smp::CameraContextType::Unknown);
}

TEST_CASE("interrupted group selection starts a fresh pair on the final two presses") {
    Replay replay;
    replay.config.controlGroupDoubleTapMs = 350;
    replay.start();
    replay.key(0, 10, '1');
    replay.key(100, 110, '2');
    replay.key(200, 210, '1');
    REQUIRE(replay.analyzer->result().navigationEvents.empty());
    replay.key(300, 310, '1');
    const auto& result = replay.finish(350);
    REQUIRE(result.navigationEvents.size() == 1);
    REQUIRE(result.navigationEvents[0].type == smp::CameraNavigationType::ControlGroupJump);
    REQUIRE(result.navigationEvents[0].id == 1);
    REQUIRE(result.navigationEvents[0].timestampTicks == 300);
    REQUIRE(result.recenters.empty());
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::ControlGroupSelect) == 4);
}

TEST_CASE("intervening group can form its own consecutive pair") {
    Replay replay;
    replay.start();
    replay.key(0, 10, '1');
    replay.key(100, 110, '2');
    replay.key(200, 210, '2');
    const auto& result = replay.finish(250);
    REQUIRE(navigationCount(result, smp::CameraNavigationType::ControlGroupJump) == 1);
    REQUIRE(result.navigationEvents[0].id == 2);
    REQUIRE(result.navigationEvents[0].timestampTicks == 200);
    REQUIRE(result.recenters.empty());
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::ControlGroupSelect) == 3);
}

TEST_CASE("several alternating groups cannot reuse an earlier tap") {
    Replay replay;
    replay.config.controlGroupDoubleTapMs = 350;
    replay.start();
    replay.key(0, 10, '1');
    replay.key(100, 110, '2');
    replay.key(200, 210, '3');
    replay.key(300, 310, '1');
    const auto& result = replay.finish(350);
    REQUIRE(result.navigationEvents.empty());
    REQUIRE(result.recenters.empty());
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::ControlGroupSelect) == 4);
}

TEST_CASE("control-group pair timeout starts a fresh pending tap") {
    Replay replay;
    replay.config.controlGroupDoubleTapMs = 150;
    replay.start();
    replay.key(0, 10, '1');
    replay.key(151, 161, '1');
    REQUIRE(replay.analyzer->result().navigationEvents.empty());
    REQUIRE(replay.analyzer->result().recenters.empty());
    replay.key(301, 311, '1'); // The configured maximum interval is inclusive.
    const auto& result = replay.finish(350);
    REQUIRE(navigationCount(result, smp::CameraNavigationType::ControlGroupJump) == 1);
    REQUIRE(result.navigationEvents[0].timestampTicks == 301);
    REQUIRE(result.recenters.empty());
}

TEST_CASE("control-group taps cannot span foreground loss and gain") {
    Replay replay;
    replay.start();
    replay.key(0, 10, '1');
    replay.send(50, smp::RawEventType::ForegroundLost);
    replay.send(100, smp::RawEventType::ForegroundGained);
    replay.key(150, 160, '1');
    const auto& result = replay.finish(200);
    REQUIRE(result.navigationEvents.empty());
    REQUIRE(result.recenters.empty());
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::ControlGroupSelect) == 2);
}

TEST_CASE("a new analyzer session has no pending control-group tap") {
    Replay replay;
    replay.start();
    replay.key(0, 10, '1');
    replay.finish(20);
    // Sessions reconstruct Analyzer; omit focus markers to also check initial tap state.
    replay.analyzer.emplace(replay.config, 1000);
    replay.key(150, 160, '1');
    const auto& result = replay.finish(200);
    REQUIRE(result.navigationEvents.empty());
    REQUIRE(result.recenters.empty());
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::ControlGroupSelect) == 1);
}

TEST_CASE("assignment and add to any group interrupt a pending control-group pair") {
    for (const auto modifier : {VK_CONTROL, VK_SHIFT}) {
        for (const auto group : {'1', '2'}) {
            Replay replay;
            replay.start();
            replay.key(0, 10, '1');
            replay.send(90, smp::RawEventType::KeyDown, modifier);
            replay.key(100, 110, group);
            replay.send(120, smp::RawEventType::KeyUp, modifier);
            replay.key(200, 210, '1');
            REQUIRE(replay.analyzer->result().navigationEvents.empty());
            REQUIRE(replay.analyzer->result().recenters.empty());
            replay.key(300, 310, '1');
            const auto& result = replay.finish(350);
            REQUIRE(navigationCount(result, smp::CameraNavigationType::ControlGroupJump) == 1);
            REQUIRE(result.navigationEvents[0].id == 1);
            REQUIRE(result.navigationEvents[0].timestampTicks == 300);
            REQUIRE(result.recenters.empty());
            REQUIRE(mechanicalCount(result, smp::MechanicalInputType::ControlGroupSelect) == 3);
            REQUIRE(mechanicalCount(result, modifier == VK_CONTROL
                                                ? smp::MechanicalInputType::ControlGroupAssign
                                                : smp::MechanicalInputType::ControlGroupAdd) == 1);
        }
    }
}

TEST_CASE("intervening selection cannot manufacture a recenter in existing group context") {
    Replay replay;
    replay.start();
    replay.key(0, 10, '1');
    replay.key(100, 110, '1');
    replay.key(200, 210, '1');
    replay.key(300, 310, '2');
    replay.key(400, 410, '1');
    REQUIRE(replay.analyzer->result().recenters.empty());
    replay.key(500, 510, '1');
    const auto& result = replay.finish(550);
    REQUIRE(navigationCount(result, smp::CameraNavigationType::ControlGroupJump) == 1);
    REQUIRE(recenterCount(result, smp::CameraRecenterType::ControlGroup) == 1);
    REQUIRE(result.recenters[0].timestampTicks == 500);
    REQUIRE(replay.analyzer->cameraContext().id == 1);
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

TEST_CASE("Shift-number is retained as a control-group add and never a selection") {
    Replay replay;
    replay.start();
    replay.send(10, smp::RawEventType::KeyDown, VK_SHIFT);
    replay.key(20, 40, '2');
    replay.send(50, smp::RawEventType::KeyUp, VK_SHIFT);
    const auto& result = replay.finish(100);
    REQUIRE(result.navigationEvents.empty());
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::ControlGroupAdd) == 1);
    REQUIRE(mechanicalCount(result, smp::MechanicalInputType::ControlGroupSelect) == 0);
    const auto addition = std::find_if(result.mechanicalEvents.begin(), result.mechanicalEvents.end(),
                                       [](const auto& event) {
                                           return event.type == smp::MechanicalInputType::ControlGroupAdd;
                                       });
    REQUIRE(addition != result.mechanicalEvents.end());
    REQUIRE(addition->value == 2);
    REQUIRE((addition->modifiers & smp::ModifierShift) != 0);
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

TEST_CASE("location recall splits continued edge dwell and resolves context before classification") {
    Replay replay;
    replay.start();
    replay.key(0, 1, VK_F2);
    replay.send(10, smp::RawEventType::MouseMove, 0, 242, 500);
    replay.send(100, smp::RawEventType::KeyDown, VK_F2, 242, 500);
    REQUIRE(navigationCount(replay.analyzer->result(), smp::CameraNavigationType::LocationHotkey) == 2);
    REQUIRE(replay.analyzer->result().recenters.empty());
    REQUIRE(replay.analyzer->cameraContext().type == smp::CameraContextType::LocationHotkey);
    replay.send(110, smp::RawEventType::KeyUp, VK_F2, 242, 500);
    replay.send(300, smp::RawEventType::MouseMove);
    REQUIRE(replay.analyzer->cameraContext().type == smp::CameraContextType::Manual);
    const auto& result = replay.finish(400);
    REQUIRE(result.navigationEvents.size() == 4);
    REQUIRE(result.navigationEvents[1].timestampTicks == 10);
    REQUIRE_NEAR(result.navigationEvents[1].durationMs, 90, 0.01);
    REQUIRE(result.navigationEvents[2].type == smp::CameraNavigationType::LocationHotkey);
    REQUIRE(result.navigationEvents[3].timestampTicks == 100);
    REQUIRE_NEAR(result.navigationEvents[3].durationMs, 200, 0.01);
}

TEST_CASE("camera boundaries require independent dwell on both sides") {
    for (const auto before : {10ULL, 100ULL}) {
        for (const auto after : {10ULL, 100ULL}) {
            Replay replay;
            replay.start();
            replay.send(0, smp::RawEventType::MouseMove, 0, 242, 500);
            replay.send(before, smp::RawEventType::KeyDown, VK_F2, 242, 500);
            replay.send(before + after, smp::RawEventType::MouseMove);
            const auto& result = replay.finish(before + after + 1);
            REQUIRE(navigationCount(result, smp::CameraNavigationType::EdgeScroll) ==
                    static_cast<std::size_t>((before >= 20) + (after >= 20)));
            REQUIRE(replay.analyzer->cameraContext().type ==
                    (after >= 20 ? smp::CameraContextType::Manual : smp::CameraContextType::LocationHotkey));
            for (const auto& event : result.navigationEvents) {
                if (event.type == smp::CameraNavigationType::EdgeScroll)
                    REQUIRE(event.timestampTicks >= before || event.timestampTicks + event.durationMs <= before);
            }
        }
    }
}

TEST_CASE("control group camera boundaries split edges before jump or recenter classification") {
    for (const bool existingContext : {false, true}) {
        for (const auto dwell : {10ULL, 100ULL}) {
            Replay replay;
            replay.start();
            if (existingContext) {
                replay.key(0, 1, '1');
                replay.key(10, 11, '1');
            }
            replay.send(100, smp::RawEventType::MouseMove, 0, 242, 500);
            replay.key(101, 102, '1');
            replay.send(100 + dwell, smp::RawEventType::KeyDown, '1', 242, 500);
            REQUIRE(replay.analyzer->cameraContext().type == smp::CameraContextType::ControlGroup);
            REQUIRE(replay.analyzer->cameraContext().id == 1);
            REQUIRE(recenterCount(replay.analyzer->result(), smp::CameraRecenterType::ControlGroup) ==
                    static_cast<std::size_t>(existingContext && dwell < 20));
            replay.send(300, smp::RawEventType::MouseMove);
            const auto& result = replay.finish(400);
            REQUIRE(replay.analyzer->cameraContext().type == smp::CameraContextType::Manual);
            REQUIRE(navigationCount(result, smp::CameraNavigationType::EdgeScroll) ==
                    static_cast<std::size_t>(1 + (dwell >= 20)));
            REQUIRE(result.navigationEvents.back().timestampTicks == 100 + dwell);
            REQUIRE_NEAR(result.navigationEvents.back().durationMs, 200 - dwell, 0.01);
        }
    }
}

TEST_CASE("minimap jump resolves preceding edge at its own timestamp") {
    Replay replay;
    replay.start();
    replay.send(0, smp::RawEventType::MouseMove, 0, 242, 500);
    replay.send(100, smp::RawEventType::MouseLeftDown, 0, 400, 900);
    replay.send(300, smp::RawEventType::MouseMove);
    const auto& result = replay.finish(400);
    REQUIRE(result.navigationEvents.size() == 2);
    REQUIRE(result.navigationEvents[0].type == smp::CameraNavigationType::EdgeScroll);
    REQUIRE_NEAR(result.navigationEvents[0].durationMs, 100, 0.01);
    REQUIRE(result.navigationEvents[1].type == smp::CameraNavigationType::MinimapJump);
    REQUIRE(replay.analyzer->cameraContext().type == smp::CameraContextType::Manual);
}

TEST_CASE("non navigation inputs do not split edge candidates") {
    Replay replay;
    replay.start();
    replay.send(0, smp::RawEventType::MouseMove, 0, 242, 500);
    replay.key(10, 11, '1');
    replay.send(20, smp::RawEventType::KeyDown, VK_CONTROL);
    replay.key(21, 22, '2');
    replay.send(23, smp::RawEventType::KeyUp, VK_CONTROL);
    replay.send(30, smp::RawEventType::KeyDown, VK_SHIFT);
    replay.key(31, 32, '2');
    replay.key(33, 34, VK_F2);
    replay.send(35, smp::RawEventType::KeyUp, VK_SHIFT);
    replay.key(40, 41, 'D');
    replay.send(50, smp::RawEventType::MouseLeftDown);
    replay.send(100, smp::RawEventType::MouseMove);
    const auto& result = replay.finish(200);
    REQUIRE(result.navigationEvents.size() == 1);
    REQUIRE_NEAR(result.navigationEvents[0].durationMs, 100, 0.01);
}

TEST_CASE("edge foreground completion and finalize never bridge inactive time") {
    Replay replay;
    replay.start();
    replay.send(10, smp::RawEventType::MouseMove, 0, 242, 500);
    replay.send(100, smp::RawEventType::ForegroundLost);
    replay.send(200, smp::RawEventType::ForegroundGained, 0, 242, 500);
    replay.send(300, smp::RawEventType::MouseMove, 0, 242, 500);
    const auto& result = replay.finish(400);
    REQUIRE(result.navigationEvents.size() == 2);
    REQUIRE_NEAR(result.navigationEvents[0].durationMs, 90, 0.01);
    REQUIRE(result.navigationEvents[1].timestampTicks == 300);
    REQUIRE_NEAR(result.navigationEvents[1].activeMs, 200, 0.01);
    REQUIRE_NEAR(result.navigationEvents[1].durationMs, 100, 0.01);
}

TEST_CASE("split edge camera access follows recall without duplicating its mechanical evidence") {
    for (const bool recenter : {false, true}) {
        Replay replay;
        replay.start();
        replay.key(0, 1, VK_F2);
        replay.send(recenter ? 95 : 10, smp::RawEventType::MouseMove, 0, 242, 500);
        replay.send(100, smp::RawEventType::KeyDown, VK_F2, 242, 500);
        replay.send(300, smp::RawEventType::MouseMove);
        const auto& result = replay.finish(400);
        std::vector<smp::ProductionVisit> visits(2);
        for (auto& visit : visits)
            visit.selectionAccess = smp::ProductionSelectionAccess::DirectClick;
        visits[0].contextTimestampTicks = 50;
        visits[1].contextTimestampTicks = 200;
        smp::annotateProductionAccessTelemetry(visits, result);
        REQUIRE(visits[0].cameraAccess == (recenter ? smp::ProductionCameraAccess::LocationHotkey
                                                  : smp::ProductionCameraAccess::EdgeScroll));
        REQUIRE(visits[1].cameraAccess == smp::ProductionCameraAccess::EdgeScroll);
        REQUIRE(visits[1].cameraAnchorTimestampTicks == 100);
        REQUIRE(visits[1].cameraEpisodeId == (recenter ? 3 : 4));
    }
}

TEST_CASE("capture sequence gap clears modifiers and retains the surviving key") {
    Replay replay;
    replay.start();
    replay.send(100, smp::RawEventType::KeyDown, VK_CONTROL);
    ++replay.sequence; // Missing key-up.
    replay.send(120, smp::RawEventType::KeyDown, '1');
    const auto& result = replay.finish(200);
    REQUIRE(result.captureDiscontinuityCount == 1);
    REQUIRE(result.missingCaptureEventCount == 1);
    REQUIRE(result.mechanicalEvents.back().type == smp::MechanicalInputType::ControlGroupSelect);
    REQUIRE(result.mechanicalEvents.back().value == 1);
    REQUIRE(result.mechanicalEvents.back().modifiers == smp::ModifierNone);
    REQUIRE(result.mechanicalEvents.back().captureEpoch == 1);
}

TEST_CASE("capture gap invalidates double taps and permits a new clean pair") {
    Replay replay;
    replay.start();
    replay.key(100, 110, '1');
    ++replay.sequence;
    replay.key(120, 130, '1');
    REQUIRE(replay.analyzer->result().navigationEvents.empty());
    REQUIRE(replay.analyzer->result().recenters.empty());
    replay.key(140, 150, '1');
    const auto& result = replay.finish(200);
    REQUIRE(result.navigationEvents.size() == 1);
    REQUIRE(result.navigationEvents[0].captureEpoch == 1);
    REQUIRE(replay.analyzer->takeEmittedNavigationEvents()[0].captureEpoch == 1);
}

TEST_CASE("capture gap discards candidate and active edge episodes") {
    for (bool active : {false, true}) {
        Replay replay;
        replay.start();
        replay.send(100, smp::RawEventType::MouseMove, 0, 240, 500);
        if (active) replay.send(130, smp::RawEventType::MouseMove, 0, 240, 500);
        ++replay.sequence;
        replay.send(200, smp::RawEventType::MouseMove, 0, 900, 500);
        REQUIRE(replay.finish(300).navigationEvents.empty());
    }
}

TEST_CASE("capture gap resets symbolic camera even when timestamps are equal") {
    Replay replay;
    replay.start();
    replay.key(100, 100, VK_F2);
    ++replay.sequence;
    replay.key(100, 100, VK_F2);
    replay.key(120, 120, VK_F2);
    const auto& result = replay.finish(200);
    REQUIRE(result.navigationEvents.size() == 2);
    REQUIRE(result.navigationEvents[0].captureEpoch == 0);
    REQUIRE(result.navigationEvents[1].captureEpoch == 1);
    REQUIRE(result.recenters.size() == 1);
    REQUIRE(result.recenters[0].captureEpoch == 1);
    REQUIRE(replay.analyzer->takeEmittedRecenters()[0].captureEpoch == 1);
}

TEST_CASE("zero raw sequences preserve legacy fixture behavior") {
    Replay replay;
    replay.start();
    replay.sequence = 0;
    replay.send(100, smp::RawEventType::KeyDown, VK_CONTROL);
    replay.sequence = 0;
    replay.send(120, smp::RawEventType::KeyDown, '1');
    const auto& result = replay.finish(200);
    REQUIRE(result.captureDiscontinuityCount == 0);
    REQUIRE(result.missingCaptureEventCount == 0);
    REQUIRE(result.mechanicalEvents.back().type == smp::MechanicalInputType::ControlGroupAssign);
    REQUIRE(result.mechanicalEvents.back().captureEpoch == 0);
}

TEST_CASE("initial duplicate and backward sequences invalidate without negative missing counts") {
    for (const auto sequence : {5ULL, 100ULL, 99ULL}) {
        smp::Analyzer analyzer({}, 1000);
        smp::RawInputEvent raw{};
        raw.sequence = sequence == 5 ? 5 : 100;
        raw.timestampTicks = 100;
        raw.type = smp::RawEventType::KeyDown;
        raw.virtualKey = VK_CONTROL;
        analyzer.process(raw);
        REQUIRE(analyzer.result().captureDiscontinuityCount == 1);
        REQUIRE(analyzer.result().missingCaptureEventCount == raw.sequence - 1);
        const auto initialMissing = analyzer.result().missingCaptureEventCount;
        raw.sequence = sequence == 5 ? 6 : sequence;
        raw.virtualKey = '1';
        analyzer.process(raw);
        REQUIRE(analyzer.result().missingCaptureEventCount == initialMissing);
        REQUIRE(analyzer.result().captureDiscontinuityCount == (sequence == 5 ? 1 : 2));
        REQUIRE(analyzer.result().mechanicalEvents.back().type == (sequence == 5
                    ? smp::MechanicalInputType::ControlGroupAssign
                    : smp::MechanicalInputType::ControlGroupSelect));
    }
    smp::Analyzer analyzer({}, 1000);
    smp::RawInputEvent raw{};
    raw.sequence = std::numeric_limits<std::uint64_t>::max();
    raw.type = smp::RawEventType::MouseLeftDown;
    analyzer.process(raw);
    raw.sequence = 1;
    analyzer.process(raw);
    REQUIRE(analyzer.result().missingCaptureEventCount == std::numeric_limits<std::uint64_t>::max() - 1);
    REQUIRE(analyzer.result().captureDiscontinuityCount == 2);
}

TEST_CASE("trailing collector loss clears edge modifiers and pending taps before finalize") {
    Replay replay;
    replay.start();
    replay.key(10, 20, VK_F2);
    replay.key(30, 40, '1');
    replay.send(50, smp::RawEventType::KeyDown, VK_CONTROL);
    replay.send(60, smp::RawEventType::MouseMove, 0, 240, 500);
    replay.analyzer->reconcileCollectorDrops(2);
    REQUIRE(replay.analyzer->cameraContext().type == smp::CameraContextType::Unknown);
    replay.analyzer->reconcileCollectorDrops(2); // Idempotent final reconciliation.
    replay.send(70, smp::RawEventType::KeyDown, '1');
    REQUIRE(replay.analyzer->result().mechanicalEvents.back().type == smp::MechanicalInputType::ControlGroupSelect);
    replay.analyzer->finalize(1000, 2);
    REQUIRE(navigationCount(replay.analyzer->result(), smp::CameraNavigationType::EdgeScroll) == 0);
    REQUIRE(navigationCount(replay.analyzer->result(), smp::CameraNavigationType::ControlGroupJump) == 0);
    REQUIRE(replay.analyzer->result().captureDiscontinuityCount == 1);
    REQUIRE(replay.analyzer->result().missingCaptureEventCount == 2);
    const auto count = replay.analyzer->result().mechanicalEvents.size();
    replay.send(1100, smp::RawEventType::MouseLeftDown);
    REQUIRE(replay.analyzer->result().mechanicalEvents.size() == count);
}

TEST_CASE("accounted collector loss and raw writer drops do not invalidate again") {
    Replay replay;
    replay.start();
    ++replay.sequence;
    replay.send(10, smp::RawEventType::MouseMove, 0, 240, 500);
    replay.analyzer->reconcileCollectorDrops(1);
    replay.analyzer->finalize(100, 7); // 1 collector + 6 optional writer drops.
    REQUIRE(replay.analyzer->result().captureDiscontinuityCount == 1);
    REQUIRE(replay.analyzer->result().droppedEventCount == 7);
    REQUIRE(navigationCount(replay.analyzer->result(), smp::CameraNavigationType::EdgeScroll) == 1);
    Replay writerOnly;
    writerOnly.start();
    writerOnly.send(10, smp::RawEventType::MouseMove, 0, 240, 500);
    writerOnly.analyzer->reconcileCollectorDrops(0);
    writerOnly.analyzer->finalize(100, 6);
    REQUIRE(writerOnly.analyzer->result().captureDiscontinuityCount == 0);
    REQUIRE(navigationCount(writerOnly.analyzer->result(), smp::CameraNavigationType::EdgeScroll) == 1);
}

TEST_CASE("real bounded queue overflow exposes exactly one capture boundary") {
    smp::SpscRingBuffer<smp::RawInputEvent, 2> queue;
    smp::Analyzer analyzer({}, 1000);
    smp::RawInputEvent raw{};
    raw.sequence = 1;
    raw.type = smp::RawEventType::KeyDown;
    raw.virtualKey = VK_CONTROL;
    REQUIRE(queue.tryPush(raw));
    raw.sequence = 2;
    raw.type = smp::RawEventType::KeyUp;
    REQUIRE(!queue.tryPush(raw));
    smp::RawInputEvent surviving{};
    REQUIRE(queue.tryPop(surviving));
    REQUIRE(surviving.sequence == 1);
    analyzer.process(surviving);
    raw.sequence = 3;
    raw.type = smp::RawEventType::KeyDown;
    raw.virtualKey = '1';
    REQUIRE(queue.tryPush(raw));
    REQUIRE(queue.tryPop(surviving));
    REQUIRE(surviving.sequence == 3);
    analyzer.process(surviving);
    analyzer.reconcileCollectorDrops(1);
    REQUIRE(analyzer.result().captureDiscontinuityCount == 1);
    REQUIRE(analyzer.result().missingCaptureEventCount == 1);
    REQUIRE(analyzer.result().mechanicalEvents.back().type == smp::MechanicalInputType::ControlGroupSelect);
    REQUIRE(analyzer.result().mechanicalEvents.back().captureEpoch == 1);
}

TEST_CASE("capture gap missing counts accumulate while contiguous inputs keep their epoch") {
    Replay replay;
    replay.start(); // sequence 1.
    replay.sequence = 4;
    replay.send(10, smp::RawEventType::MouseLeftDown);
    replay.send(20, smp::RawEventType::MouseLeftUp); // sequence 5, no new boundary.
    REQUIRE(replay.analyzer->result().captureDiscontinuityCount == 1);
    REQUIRE(replay.analyzer->result().missingCaptureEventCount == 2);
    REQUIRE(replay.analyzer->result().mechanicalEvents[0].captureEpoch == 1);
    REQUIRE(replay.analyzer->result().mechanicalEvents[1].captureEpoch == 1);
    replay.sequence = 7;
    replay.send(30, smp::RawEventType::MouseRightDown);
    REQUIRE(replay.analyzer->result().captureDiscontinuityCount == 2);
    REQUIRE(replay.analyzer->result().missingCaptureEventCount == 3);
    replay.analyzer->reconcileCollectorDrops(5);
    REQUIRE(replay.analyzer->result().captureDiscontinuityCount == 3);
    REQUIRE(replay.analyzer->result().missingCaptureEventCount == 5);
}

TEST_CASE("dropped foreground gain resumes at the surviving numbered input") {
    Replay replay;
    replay.start(); // seq 1: gained at 0.
    replay.send(100, smp::RawEventType::ForegroundLost); // seq 2.
    ++replay.sequence; // seq 3: dropped gain, exact time unknown.
    replay.send(1000, smp::RawEventType::KeyDown, 'D'); // seq 4.
    REQUIRE(replay.analyzer->result().mechanicalEvents.size() == 1);
    const auto first = replay.analyzer->result().mechanicalEvents[0];
    REQUIRE(first.type == smp::MechanicalInputType::KeyPress);
    REQUIRE(first.virtualKey == 'D');
    REQUIRE(first.captureEpoch == 1);
    REQUIRE_NEAR(first.activeMs, 100.0, 0.001);
    replay.send(1010, smp::RawEventType::KeyUp, 'D');
    replay.send(1100, smp::RawEventType::KeyDown, 'Q');
    REQUIRE(replay.analyzer->result().mechanicalEvents.size() == 2);
    REQUIRE_NEAR(replay.analyzer->result().mechanicalEvents[1].activeMs, 200.0, 0.001);
    const auto& result = replay.finish(1200);
    REQUIRE_NEAR(result.activeDurationSeconds, 0.3, 0.001);
    REQUIRE_NEAR(result.pausedDurationSeconds, 0.9, 0.001);
}

TEST_CASE("dropped foreground loss followed by gain preserves observed active time") {
    Replay replay;
    replay.start();
    replay.key(400, 410, 'D');
    ++replay.sequence; // Dropped loss, before the next surviving gain.
    replay.send(1000, smp::RawEventType::ForegroundGained);
    replay.key(1100, 1110, 'Q');
    const auto& result = replay.finish(1200);
    REQUIRE(result.mechanicalEvents.size() == 2);
    REQUIRE_NEAR(result.mechanicalEvents[0].activeMs, 400.0, 0.001);
    // The key-up at 410 is also a successfully observed active event.
    REQUIRE_NEAR(result.mechanicalEvents[1].activeMs, 510.0, 0.001);
    REQUIRE(result.mechanicalEvents[1].activeMs > result.mechanicalEvents[0].activeMs);
    REQUIRE(result.mechanicalEvents[1].captureEpoch == 1);
    REQUIRE_NEAR(result.activeDurationSeconds, 0.610, 0.001);
    REQUIRE_NEAR(result.pausedDurationSeconds, 0.590, 0.001);
}

TEST_CASE("foreground recovery retains safely observed raw time even for suppressed autorepeat") {
    Replay replay;
    replay.start();
    replay.send(400, smp::RawEventType::KeyDown, 'D');
    replay.send(500, smp::RawEventType::KeyDown, 'D'); // Suppressed, but known active.
    ++replay.sequence;
    replay.send(1000, smp::RawEventType::ForegroundGained);
    replay.send(1100, smp::RawEventType::KeyDown, 'Q');
    const auto& result = replay.finish(1200);
    REQUIRE(result.mechanicalEvents.size() == 2);
    REQUIRE_NEAR(result.mechanicalEvents[1].activeMs, 600.0, 0.001);
    REQUIRE_NEAR(result.activeDurationSeconds, 0.7, 0.001);
    REQUIRE_NEAR(result.pausedDurationSeconds, 0.5, 0.001);
}

TEST_CASE("ordinary active capture gap does not manufacture a foreground pause") {
    Replay replay;
    replay.start();
    replay.key(400, 410, 'D');
    ++replay.sequence; // Missing ordinary mouse/key event, not a focus marker.
    replay.key(1000, 1010, 'Q');
    const auto& result = replay.finish(1200);
    REQUIRE_NEAR(result.mechanicalEvents[1].activeMs, 1000.0, 0.001);
    REQUIRE(result.mechanicalEvents[1].captureEpoch == 1);
    REQUIRE_NEAR(result.activeDurationSeconds, 1.2, 0.001);
    REQUIRE_NEAR(result.pausedDurationSeconds, 0.0, 0.001);
}

TEST_CASE("gap before a surviving loss conservatively closes the known active segment") {
    Replay replay;
    replay.start();
    replay.key(400, 410, 'D');
    replay.sequence += 2; // Lost and gained could both have been dropped.
    replay.send(1500, smp::RawEventType::ForegroundLost);
    replay.send(1600, smp::RawEventType::ForegroundLost); // Already inactive: no double count.
    replay.send(2000, smp::RawEventType::ForegroundGained);
    replay.key(2100, 2110, 'Q');
    const auto& result = replay.finish(2200);
    REQUIRE_NEAR(result.mechanicalEvents[1].activeMs, 510.0, 0.001);
    REQUIRE_NEAR(result.activeDurationSeconds, 0.610, 0.001);
    REQUIRE_NEAR(result.pausedDurationSeconds, 1.590, 0.001);
}

TEST_CASE("unnumbered input and numbered input without missing positions do not recover inactive focus") {
    for (bool numbered : {false, true}) {
        Replay replay;
        replay.start();
        replay.send(100, smp::RawEventType::ForegroundLost);
        if (!numbered) replay.sequence = 0;
        replay.send(1000, smp::RawEventType::KeyDown, 'D');
        const auto& result = replay.finish(1200);
        REQUIRE(result.mechanicalEvents.empty());
        REQUIRE_NEAR(result.activeDurationSeconds, 0.1, 0.001);
        REQUIRE_NEAR(result.pausedDurationSeconds, 1.1, 0.001);
    }
}

TEST_CASE("arbitrary omissions of live foreground markers never move active evidence backward") {
    struct Observation {
        std::uint64_t ticks;
        smp::RawEventType type;
        std::uint16_t key{};
    };
    const Observation observations[] = {
        {0, smp::RawEventType::ForegroundGained},
        {200, smp::RawEventType::KeyDown, 'D'},
        {210, smp::RawEventType::KeyUp, 'D'},
        {500, smp::RawEventType::ForegroundLost},
        {1000, smp::RawEventType::ForegroundGained},
        {1100, smp::RawEventType::KeyDown, VK_F2},
        {1110, smp::RawEventType::KeyUp, VK_F2},
        {1500, smp::RawEventType::ForegroundLost},
        {2000, smp::RawEventType::ForegroundGained},
        {2100, smp::RawEventType::MouseLeftDown},
        {2200, smp::RawEventType::KeyDown, VK_F2},
        {2210, smp::RawEventType::KeyUp, VK_F2},
        {2500, smp::RawEventType::ForegroundLost}};
    constexpr std::size_t count = sizeof(observations) / sizeof(observations[0]);
    for (std::uint32_t omitted = 0; omitted < (1U << count); ++omitted) {
        smp::Config config;
        config.minimap = {300, 800, 520, 1040};
        smp::Analyzer analyzer(config, 1000);
        std::size_t expectedMechanical = 0;
        std::uint64_t drops = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if ((omitted & (1U << i)) != 0) { ++drops; continue; }
            smp::RawInputEvent raw{};
            raw.sequence = i + 1;
            raw.timestampTicks = observations[i].ticks;
            raw.type = observations[i].type;
            raw.virtualKey = observations[i].key;
            raw.cursorX = 350;
            raw.cursorY = 900;
            analyzer.process(raw);
            if (raw.type == smp::RawEventType::KeyDown || raw.type == smp::RawEventType::MouseLeftDown)
                ++expectedMechanical;
        }
        analyzer.reconcileCollectorDrops(drops);
        analyzer.finalize(3000, drops);
        const auto& result = analyzer.result();
        REQUIRE(result.mechanicalEvents.size() == expectedMechanical);
        auto checkClock = [&](const auto& events) {
            double previous = 0.0;
            for (const auto& event : events) {
                REQUIRE(std::isfinite(event.activeMs));
                REQUIRE(event.activeMs >= previous);
                REQUIRE(event.activeMs <= result.activeDurationSeconds * 1000.0 + 0.001);
                previous = event.activeMs;
            }
        };
        checkClock(result.mechanicalEvents);
        checkClock(result.navigationEvents);
        checkClock(result.recenters);
        REQUIRE(std::isfinite(result.activeDurationSeconds));
        REQUIRE(std::isfinite(result.pausedDurationSeconds));
        REQUIRE(result.activeDurationSeconds >= 0.0);
        REQUIRE(result.pausedDurationSeconds >= 0.0);
        REQUIRE(result.activeDurationSeconds + result.pausedDurationSeconds <= 3.0 + 0.000001);
    }
}
