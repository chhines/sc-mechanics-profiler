#include "test_framework.h"

#include "analysis/army_control_group.h"
#include "analysis/replay_analysis.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t qpcFrequency = 1000;
constexpr double ownSpawnX = 0.0;
constexpr double ownSpawnY = 0.0;
constexpr double enemySpawnX = 3200.0;
constexpr double enemySpawnY = 0.0;

smp::ArmyControlGroupEdit edit(double activeMs, int group, std::uint32_t unitTag,
                               std::string unitType,
                               smp::ArmyControlGroupOperation operation =
                                   smp::ArmyControlGroupOperation::Assign) {
    smp::ArmyControlGroupEdit result;
    result.operationActiveMs = activeMs;
    result.operationQpc = static_cast<std::uint64_t>(activeMs);
    result.group = group;
    result.operation = operation;
    result.selectionMethod = smp::ArmySelectionMethod::ExistingSelection;
    result.scope = smp::ArmyControlGroupScope::Army;
    result.replayConfirmed = true;
    result.bindingConfidence =
        smp::ArmyControlGroupBindingConfidence::ReplayConfirmed;
    result.selectedUnitTags = {unitTag};
    if (!unitType.empty())
        result.selectedUnitTypes = {std::move(unitType)};
    result.selectedUnitCount = 1;
    return result;
}

smp::ScoutingUnitCommandEvidence command(std::size_t assignmentEditIndex,
                                         std::uint32_t unitTag,
                                         double activeMs,
                                         double targetX,
                                         double targetY = 0.0) {
    return {assignmentEditIndex, unitTag,
            ownSpawnX, ownSpawnY, enemySpawnX, enemySpawnY,
            targetX, targetY, activeMs};
}

smp::MechanicalInputEvent input(smp::MechanicalInputType type,
                                std::uint64_t qpc,
                                double activeMs,
                                int value = -1) {
    return {qpc, activeMs, type, 0, 0, smp::ModifierNone, value, 100, 100};
}

smp::ArmyControlGroupAnalysis analyze(
    std::vector<smp::ArmyControlGroupEdit> edits,
    std::vector<smp::ScoutingUnitCommandEvidence> evidence,
    std::vector<smp::MechanicalInputEvent> physicalEvents = {}) {
    smp::ArmyControlGroupAnalysis analysis;
    analysis.available = true;
    analysis.edits = std::move(edits);
    smp::rebuildArmyControlGroupStatistics(analysis);
    smp::applyScoutingUnitClassification(analysis, evidence);
    smp::AnalysisResult result;
    result.mechanicalEvents = std::move(physicalEvents);
    smp::analyzeScoutingUnitActivity(analysis, result, qpcFrequency);
    return analysis;
}

smp::MechanicalInputEvent timedInput(smp::MechanicalInputType type,
                                     std::uint64_t ms,
                                     int value = -1) {
    return input(type, ms, static_cast<double>(ms), value);
}

smp::ProductionAnalysis correlatedBase(const smp::AnalysisResult& live) {
    smp::ProductionAnalysis base;
    base.visitsAvailable = true;
    base.armyControlGroupManagement =
        smp::detectArmyControlGroupManagement(live, qpcFrequency);
    return base;
}

} // namespace

TEST_CASE("scouting redesign: enemy-side worker command confirms scout") {
    const auto analysis = analyze(
        {edit(35000.0, 2, 1001, "Probe")},
        {command(0, 1001, 50000.0, 2200.0)});

    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(analysis.assignments == 0);
    REQUIRE(analysis.excludedScoutingUnitEdits == 1);
    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    REQUIRE(analysis.scoutingUnitActivities[0].commandCount == 1);
}

TEST_CASE("scouting redesign: local builder command remains uncertain") {
    const auto analysis = analyze(
        {edit(35000.0, 2, 1001, "Probe")},
        {command(0, 1001, 50000.0, 400.0)});

    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(analysis.assignments == 0);
    REQUIRE(analysis.uncertainEdits == 1);
    REQUIRE(analysis.scoutingUnitActivities.empty());
}

