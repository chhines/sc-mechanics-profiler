#include "test_framework.h"

#include "analysis/analyzer.h"
#include "analysis/army_control_group.h"
#include "analysis/scouting_travel_gate.h"
#include "config/config.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <utility>
#include <windows.h>

namespace {

smp::RawInputEvent raw(std::uint64_t ticks, smp::RawEventType type,
                       std::uint16_t key = 0, int x = 50, int y = 50) {
    smp::RawInputEvent event{};
    event.timestampTicks = ticks;
    event.type = type;
    event.virtualKey = key;
    event.cursorX = x;
    event.cursorY = y;
    return event;
}

smp::ArmyControlGroupEdit singletonEdit(std::string unitType) {
    smp::ArmyControlGroupEdit edit;
    edit.operationActiveMs = 30000.0;
    edit.operationQpc = 30000;
    edit.group = 2;
    edit.operation = smp::ArmyControlGroupOperation::Assign;
    edit.selectionMethod = smp::ArmySelectionMethod::ExistingSelection;
    edit.scope = smp::ArmyControlGroupScope::Army;
    edit.replayConfirmed = true;
    edit.bindingConfidence = smp::ArmyControlGroupBindingConfidence::ReplayConfirmed;
    edit.selectedUnitTags = {1001};
    if (!unitType.empty())
        edit.selectedUnitTypes = {std::move(unitType)};
    edit.selectedUnitCount = 1;
    return edit;
}

smp::ScoutingUnitCommandEvidence enemyWardCommand() {
    smp::ScoutingUnitCommandEvidence evidence;
    evidence.assignmentEditIndex = 0;
    evidence.unitTag = 1001;
    evidence.ownSpawnX = 0.0;
    evidence.ownSpawnY = 0.0;
    evidence.enemySpawnX = 3200.0;
    evidence.enemySpawnY = 0.0;
    evidence.targetX = 2400.0;
    evidence.targetY = 0.0;
    evidence.commandActiveMs = 50000.0;
    return evidence;
}

} // namespace

TEST_CASE("travel gate excludes a confirmed typed worker scout") {
    smp::ArmyControlGroupAnalysis analysis;
    analysis.available = true;
    analysis.edits = {singletonEdit("Probe")};
    analysis.edits[0].scope = smp::ArmyControlGroupScope::Worker;
    smp::rebuildArmyControlGroupStatistics(analysis);

    smp::applyTravelGatedScoutingUnitClassification(analysis, {enemyWardCommand()});

    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(analysis.assignments == 0);
    REQUIRE(analysis.excludedWorkerEdits == 0);
    REQUIRE(analysis.excludedScoutingUnitEdits == 1);
}

TEST_CASE("analyzer treats configured non-F location keys as ordinary key presses") {
    smp::Config config;
    config.locationHotkeys = {'A'};
    smp::Analyzer analyzer(config, 1000);
    analyzer.process(raw(0, smp::RawEventType::ForegroundGained));
    analyzer.process(raw(10, smp::RawEventType::KeyDown, 'A'));
    analyzer.process(raw(20, smp::RawEventType::KeyUp, 'A'));
    analyzer.finalize(30, 0);

    const auto& result = analyzer.result();
    REQUIRE(result.locationRecallCount == 0);
    REQUIRE(result.navigationEvents.empty());
    REQUIRE(result.mechanicalEvents.size() == 1);
    REQUIRE(result.mechanicalEvents[0].type == smp::MechanicalInputType::KeyPress);
    REQUIRE(result.mechanicalEvents[0].value == -1);
}

TEST_CASE("config discards non-F location hotkeys") {
    const auto path = std::filesystem::temp_directory_path() /
                      "smp_review_location_hotkeys.json";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << R"({"location_hotkeys":{"recall":["A","F3"]}})";
    }

    const auto config = smp::Config::loadOrCreate(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    REQUIRE(config.locationHotkeys.size() == 1);
    REQUIRE(config.locationHotkeys[0] == VK_F3);
}

TEST_CASE("backward timestamps cannot manufacture a control-group double tap") {
    smp::Analyzer analyzer(smp::Config{}, 1000);
    analyzer.process(raw(0, smp::RawEventType::ForegroundGained));
    analyzer.process(raw(100, smp::RawEventType::KeyDown, '1'));
    analyzer.process(raw(101, smp::RawEventType::KeyUp, '1'));
    analyzer.process(raw(50, smp::RawEventType::KeyDown, '1'));
    analyzer.process(raw(51, smp::RawEventType::KeyUp, '1'));
    analyzer.finalize(200, 0);

    REQUIRE(analyzer.result().navigationEvents.empty());
    REQUIRE(analyzer.result().recenters.empty());
}

TEST_CASE("backward timestamps cannot create a wrapped edge-scroll duration") {
    smp::Config config;
    config.gameArea = {0, 0, 99, 99};
    config.viewport = {0, 0, 99, 79};
    config.edgeMarginPx = 5;
    config.edgeMinimumDwellMs = 20;
    smp::Analyzer analyzer(config, 1000);

    analyzer.process(raw(0, smp::RawEventType::ForegroundGained));
    analyzer.process(raw(100, smp::RawEventType::MouseMove, 0, 0, 50));
    analyzer.process(raw(50, smp::RawEventType::MouseMove, 0, 50, 50));
    analyzer.finalize(200, 0);

    REQUIRE(analyzer.result().navigationEvents.empty());
}
