#include "test_framework.h"

#include "analysis/army_command.h"
#include "analysis/replay_analysis.h"
#include "storage/session.h"

#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t testQpcFrequency = 1000;

smp::ArmyControlGroupEdit confirmedEdit(
    smp::ArmyControlGroupScope scope,
    std::vector<std::uint32_t> selectedUnitTags) {
    smp::ArmyControlGroupEdit edit;
    edit.scope = scope;
    edit.selectedUnitTags = std::move(selectedUnitTags);
    edit.replayConfirmed = true;
    edit.bindingConfidence =
        smp::ArmyControlGroupBindingConfidence::ReplayConfirmed;
    return edit;
}

smp::ArmyCommandCandidate candidate(
    double activeMs, std::vector<std::uint32_t> selectedUnitTags,
    std::size_t commandIndex = 0) {
    return {static_cast<std::int64_t>(activeMs / 42.0), commandIndex,
            activeMs, "Targeted Order", "AttackMove",
            std::move(selectedUnitTags)};
}

smp::ArmyCommandRoleEvidence standardEvidence() {
    smp::ArmyControlGroupAnalysis controlGroups;
    controlGroups.edits = {
        confirmedEdit(smp::ArmyControlGroupScope::Army, {100, 101}),
        confirmedEdit(smp::ArmyControlGroupScope::ScoutingUnit, {400}),
    };
    return smp::buildArmyCommandRoleEvidence({200}, {300}, controlGroups);
}

smp::MechanicalInputEvent mechanical(smp::MechanicalInputType type,
                                     std::uint64_t activeMs, int group,
                                     std::uint16_t modifiers =
                                         smp::ModifierNone) {
    return {activeMs, static_cast<double>(activeMs), type, 0, 0, modifiers,
            group, 100, 100};
}

smp::AnalysisResult liveForReplayCorrelation() {
    smp::AnalysisResult live;
    live.activeDurationSeconds = 130.0;
    live.mechanicalEvents = {
        mechanical(smp::MechanicalInputType::ControlGroupSelect, 120000, 1),
        mechanical(smp::MechanicalInputType::ControlGroupSelect, 121000, 2),
        mechanical(smp::MechanicalInputType::MouseLeftDown, 121450, -1,
                   smp::ModifierCtrl),
        mechanical(smp::MechanicalInputType::MouseLeftUp, 121470, -1,
                   smp::ModifierCtrl),
        mechanical(smp::MechanicalInputType::ControlGroupAssign, 121600, 5,
                   smp::ModifierCtrl),
    };
    return live;
}

} // namespace

TEST_CASE("screp parser retains reviewed direct unit command semantics") {
    const std::string fixture = R"json({
      "Header":{"Frames":100,"Players":[{"ID":0,"Name":"P"}]},
      "Commands":{"Cmds":[
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Right Click"},
         "Pos":{"X":12,"Y":34}},
        {"Frame":11,"PlayerID":0,"Type":{"Name":"Targeted Order"},
         "Order":{"Name":"AttackMove"},"Pos":{"X":56,"Y":78}},
        {"Frame":12,"PlayerID":0,"Type":{"Name":"Targeted Order"},
         "Order":{"Name":"CastPsionicStorm"},"Pos":{"X":90,"Y":91}},
        {"Frame":13,"PlayerID":0,"Type":{"Name":"Targeted Order"},
         "Order":{"Name":"CastScannerSweep"},"Pos":{"X":92,"Y":93}},
        {"Frame":14,"PlayerID":0,"Type":{"Name":"Stop"}},
        {"Frame":15,"PlayerID":0,"Type":{"Name":"Hold Position"}},
        {"Frame":16,"PlayerID":0,"Type":{"Name":"Targeted Order"},
         "Order":{"Name":"RallyPointTile"},"Pos":{"X":1,"Y":2}},
        {"Frame":17,"PlayerID":0,"Type":{"Name":"Train"},
         "Unit":{"Name":"Zealot","ID":65}},
        {"Frame":18,"PlayerID":0,"Type":{"Name":"Select"},
         "UnitTags":[]}
      ]}}
    )json";

    const auto replay = smp::parseScrepReplayJson(fixture);
    REQUIRE(replay.unitCommands.size() == 5);
    REQUIRE(replay.unitCommands[0].kind == "Right Click");
    REQUIRE(replay.unitCommands[0].commandIndex == 0);
    REQUIRE_NEAR(*replay.unitCommands[0].targetX, 12.0, 0.001);
    REQUIRE(replay.unitCommands[1].kind == "Targeted Order");
    REQUIRE(replay.unitCommands[1].order == "AttackMove");
    REQUIRE(replay.unitCommands[1].commandIndex == 1);
    REQUIRE(replay.unitCommands[2].order == "CastPsionicStorm");
    REQUIRE(replay.unitCommands[3].kind == "Stop");
    REQUIRE(replay.unitCommands[4].kind == "Hold Position");
    for (const auto& command : replay.unitCommands)
        REQUIRE(command.order != "CastScannerSweep");
    REQUIRE(replay.selections.size() == 1);
    REQUIRE(replay.selections[0].unitTags.empty());
}

