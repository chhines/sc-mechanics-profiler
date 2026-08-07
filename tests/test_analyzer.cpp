#include "test_framework.h"

#include "analysis/analyzer.h"

#include <algorithm>
#include <cstdint>
#include <windows.h>

namespace {

class Replay {
  public:
    Replay() {
        config.viewport = {100, 100, 900, 700};
        config.minimap = {0, 600, 100, 700};
        config.commandCard = {900, 500, 1200, 700};
    }

    void start() {
        analyzer.emplace(config, 1000);
        send(0, scm::RawEventType::ForegroundGained);
    }

    void send(std::uint64_t ms, scm::RawEventType type, std::uint16_t key = 0, int x = 500, int y = 400) {
        scm::RawInputEvent event{};
        event.sequence = sequence++;
        event.timestampTicks = ms;
        event.type = type;
        event.virtualKey = key;
        event.cursorX = x;
        event.cursorY = y;
        analyzer->process(event);
    }

    void key(std::uint64_t ms, std::uint16_t key) {
        send(ms, scm::RawEventType::KeyDown, key);
        send(ms + 1, scm::RawEventType::KeyUp, key);
    }

    void click(std::uint64_t ms, int x = 500, int y = 400) {
        send(ms, scm::RawEventType::MouseLeftDown, 0, x, y);
        send(ms + 1, scm::RawEventType::MouseLeftUp, 0, x, y);
    }

    const scm::AnalysisResult& finish(std::uint64_t ms) {
        analyzer->finalize(ms, 0);
        return analyzer->result();
    }

    scm::Config config;
    std::optional<scm::Analyzer> analyzer;
    std::uint64_t sequence{1};
};

std::size_t countType(const scm::Analyzer& analyzer, scm::LogicalEventType type) {
    return static_cast<std::size_t>(std::count_if(analyzer.logicalEvents().begin(), analyzer.logicalEvents().end(),
                                                  [&](const auto& event) { return event.type == type; }));
}

} // namespace

TEST_CASE("control groups detect selection assignment double tap switch and return") {
    Replay replay;
    replay.start();
    replay.key(10, '1');
    replay.send(50, scm::RawEventType::KeyDown, VK_CONTROL);
    replay.key(51, '2');
    replay.send(53, scm::RawEventType::KeyUp, VK_CONTROL);
    replay.key(100, '1');
    replay.key(200, '1');
    replay.key(600, '2');
    replay.key(700, '1');
    replay.key(750, 'S');
    const auto& result = replay.finish(1000);
    REQUIRE(countType(*replay.analyzer, scm::LogicalEventType::ControlGroupAssign) == 1);
    REQUIRE(countType(*replay.analyzer, scm::LogicalEventType::ControlGroupDoubleTap) == 2);
    REQUIRE(result.controlGroupSwitchCount == 2);
    REQUIRE(result.returnLatenciesMs.size() == 1);
    REQUIRE_NEAR(result.returnLatenciesMs[0], 100.0, 0.01);
    REQUIRE(result.returnToActionLatenciesMs.size() == 1);
    REQUIRE_NEAR(result.returnToActionLatenciesMs[0], 50.0, 0.01);
}

TEST_CASE("synthetic control-group PAC computes first action and target latency") {
    Replay replay;
    replay.start();
    replay.key(0, '1');
    replay.key(100, '1');
    replay.key(260, 'A');
    replay.click(340);
    const auto& result = replay.finish(500);
    REQUIRE(result.pacs.size() == 1);
    REQUIRE(result.pacs[0].firstActionMs.has_value());
    REQUIRE_NEAR(*result.pacs[0].firstActionMs, 160.0, 0.01);
    REQUIRE(result.commandTargets.size() == 1);
    REQUIRE_NEAR(result.commandTargets[0].latencyMs, 80.0 + 1.0, 0.01);
}

TEST_CASE("targeted command replacement ignores UI clicks and preserves switch completion latency") {
    Replay replay;
    replay.start();
    replay.key(10, '1');
    replay.key(100, '2');
    replay.key(200, 'A');
    replay.click(300, 1000, 600); // command card: not a target completion
    replay.key(400, 'M');
    replay.click(450, 500, 400);
    const auto& result = replay.finish(600);
    REQUIRE(result.commandTargets.size() == 1);
    REQUIRE(result.commandTargets[0].command == scm::LogicalEventType::MoveCommandStart);
    REQUIRE_NEAR(result.commandTargets[0].latencyMs, 51.0, 0.01);
    REQUIRE(result.controlGroupCompletedCommandLatenciesMs.size() == 1);
    REQUIRE_NEAR(result.controlGroupCompletedCommandLatenciesMs[0], 351.0, 0.01);
}