TEST_CASE("scouting redesign: midpoint is not enough but enemy side is") {
    const auto midpoint = analyze(
        {edit(35000.0, 2, 1001, "Probe")},
        {command(0, 1001, 50000.0, 1600.0)});
    REQUIRE(midpoint.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);

    const auto enemySide = analyze(
        {edit(35000.0, 2, 1001, "Probe")},
        {command(0, 1001, 50000.0, 1601.0)});
    REQUIRE(enemySide.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
}

TEST_CASE("scouting redesign: repeated assignment of same worker is one unit activity") {
    const auto analysis = analyze(
        {edit(35000.0, 2, 1001, "Probe"),
         edit(35200.0, 2, 1001, "Probe"),
         edit(35400.0, 2, 1001, "Probe")},
        {command(0, 1001, 50000.0, 2200.0),
         command(1, 1001, 50000.0, 2200.0),
         command(2, 1001, 50000.0, 2200.0)});

    REQUIRE(analysis.excludedScoutingUnitEdits == 3);
    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    REQUIRE(analysis.scoutingUnitActivities[0].assignedQpc == 35000);
    REQUIRE(analysis.scoutingUnitActivities[0].commandCount == 1);
}

TEST_CASE("scouting redesign: hotkey overwrite does not lose scout unit identity") {
    const auto analysis = analyze(
        {edit(35000.0, 2, 1001, "Probe"),
         edit(60000.0, 2, 2001, "Zealot")},
        {command(0, 1001, 50000.0, 2200.0),
         command(0, 1001, 70000.0, 2600.0)});

    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(analysis.edits[1].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    REQUIRE(analysis.scoutingUnitActivities[0].commandCount == 2);
    REQUIRE_NEAR(*analysis.scoutingUnitActivities[0].lastCommandActiveMs,
                 70000.0, 0.001);
}

TEST_CASE("scouting redesign: shift add does not cancel an already identifiable scout") {
    const auto analysis = analyze(
        {edit(35000.0, 2, 1001, "Probe"),
         edit(36000.0, 2, 2001, "Zealot",
              smp::ArmyControlGroupOperation::Add)},
        {command(0, 1001, 50000.0, 2200.0)});

    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(analysis.edits[1].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(analysis.excludedScoutingUnitEdits == 1);
}

TEST_CASE("scouting redesign: left clicks do not invalidate replay-attributed scout commands") {
    const auto analysis = analyze(
        {edit(35000.0, 2, 1001, "Probe")},
        {command(0, 1001, 50000.0, 2200.0),
         command(0, 1001, 70000.0, 2600.0)},
        {timedInput(smp::MechanicalInputType::MouseRightDown, 50000),
         timedInput(smp::MechanicalInputType::MouseLeftDown, 60000),
         timedInput(smp::MechanicalInputType::MouseLeftUp, 60100),
         timedInput(smp::MechanicalInputType::MouseRightDown, 70000)});

    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    REQUIRE(analysis.scoutingUnitActivities[0].commandCount == 2);
    REQUIRE(*analysis.scoutingUnitActivities[0].lastCommandQpc == 70000);
}

TEST_CASE("scouting redesign: confirmed return home ends scouting before later mining commands") {
    const auto analysis = analyze(
        {edit(35000.0, 2, 1001, "Probe")},
        {command(0, 1001, 45000.0, 1000.0),
         command(0, 1001, 50000.0, 2300.0),
         command(0, 1001, 60000.0, 2700.0),
         command(0, 1001, 70000.0, 100.0),
         command(0, 1001, 80000.0, 50.0)});

    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    const auto& scout = analysis.scoutingUnitActivities[0];
    REQUIRE(scout.commandCount == 4);
    REQUIRE_NEAR(*scout.lastCommandActiveMs, 70000.0, 0.001);
    REQUIRE_NEAR(*scout.scoutingActivityDurationMs, 35000.0, 0.001);
}

TEST_CASE("scouting redesign: temporary return home does not end a later scouting excursion") {
    const auto analysis = analyze(
        {edit(35000.0, 2, 1001, "Probe")},
        {command(0, 1001, 50000.0, 2300.0),
         command(0, 1001, 60000.0, 100.0),
         command(0, 1001, 70000.0, 2600.0),
         command(0, 1001, 90000.0, 120.0),
         command(0, 1001, 100000.0, 80.0)});

    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    const auto& scout = analysis.scoutingUnitActivities[0];
    REQUIRE(scout.commandCount == 4);
    REQUIRE_NEAR(*scout.lastCommandActiveMs, 90000.0, 0.001);
}

TEST_CASE("scouting redesign: without return home the last unit command ends observed scouting") {
    const auto analysis = analyze(
        {edit(35000.0, 2, 1001, "Probe")},
        {command(0, 1001, 50000.0, 2300.0),
         command(0, 1001, 70000.0, 2600.0),
         command(0, 1001, 80000.0, 1000.0)});

    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    REQUIRE(analysis.scoutingUnitActivities[0].commandCount == 3);
    REQUIRE_NEAR(*analysis.scoutingUnitActivities[0].lastCommandActiveMs,
                 80000.0, 0.001);
}

TEST_CASE("scouting redesign: physical QPC is used when replay command can be matched") {
    auto scoutEdit = edit(40000.0, 2, 1001, "Probe");
    scoutEdit.operationQpc = 100000;
    const auto analysis = analyze(
        {scoutEdit},
        {command(0, 1001, 50000.0, 2300.0),
         command(0, 1001, 70000.0, 2600.0)},
        {input(smp::MechanicalInputType::MouseRightDown, 150000, 50020.0),
         input(smp::MechanicalInputType::MouseRightDown, 187000, 70020.0)});

    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    const auto& scout = analysis.scoutingUnitActivities[0];
    REQUIRE_NEAR(*scout.scoutingActivityDurationMs, 87000.0, 0.001);
    REQUIRE_NEAR(*scout.firstToLastCommandMs, 37000.0, 0.001);
}

TEST_CASE("scouting redesign: combat singleton is never promoted by enemy-side commands") {
    const auto analysis = analyze(
        {edit(35000.0, 2, 2001, "Zealot")},
        {command(0, 2001, 50000.0, 3000.0)});

    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(analysis.assignments == 1);
    REQUIRE(analysis.scoutingUnitActivities.empty());
}

TEST_CASE("scouting redesign: all three race workers remain eligible") {
    for (const std::string worker : {"Probe", "SCV", "Drone"}) {
        const auto analysis = analyze(
            {edit(35000.0, 2, 1001, worker)},
            {command(0, 1001, 50000.0, 2300.0)});
        REQUIRE(analysis.edits[0].scope ==
                smp::ArmyControlGroupScope::ScoutingUnit);
    }
}

TEST_CASE("scouting redesign: replay correlation uses the occupied enemy spawn") {
    smp::AnalysisResult live;
    live.activeDurationSeconds = 10.0;
    live.mechanicalEvents = {
        timedInput(smp::MechanicalInputType::ControlGroupSelect, 0, 4),
        timedInput(smp::MechanicalInputType::ControlGroupSelect, 1000, 5),
        timedInput(smp::MechanicalInputType::ControlGroupAssign, 2000, 1),
        timedInput(smp::MechanicalInputType::ControlGroupAssign, 2167, 1),
        timedInput(smp::MechanicalInputType::ControlGroupAssign, 3000, 1),
        timedInput(smp::MechanicalInputType::ControlGroupAssign, 3500, 2),
        timedInput(smp::MechanicalInputType::ControlGroupAssign, 3667, 2),
    };

    smp::ReplayData replay;
    replay.totalFrames = 240;
    replay.mapWidthPixels = 3200.0;
    replay.mapHeightPixels = 3200.0;
    replay.players = {
        {0, "player", 1, "Protoss"},
        {1, "enemy", 2, "Terran"},
    };
    replay.startLocations = {
        {1, 0.0, 0.0},
        {2, 3200.0, 3200.0},
    };
    replay.controlGroupSelections = {{0, 0, 4, 0}, {24, 0, 5, 1}};
    replay.selections = {
        {40, 0, smp::ReplaySelectionKind::Select, {100}, 2, {}},
        {70, 0, smp::ReplaySelectionKind::Select, {300}, 7, {"Zealot"}},
        {80, 0, smp::ReplaySelectionKind::Select, {200}, 10, {}},
    };
    replay.controlGroupEdits = {
        {48, 0, 1, smp::ArmyControlGroupOperation::Assign, 3},
        {52, 0, 1, smp::ArmyControlGroupOperation::Assign, 4},
        {72, 0, 1, smp::ArmyControlGroupOperation::Assign, 8},
        {84, 0, 2, smp::ArmyControlGroupOperation::Assign, 11},
        {88, 0, 2, smp::ArmyControlGroupOperation::Assign, 12},
    };
    replay.commandTargets = {
        {56, 0, 200.0, 200.0, 5},
        {76, 0, 2800.0, 2800.0, 9},
        {92, 0, 2800.0, 2800.0, 13},
    };
    replay.buildEvents = {{60, 0, 6}};

    smp::MacroHotkeyProfile hotkeys;
    const auto correlated = smp::correlateProductionVisitsWithReplay(
        live, hotkeys, qpcFrequency, correlatedBase(live), replay, "test");
    const auto& groups = correlated.armyControlGroupManagement;

    REQUIRE(correlated.replayCorrelation.available);
    REQUIRE(groups.edits.size() == 5);
    REQUIRE(groups.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(groups.edits[1].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(groups.edits[2].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(groups.edits[3].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(groups.edits[4].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(groups.scoutingUnitActivities.size() == 1);
    REQUIRE(groups.scoutingUnitActivities[0].commandCount == 1);
}