TEST_CASE("empty Select clears replay selection before an army-like command") {
    const auto live = liveForReplayCorrelation();
    smp::ProductionAnalysis base;
    base.visitsAvailable = true;
    base.armyControlGroupManagement =
        smp::detectArmyControlGroupManagement(live, testQpcFrequency);

    smp::ReplayData replay;
    replay.totalFrames = 3100;
    replay.players = {{0, "player"}};
    replay.controlGroupSelections = {{2857, 0, 1, 0}, {2881, 0, 2, 1}};
    replay.selections = {
        {2892, 0, smp::ReplaySelectionKind::Select, {100, 101}, 2, {}},
        {2896, 0, smp::ReplaySelectionKind::Select, {}, 4, {}},
    };
    replay.controlGroupEdits = {
        {2895, 0, 5, smp::ArmyControlGroupOperation::Assign, 3}};
    replay.unitCommands = {
        {2897, 0, 5, "Targeted Order", "AttackMove", 100.0, 100.0}};

    smp::MacroHotkeyProfile hotkeys;
    const auto correlated = smp::correlateProductionVisitsWithReplay(
        live, hotkeys, testQpcFrequency, std::move(base), replay, "test");
    REQUIRE(correlated.armyCommandActivity.available);
    REQUIRE(correlated.armyCommandActivity.commandCount == 0);
    REQUIRE(correlated.armyCommandActivity.unresolvedSelectionCommands == 1);
}

TEST_CASE("army command attribution requires a wholly inferred Army selection") {
    const auto evidence = standardEvidence();
    const auto analysis = smp::analyzeArmyCommands(
        {
            candidate(1000.0, {100, 101}, 0),
            candidate(2000.0, {200}, 1),
            candidate(3000.0, {300}, 2),
            candidate(4000.0, {400}, 3),
            candidate(5000.0, {100, 200}, 4),
            candidate(6000.0, {100, 999}, 5),
            candidate(7000.0, {}, 6),
            candidate(61000.0, {100, 101}, 7),
        },
        evidence, 60.0);

    REQUIRE(analysis.available);
    REQUIRE(analysis.commandCount == 1);
    REQUIRE(analysis.unresolvedSelectionCommands == 6);
    REQUIRE_NEAR(*analysis.commandsPerMinute(), 1.0, 0.001);
}

TEST_CASE("whole-replay clean Army evidence applies retrospectively") {
    smp::ArmyControlGroupAnalysis controlGroups;
    auto laterEdit = confirmedEdit(smp::ArmyControlGroupScope::Army,
                                   {100, 101});
    laterEdit.operationActiveMs = 420000.0;
    controlGroups.edits.push_back(std::move(laterEdit));
    const auto evidence = smp::buildArmyCommandRoleEvidence(
        {}, {}, controlGroups);

    const auto analysis = smp::analyzeArmyCommands(
        {candidate(300000.0, {100, 101})}, evidence, 600.0);
    REQUIRE(analysis.commandCount == 1);
}

TEST_CASE("contaminated Army-scope control-group edit seeds no Army tags") {
    smp::ArmyControlGroupAnalysis controlGroups;
    controlGroups.edits.push_back(
        confirmedEdit(smp::ArmyControlGroupScope::Army, {200, 999}));
    const auto evidence = smp::buildArmyCommandRoleEvidence(
        {200}, {}, controlGroups);
    REQUIRE(evidence.armyTags.empty());

    const auto analysis = smp::analyzeArmyCommands(
        {candidate(1000.0, {999})}, evidence, 60.0);
    REQUIRE(analysis.commandCount == 0);
}