TEST_CASE("location hotkey, minimap click, viewport click, and edge scrolling are classified") {
    Replay replay;
    replay.config.edgeDwellMs = 100;
    replay.start();
    replay.key(10, VK_F2);
    replay.click(100, 50, 650);
    replay.click(200, 500, 400);
    replay.send(300, scm::RawEventType::MouseMove, 0, 102, 300);
    replay.send(410, scm::RawEventType::MouseMove, 0, 102, 301);
    replay.send(600, scm::RawEventType::MouseMove, 0, 500, 400);
    const auto& result = replay.finish(800);
    REQUIRE(countType(*replay.analyzer, scm::LogicalEventType::LocationHotkey) == 1);
    REQUIRE(countType(*replay.analyzer, scm::LogicalEventType::MinimapClick) == 1);
    REQUIRE(countType(*replay.analyzer, scm::LogicalEventType::ViewportClick) == 1);
    REQUIRE(countType(*replay.analyzer, scm::LogicalEventType::EdgeScrollStart) == 1);
    REQUIRE(countType(*replay.analyzer, scm::LogicalEventType::EdgeScrollEnd) == 1);
    REQUIRE(result.navigation.size() == 3);
}

TEST_CASE("shift location hotkey assigns without starting a PAC") {
    Replay replay;
    replay.start();
    replay.send(10, scm::RawEventType::KeyDown, VK_SHIFT);
    replay.key(20, VK_F3);
    replay.send(22, scm::RawEventType::KeyUp, VK_SHIFT);
    const auto& result = replay.finish(100);
    REQUIRE(countType(*replay.analyzer, scm::LogicalEventType::LocationHotkeyAssign) == 1);
    REQUIRE(result.pacs.empty());
}

TEST_CASE("box selection measures geometry path efficiency context and command latency") {
    Replay replay;
    replay.start();
    replay.key(0, VK_F2);
    replay.send(150, scm::RawEventType::MouseLeftDown, 0, 200, 200);
    replay.send(190, scm::RawEventType::MouseMove, 0, 250, 240);
    replay.send(270, scm::RawEventType::MouseLeftUp, 0, 300, 300);
    replay.key(340, 'A');
    replay.click(430, 500, 400);
    const auto& result = replay.finish(600);
    REQUIRE(result.boxes.size() == 1);
    const auto& box = result.boxes[0];
    REQUIRE(box.width == 100);
    REQUIRE(box.height == 100);
    REQUIRE_NEAR(box.endActiveMs - box.startActiveMs, 120.0, 0.01);
    REQUIRE(box.pathEfficiency > 0.9 && box.pathEfficiency <= 1.0);
    REQUIRE(box.contextStartLatencyMs.has_value());
    REQUIRE_NEAR(*box.contextStartLatencyMs, 150.0, 0.01);
    REQUIRE(box.commandLatencyMs.has_value());
    REQUIRE_NEAR(*box.commandLatencyMs, 70.0, 0.01);
    REQUIRE_NEAR(result.commandTargets[0].latencyMs, 91.0, 0.01);
}

TEST_CASE("click without drag does not create box and overlapping boxes form probable re-selection") {
    Replay replay;
    replay.start();
    replay.click(10, 200, 200);
    replay.send(100, scm::RawEventType::MouseLeftDown, 0, 200, 200);
    replay.send(120, scm::RawEventType::MouseMove, 0, 300, 300);
    replay.send(150, scm::RawEventType::MouseLeftUp, 0, 300, 300);
    replay.send(200, scm::RawEventType::MouseLeftDown, 0, 205, 205);
    replay.send(220, scm::RawEventType::MouseMove, 0, 305, 305);
    replay.send(250, scm::RawEventType::MouseLeftUp, 0, 305, 305);
    const auto& result = replay.finish(500);
    REQUIRE(result.boxes.size() == 2);
    REQUIRE(result.boxes[1].probableReselection);
    REQUIRE(*result.boxes[1].reselectionIou >= 0.5);
}

