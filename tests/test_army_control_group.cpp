#include "test_framework.h"

#include "analysis/army_control_group.h"
#include "analysis/replay_analysis.h"
#include "cli/automatic_session_stats.h"
#include "cli/report.h"
#include "storage/session.h"

#include <cstdint>
#include <vector>

namespace {

constexpr std::uint64_t qpcFrequency = 1000;

smp::MechanicalInputEvent event(smp::MechanicalInputType type, std::uint64_t ms,
                                std::uint16_t modifiers = smp::ModifierNone,
                                int value = -1, int x = 100, int y = 100) {
    return {ms, static_cast<double>(ms), type, 0, 0, modifiers, value, x, y};
}

smp::ArmyControlGroupEdit singleEdit(const std::vector<smp::MechanicalInputEvent>& events,
                                     double activeSeconds = 5.0) {
    smp::AnalysisResult result;
    result.activeDurationSeconds = activeSeconds;
    result.mechanicalEvents = events;
    const auto analysis = smp::detectArmyControlGroupManagement(result, qpcFrequency);
    REQUIRE(analysis.edits.size() == 1);
    return analysis.edits.front();
}

smp::AnalysisResult correlatedLive(smp::ArmyControlGroupOperation operation) {
    smp::AnalysisResult live;
    live.activeDurationSeconds = 3.0;
    live.mechanicalEvents = {
        event(smp::MechanicalInputType::ControlGroupSelect, 1000, smp::ModifierNone, 1),
        event(smp::MechanicalInputType::ControlGroupSelect, 2000, smp::ModifierNone, 2),
        event(smp::MechanicalInputType::MouseLeftDown, 2450, smp::ModifierCtrl, -1, 300, 300),
        event(smp::MechanicalInputType::MouseLeftUp, 2470, smp::ModifierCtrl, -1, 300, 300),
        event(operation == smp::ArmyControlGroupOperation::Assign
                  ? smp::MechanicalInputType::ControlGroupAssign
                  : smp::MechanicalInputType::ControlGroupAdd,
              2600,
              operation == smp::ArmyControlGroupOperation::Assign ? smp::ModifierCtrl
                                                                   : smp::ModifierShift,
              5),
    };
    return live;
}

smp::ProductionAnalysis correlatedBase(const smp::AnalysisResult& live) {
    smp::ProductionAnalysis base;
    base.visitsAvailable = true;
    base.armyControlGroupManagement =
        smp::detectArmyControlGroupManagement(live, qpcFrequency);
    return base;
}

smp::ReplayData correlatedReplay(std::string unitType,
                                 smp::ArmyControlGroupOperation operation,
                                 bool productionBuilding) {
    smp::ReplayData replay;
    replay.totalFrames = 72;
    replay.players = {{0, "player"}};
    replay.controlGroupSelections = {{24, 0, 1, 0}, {48, 0, 2, 1}};
    replay.selections.push_back(
        {59, 0, smp::ReplaySelectionKind::Select, {9001}, 2, {std::move(unitType)}});
    replay.controlGroupEdits.push_back({62, 0, 5, operation, 3});
    if (productionBuilding) {
        replay.productionEvents.push_back(
            {63, 0, smp::ReplayProductionKind::Train, "Probe", 0x40, 4});
    }
    return replay;
}

} // namespace

TEST_CASE("army control-group box assign retains QPC selection timing") {
    const auto edit = singleEdit({
        event(smp::MechanicalInputType::MouseLeftDown, 100, smp::ModifierNone, -1, 10, 10),
        event(smp::MechanicalInputType::MouseLeftUp, 280, smp::ModifierNone, -1, 30, 25),
        event(smp::MechanicalInputType::ControlGroupAssign, 400, smp::ModifierCtrl, 1),
    });
    REQUIRE(edit.operation == smp::ArmyControlGroupOperation::Assign);
    REQUIRE(edit.selectionMethod == smp::ArmySelectionMethod::BoxSelect);
    REQUIRE(edit.group == 1);
    REQUIRE_NEAR(*edit.selectionDurationMs, 180.0, 0.001);
    REQUIRE_NEAR(*edit.selectionToOperationMs, 120.0, 0.001);
    REQUIRE_NEAR(*edit.totalExecutionMs, 300.0, 0.001);
}