TEST_CASE("army command gaps reuse exact interpolated percentile semantics") {
    const auto evidence = standardEvidence();
    const auto analysis = smp::analyzeArmyCommands(
        {
            candidate(10000.0, {100, 101}, 0),
            candidate(12000.0, {100, 101}, 1),
            candidate(17000.0, {100, 101}, 2),
            candidate(27000.0, {100, 101}, 3),
        },
        evidence, 60.0);

    REQUIRE(analysis.commandCount == 4);
    REQUIRE(analysis.gapDurationsMs.size() == 3);
    REQUIRE_NEAR(analysis.gapDurationsMs[0], 2000.0, 0.001);
    REQUIRE_NEAR(analysis.gapDurationsMs[1], 5000.0, 0.001);
    REQUIRE_NEAR(analysis.gapDurationsMs[2], 10000.0, 0.001);
    REQUIRE_NEAR(*analysis.commandsPerMinute(), 4.0, 0.001);
    REQUIRE_NEAR(*analysis.medianGapMs(), 5000.0, 0.001);
    REQUIRE_NEAR(*analysis.p90GapMs(), 9000.0, 0.001);
    REQUIRE_NEAR(*analysis.longestGapMs(), 10000.0, 0.001);
}

TEST_CASE("one or zero Army commands keep gap statistics unavailable") {
    const auto evidence = standardEvidence();
    const auto one = smp::analyzeArmyCommands(
        {candidate(1000.0, {100, 101})}, evidence, 60.0);
    REQUIRE(one.commandCount == 1);
    REQUIRE_NEAR(*one.commandsPerMinute(), 1.0, 0.001);
    REQUIRE(!one.medianGapMs().has_value());
    REQUIRE(!one.p90GapMs().has_value());
    REQUIRE(!one.longestGapMs().has_value());

    const auto zero = smp::analyzeArmyCommands({}, evidence, 60.0);
    REQUIRE(zero.available);
    REQUIRE(zero.commandCount == 0);
    REQUIRE_NEAR(*zero.commandsPerMinute(), 0.0, 0.001);
    REQUIRE(!zero.medianGapMs().has_value());
}

TEST_CASE("same-time Army commands retain a legitimate zero gap") {
    const auto analysis = smp::analyzeArmyCommands(
        {candidate(1000.0, {100, 101}, 0),
         candidate(1000.0, {100, 101}, 1)},
        standardEvidence(), 60.0);
    REQUIRE(analysis.gapDurationsMs.size() == 1);
    REQUIRE_NEAR(analysis.gapDurationsMs[0], 0.0, 0.001);
    REQUIRE_NEAR(*analysis.medianGapMs(), 0.0, 0.001);
    REQUIRE_NEAR(*analysis.p90GapMs(), 0.0, 0.001);
    REQUIRE_NEAR(*analysis.longestGapMs(), 0.0, 0.001);
}

TEST_CASE("zero active duration makes Army commands per minute unavailable") {
    const auto analysis =
        smp::analyzeArmyCommands({}, standardEvidence(), 0.0);
    REQUIRE(analysis.available);
    REQUIRE(!analysis.commandsPerMinute().has_value());
}

TEST_CASE("derived JSON persists Army command KPIs gaps and observations") {
    smp::AnalysisResult live;
    live.activeDurationSeconds = 60.0;
    smp::ProductionAnalysis production;
    production.armyCommandActivity = smp::analyzeArmyCommands(
        {candidate(10000.0, {100, 101}, 0),
         candidate(12000.0, {100, 101}, 1)},
        standardEvidence(), live.activeDurationSeconds);
    smp::MacroHotkeyProfile hotkeys;

    const auto encoded =
        smp::analysisToJson(live, "army-command", production, hotkeys);
    const auto& armyCommands = encoded["army_command_activity"];
    REQUIRE(armyCommands["available"].asBool(false));
    REQUIRE(armyCommands["command_count"].asInt() == 2);
    REQUIRE_NEAR(armyCommands["commands_per_minute"].asNumber(), 2.0,
                 0.001);
    REQUIRE(armyCommands["gap_durations_ms"].asArray().size() == 1);
    REQUIRE_NEAR(armyCommands["gap_durations_ms"].asArray()[0].asNumber(),
                 2000.0, 0.001);
    REQUIRE_NEAR(armyCommands["median_gap_ms"].asNumber(), 2000.0, 0.001);
    REQUIRE_NEAR(armyCommands["p90_gap_ms"].asNumber(), 2000.0, 0.001);
    REQUIRE_NEAR(armyCommands["longest_gap_ms"].asNumber(), 2000.0, 0.001);
    REQUIRE(armyCommands["commands"].asArray().size() == 2);
    REQUIRE(armyCommands["commands"].asArray()[0]["order"].asString() ==
            "AttackMove");
}