TEST_CASE("configured worker and army attempts form macro episodes") {
    Replay replay;
    replay.config.workerRules = {{4, {'P'}}};
    replay.config.armyRules = {{5, {'D'}}, {6, {'Z'}}};
    replay.start();
    replay.key(10, '4');
    replay.key(50, 'P');
    replay.key(200, '5');
    replay.key(250, 'D');
    replay.key(400, '6');
    replay.key(450, 'Z');
    replay.key(3000, '4');
    replay.key(3050, 'P');
    const auto& result = replay.finish(4000);
    REQUIRE(countType(*replay.analyzer, scm::LogicalEventType::MacroWorkerAttempt) == 2);
    REQUIRE(countType(*replay.analyzer, scm::LogicalEventType::MacroArmyAttempt) == 2);
    REQUIRE(result.macroEpisodes.size() == 2);
    REQUIRE(result.macroEpisodes[0].workerAttempts == 1);
    REQUIRE(result.macroEpisodes[0].armyAttempts == 2);
}

TEST_CASE("worker and army macro metrics remain separate across complete episodes") {
    Replay replay;
    replay.config.workerRules = {{4, {'Q'}}};
    replay.config.armyRules = {{5, {'D'}}, {6, {'Z'}}};
    replay.start();
    for (int i = 0; i < 6; ++i) {
        const auto base = static_cast<std::uint64_t>(i * 3000);
        replay.key(base + 10, '4');
        replay.key(base + 50, 'Q');
        replay.key(base + 100, '5');
        replay.key(base + 150, 'D');
        replay.key(base + 200, '6');
        replay.key(base + 250, 'Z');
    }
    const auto& result = replay.finish(18000);
    REQUIRE(result.macroEpisodes.size() == 6);
    REQUIRE(result.workerInterval.median.has_value());
    REQUIRE_NEAR(*result.workerInterval.median, 3000.0, 0.01);
    REQUIRE(result.armyRevisit.median.has_value());
    REQUIRE_NEAR(*result.armyRevisit.median, 3000.0, 0.01);
    REQUIRE(result.armyEpisodeDuration.median.has_value());
    REQUIRE(result.productionGroupCoverage.has_value());
    REQUIRE_NEAR(*result.productionGroupCoverage, 1.0, 0.001);
    REQUIRE(result.armyProductionGroupCoverage.has_value());
    REQUIRE_NEAR(*result.armyProductionGroupCoverage, 1.0, 0.001);
    REQUIRE_NEAR(*result.combinedMacroBurstRatio, 1.0, 0.001);
}

TEST_CASE("micro-burst heuristic ends and measures return to macro") {
    Replay replay;
    replay.config.microMinimumEvents = 3;
    replay.config.microWindowMs = 2000;
    replay.config.microEndQuietMs = 1000;
    replay.config.workerRules = {{4, {'Q'}}};
    replay.start();
    replay.key(10, '1');
    replay.key(100, '2');
    replay.key(200, '3');
    replay.key(1500, '4');
    replay.key(1550, 'Q');
    const auto& result = replay.finish(2000);
    REQUIRE(countType(*replay.analyzer, scm::LogicalEventType::MicroBurstStart) == 1);
    REQUIRE(countType(*replay.analyzer, scm::LogicalEventType::MicroBurstEnd) == 1);
    REQUIRE(result.microMacroReturnMs.size() == 1);
    REQUIRE_NEAR(result.microMacroReturnMs[0], 350.0, 0.01);
}

TEST_CASE("repeated action n-grams are reported after five occurrences") {
    Replay replay;
    replay.start();
    for (int i = 0; i < 5; ++i) {
        const auto base = static_cast<std::uint64_t>(i * 1000);
        replay.key(base + 10, '1');
        replay.key(base + 100, 'S');
        replay.send(base + 200, scm::RawEventType::MouseRightDown, 0, 500, 400);
        (void)replay.analyzer->takeEmittedEvents();
    }
    const auto& result = replay.finish(6000);
    REQUIRE(!result.sequences.empty());
    REQUIRE(result.sequences.front().count >= 5);
}

TEST_CASE("foreground pauses exclude inactive time and terminate actionless PACs") {
    Replay replay;
    replay.start();
    replay.key(100, VK_F2);
    replay.send(500, scm::RawEventType::ForegroundLost);
    replay.send(1500, scm::RawEventType::ForegroundGained);
    replay.key(1600, VK_F3);
    const auto& result = replay.finish(2000);
    REQUIRE_NEAR(result.activeDurationSeconds, 1.0, 0.001);
    REQUIRE_NEAR(result.pausedDurationSeconds, 1.0, 0.001);
    REQUIRE(result.pacs.size() == 2);
    REQUIRE(result.pacs[0].actionless);
}