TEST_CASE("army control-group Ctrl-click assign is a physical type selection") {
    const auto edit = singleEdit({
        event(smp::MechanicalInputType::MouseLeftDown, 100, smp::ModifierCtrl),
        event(smp::MechanicalInputType::MouseLeftUp, 120, smp::ModifierCtrl),
        event(smp::MechanicalInputType::ControlGroupAssign, 200, smp::ModifierCtrl, 1),
    });
    REQUIRE(edit.selectionMethod == smp::ArmySelectionMethod::CtrlClickType);
    REQUIRE(edit.operation == smp::ArmyControlGroupOperation::Assign);
}

TEST_CASE("army control-group genuine double click spans two complete click cycles") {
    const auto edit = singleEdit({
        event(smp::MechanicalInputType::MouseLeftDown, 100),
        event(smp::MechanicalInputType::MouseLeftUp, 120),
        event(smp::MechanicalInputType::MouseLeftDown, 250, smp::ModifierNone, -1, 102, 101),
        event(smp::MechanicalInputType::MouseLeftUp, 270, smp::ModifierNone, -1, 102, 101),
        event(smp::MechanicalInputType::ControlGroupAssign, 400, smp::ModifierCtrl, 1),
    });
    REQUIRE(edit.selectionMethod == smp::ArmySelectionMethod::DoubleClickType);
    REQUIRE_NEAR(*edit.selectionDurationMs, 170.0, 0.001);
    REQUIRE_NEAR(*edit.totalExecutionMs, 300.0, 0.001);
}

TEST_CASE("selection-changing input prevents a false rapid-click double click") {
    const auto edit = singleEdit({
        event(smp::MechanicalInputType::MouseLeftDown, 100),
        event(smp::MechanicalInputType::MouseLeftUp, 120),
        event(smp::MechanicalInputType::ControlGroupSelect, 160, smp::ModifierNone, 2),
        event(smp::MechanicalInputType::MouseLeftDown, 200),
        event(smp::MechanicalInputType::MouseLeftUp, 220),
        event(smp::MechanicalInputType::ControlGroupAssign, 300, smp::ModifierCtrl, 1),
    });
    REQUIRE(edit.selectionMethod == smp::ArmySelectionMethod::DirectClick);
}

TEST_CASE("army control-group box and Ctrl-click additions remain separate from assigns") {
    auto box = singleEdit({
        event(smp::MechanicalInputType::MouseLeftDown, 100, smp::ModifierNone, -1, 10, 10),
        event(smp::MechanicalInputType::MouseLeftUp, 180, smp::ModifierNone, -1, 30, 30),
        event(smp::MechanicalInputType::ControlGroupAdd, 250, smp::ModifierShift, 1),
    });
    REQUIRE(box.selectionMethod == smp::ArmySelectionMethod::BoxSelect);
    REQUIRE(box.operation == smp::ArmyControlGroupOperation::Add);

    auto ctrl = singleEdit({
        event(smp::MechanicalInputType::MouseLeftDown, 100, smp::ModifierCtrl),
        event(smp::MechanicalInputType::MouseLeftUp, 120, smp::ModifierCtrl),
        event(smp::MechanicalInputType::ControlGroupAdd, 200, smp::ModifierShift, 2),
    });
    REQUIRE(ctrl.selectionMethod == smp::ArmySelectionMethod::CtrlClickType);
    REQUIRE(ctrl.operation == smp::ArmyControlGroupOperation::Add);
}

TEST_CASE("army control-group Shift and Ctrl-Shift modifications are distinct") {
    const auto shift = singleEdit({
        event(smp::MechanicalInputType::ControlGroupSelect, 50, smp::ModifierNone, 1),
        event(smp::MechanicalInputType::MouseLeftDown, 100, smp::ModifierShift),
        event(smp::MechanicalInputType::MouseLeftUp, 120, smp::ModifierShift),
        event(smp::MechanicalInputType::ControlGroupAssign, 200, smp::ModifierCtrl, 1),
    });
    REQUIRE(shift.selectionMethod == smp::ArmySelectionMethod::ShiftClickModify);

    const auto ctrlShift = singleEdit({
        event(smp::MechanicalInputType::MouseLeftDown, 100,
              smp::ModifierCtrl | smp::ModifierShift),
        event(smp::MechanicalInputType::MouseLeftUp, 120,
              smp::ModifierCtrl | smp::ModifierShift),
        event(smp::MechanicalInputType::ControlGroupAssign, 200, smp::ModifierCtrl, 1),
    });
    REQUIRE(ctrlShift.selectionMethod == smp::ArmySelectionMethod::CtrlShiftClickType);
}

TEST_CASE("army control-group attribution expires instead of inventing a selection method") {
    const auto edit = singleEdit({
        event(smp::MechanicalInputType::MouseLeftDown, 100),
        event(smp::MechanicalInputType::MouseLeftUp, 120),
        event(smp::MechanicalInputType::ControlGroupAssign, 2201, smp::ModifierCtrl, 1),
    });
    REQUIRE(edit.selectionMethod == smp::ArmySelectionMethod::ExistingSelection);
    REQUIRE(!edit.selectionToOperationMs.has_value());
}

TEST_CASE("replay confirms army composition without replacing physical timing or method") {
    const auto live = correlatedLive(smp::ArmyControlGroupOperation::Assign);
    const auto replay = correlatedReplay("Dragoon", smp::ArmyControlGroupOperation::Assign, false);
    smp::MacroHotkeyProfile hotkeys;
    auto analysis = smp::correlateProductionVisitsWithReplay(
        live, hotkeys, qpcFrequency, correlatedBase(live), replay, "test");
    const auto& controlGroups = analysis.armyControlGroupManagement;
    REQUIRE(controlGroups.available);
    REQUIRE(controlGroups.assignments == 1);
    REQUIRE(controlGroups.edits[0].replayConfirmed);
    REQUIRE(controlGroups.edits[0].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(controlGroups.edits[0].selectionMethod == smp::ArmySelectionMethod::CtrlClickType);
    REQUIRE(controlGroups.edits[0].selectedUnitCount == 1);
    REQUIRE(controlGroups.edits[0].selectedUnitTypes[0] == "Dragoon");
    REQUIRE_NEAR(*controlGroups.edits[0].selectionToOperationMs, 130.0, 0.001);
}

TEST_CASE("known production-building bindings are excluded from army group statistics") {
    const auto live = correlatedLive(smp::ArmyControlGroupOperation::Assign);
    const auto replay = correlatedReplay("Gateway", smp::ArmyControlGroupOperation::Assign, true);
    smp::MacroHotkeyProfile hotkeys;
    auto analysis = smp::correlateProductionVisitsWithReplay(
        live, hotkeys, qpcFrequency, correlatedBase(live), replay, "test");
    const auto& controlGroups = analysis.armyControlGroupManagement;
    REQUIRE(controlGroups.assignments == 0);
    REQUIRE(controlGroups.excludedProductionBuildingEdits == 1);
    REQUIRE(controlGroups.edits[0].scope == smp::ArmyControlGroupScope::ProductionBuilding);
}

TEST_CASE("automatic session pools assign and add method timing across games") {
    auto makeProduction = [](smp::ArmyControlGroupOperation operation, double latency) {
        smp::ProductionAnalysis production;
        production.armyControlGroupManagement.available = true;
        smp::ArmyControlGroupEdit edit;
        edit.operation = operation;
        edit.group = 1;
        edit.selectionMethod = smp::ArmySelectionMethod::BoxSelect;
        edit.scope = smp::ArmyControlGroupScope::Army;
        edit.selectionToOperationMs = latency;
        edit.selectionDurationMs = 100.0;
        edit.totalExecutionMs = latency + 100.0;
        production.armyControlGroupManagement.edits.push_back(edit);
        smp::rebuildArmyControlGroupStatistics(production.armyControlGroupManagement);
        return production;
    };
    smp::AnalysisResult first;
    first.activeDurationSeconds = 60.0;
    smp::AnalysisResult second = first;
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, first,
                                     makeProduction(smp::ArmyControlGroupOperation::Assign, 100.0)));
    REQUIRE(session.addFinalizedGame(2, second,
                                     makeProduction(smp::ArmyControlGroupOperation::Add, 300.0)));
    const auto& pooled = session.stats().armyControlGroups;
    REQUIRE(pooled.assignments == 1);
    REQUIRE(pooled.additions == 1);
    REQUIRE_NEAR(pooled.editsPerMinute(), 1.0, 0.001);
    REQUIRE_NEAR(*pooled.assignmentMethods[smp::armySelectionMethodIndex(
                     smp::ArmySelectionMethod::BoxSelect)].averageSelectionToOperationMs,
                 100.0, 0.001);
    REQUIRE_NEAR(*pooled.additionMethods[smp::armySelectionMethodIndex(
                     smp::ArmySelectionMethod::BoxSelect)].averageSelectionToOperationMs,
                 300.0, 0.001);
}

TEST_CASE("army control-group JSON keeps operations methods timing composition and per-group counts") {
    smp::AnalysisResult result;
    result.activeDurationSeconds = 60.0;
    smp::ProductionAnalysis production;
    production.armyControlGroupManagement.available = true;
    production.armyControlGroupManagement.activeDurationSeconds = 60.0;
    smp::ArmyControlGroupEdit edit;
    edit.operationQpc = 400;
    edit.operationActiveMs = 400.0;
    edit.group = 2;
    edit.operation = smp::ArmyControlGroupOperation::Add;
    edit.selectionMethod = smp::ArmySelectionMethod::BoxSelect;
    edit.selectionStartQpc = 100;
    edit.selectionCompleteQpc = 280;
    edit.selectionDurationMs = 180.0;
    edit.selectionToOperationMs = 120.0;
    edit.totalExecutionMs = 300.0;
    edit.selectedUnitCount = 2;
    edit.selectedUnitTags = {11, 12};
    edit.selectedUnitTypes = {"Dragoon"};
    edit.replayConfirmed = true;
    edit.bindingConfidence = smp::ArmyControlGroupBindingConfidence::ReplayConfirmed;
    edit.scope = smp::ArmyControlGroupScope::Army;
    production.armyControlGroupManagement.edits.push_back(edit);
    smp::rebuildArmyControlGroupStatistics(production.armyControlGroupManagement);
    smp::MacroHotkeyProfile hotkeys;
    const auto encoded = smp::analysisToJson(result, "test", production, hotkeys);
    const auto& army = encoded["army_control_group_management"];
    REQUIRE(army["assignments"].asInt() == 0);
    REQUIRE(army["additions"].asInt() == 1);
    REQUIRE_NEAR(army["additions_per_minute"].asNumber(), 1.0, 0.001);
    REQUIRE(army["addition_methods"]["box_select"]["edit_count"].asInt() == 1);
    REQUIRE_NEAR(army["addition_methods"]["box_select"]
                         ["median_selection_to_operation_ms"].asNumber(),
                 120.0, 0.001);
    REQUIRE(army["by_group"]["2"]["additions"].asInt() == 1);
    REQUIRE(army["edits"].asArray().size() == 1);
    const auto& encodedEdit = army["edits"].asArray().front();
    REQUIRE(encodedEdit["operation"].asString() == "add");
    REQUIRE(encodedEdit["selected_unit_count"].asInt() == 2);
    REQUIRE(encodedEdit["scope"].asString() == "army");
}

TEST_CASE("automatic report includes separate army assignment and addition methods with timing") {
    smp::AnalysisResult result;
    result.activeDurationSeconds = 60.0;
    smp::ProductionAnalysis production;
    production.armyControlGroupManagement.available = true;
    smp::ArmyControlGroupEdit assign;
    assign.operation = smp::ArmyControlGroupOperation::Assign;
    assign.group = 1;
    assign.selectionMethod = smp::ArmySelectionMethod::BoxSelect;
    assign.selectionToOperationMs = 100.0;
    assign.selectionDurationMs = 200.0;
    assign.totalExecutionMs = 300.0;
    assign.scope = smp::ArmyControlGroupScope::Army;
    auto add = assign;
    add.operation = smp::ArmyControlGroupOperation::Add;
    add.selectionMethod = smp::ArmySelectionMethod::CtrlClickType;
    production.armyControlGroupManagement.edits = {assign, add};
    smp::rebuildArmyControlGroupStatistics(production.armyControlGroupManagement);
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, result, production));
    const auto report = smp::formatAutomaticSessionReport(session);
    REQUIRE(report.find("ARMY CONTROL-GROUP MANAGEMENT") != std::string::npos);
    REQUIRE(report.find("Assignments") != std::string::npos);
    REQUIRE(report.find("Additions") != std::string::npos);
    REQUIRE(report.find("ASSIGNMENT SELECTION METHOD") != std::string::npos);
    REQUIRE(report.find("ADDITION SELECTION METHOD") != std::string::npos);
    REQUIRE(report.find("Box select timing") != std::string::npos);
    REQUIRE(report.find("Ctrl-click type timing") != std::string::npos);
}
