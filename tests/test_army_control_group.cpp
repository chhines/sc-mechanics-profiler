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

smp::MechanicalInputEvent eventAt(smp::MechanicalInputType type, std::uint64_t qpc,
                                  double activeMs, int value = -1) {
    return {qpc, activeMs, type, 0, 0, smp::ModifierNone, value, 100, 100};
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
    live.activeDurationSeconds = 130.0;
    live.mechanicalEvents = {
        event(smp::MechanicalInputType::ControlGroupSelect, 120000, smp::ModifierNone, 1),
        event(smp::MechanicalInputType::ControlGroupSelect, 121000, smp::ModifierNone, 2),
        event(smp::MechanicalInputType::MouseLeftDown, 121450, smp::ModifierCtrl, -1, 300, 300),
        event(smp::MechanicalInputType::MouseLeftUp, 121470, smp::ModifierCtrl, -1, 300, 300),
        event(operation == smp::ArmyControlGroupOperation::Assign
                  ? smp::MechanicalInputType::ControlGroupAssign
                  : smp::MechanicalInputType::ControlGroupAdd,
              121600,
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
    replay.totalFrames = 3100;
    replay.players = {{0, "player"}};
    replay.controlGroupSelections = {{2857, 0, 1, 0}, {2881, 0, 2, 1}};
    smp::ReplaySelectionEvent selection{
        2892, 0, smp::ReplaySelectionKind::Select, {9001}, 2};
    if (!unitType.empty())
        selection.unitTypes.push_back(std::move(unitType));
    replay.selections.push_back(std::move(selection));
    replay.controlGroupEdits.push_back({2895, 0, 5, operation, 3});
    if (productionBuilding) {
        replay.productionEvents.push_back(
            {2896, 0, smp::ReplayProductionKind::Train, "Probe", 0x40, 4});
    }
    return replay;
}

smp::ArmyControlGroupEdit scopedEdit(double activeMs, int group,
                                     smp::ArmyControlGroupOperation operation,
                                     smp::ArmyControlGroupScope scope =
                                         smp::ArmyControlGroupScope::Army) {
    smp::ArmyControlGroupEdit edit;
    edit.operationActiveMs = activeMs;
    edit.operationQpc = static_cast<std::uint64_t>(activeMs);
    edit.group = group;
    edit.operation = operation;
    edit.selectionMethod = smp::ArmySelectionMethod::ExistingSelection;
    edit.scope = scope;
    edit.replayConfirmed = true;
    edit.bindingConfidence = smp::ArmyControlGroupBindingConfidence::ReplayConfirmed;
    return edit;
}

smp::ArmyControlGroupEdit workerEdit(
    double activeMs, int group, std::uint32_t unitTag,
    smp::ArmyControlGroupOperation operation =
        smp::ArmyControlGroupOperation::Assign) {
    auto edit = scopedEdit(activeMs, group, operation);
    edit.selectedUnitTags = {unitTag};
    edit.selectedUnitTypes = {"Probe"};
    edit.selectedUnitCount = 1;
    return edit;
}

smp::ScoutingUnitTravelEvidence scoutTravel(std::size_t assignmentEditIndex,
                                             double progress) {
    return {assignmentEditIndex, 0.0, 0.0, 100.0, 0.0,
            progress * 100.0, 0.0};
}

smp::ArmyControlGroupAnalysis scoutingAnalysis(
    std::vector<smp::ArmyControlGroupEdit> edits) {
    smp::ArmyControlGroupAnalysis analysis;
    analysis.available = true;
    analysis.edits = std::move(edits);
    smp::rebuildArmyControlGroupStatistics(analysis);
    return analysis;
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

TEST_CASE("two identical early worker assignments are one scouting candidate") {
    auto analysis = scoutingAnalysis({workerEdit(40000.0, 1, 11436),
                                      workerEdit(40500.0, 1, 11436)});
    smp::applyScoutingUnitClassification(analysis, {scoutTravel(0, 0.75)});
    smp::analyzeScoutingUnitActivity(analysis, {}, qpcFrequency);
    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(analysis.edits[1].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(analysis.excludedScoutingUnitEdits == 2);
    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    REQUIRE(analysis.scoutingUnitActivities[0].assignedQpc == 40000);
    REQUIRE(analysis.scoutingUnitActivities[0].assignmentGeneration == 1);
}

TEST_CASE("three identical early worker assignments are one scouting candidate") {
    auto analysis = scoutingAnalysis({workerEdit(40000.0, 2, 11436),
                                      workerEdit(40100.0, 2, 11436),
                                      workerEdit(40200.0, 2, 11436)});
    smp::applyScoutingUnitClassification(analysis, {scoutTravel(2, 0.75)});
    smp::analyzeScoutingUnitActivity(analysis, {}, qpcFrequency);
    REQUIRE(analysis.excludedScoutingUnitEdits == 3);
    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    REQUIRE(analysis.scoutingUnitActivities[0].assignedQpc == 40000);
}

TEST_CASE("early builder group without scout travel is uncertain instead of army") {
    auto analysis = scoutingAnalysis({workerEdit(40000.0, 1, 11447)});
    smp::applyScoutingUnitClassification(analysis, {scoutTravel(0, 0.10)});
    smp::analyzeScoutingUnitActivity(analysis, {}, qpcFrequency);
    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(analysis.uncertainEdits == 1);
    REQUIRE(analysis.assignments == 0);
    REQUIRE(analysis.excludedScoutingUnitEdits == 0);
    REQUIRE(analysis.scoutingUnitActivities.empty());
}

TEST_CASE("early singleton worker is promoted by attributable scout travel") {
    auto analysis = scoutingAnalysis({workerEdit(40000.0, 2, 11436)});
    smp::applyScoutingUnitClassification(analysis, {scoutTravel(0, 0.75)});
    smp::analyzeScoutingUnitActivity(analysis, {}, qpcFrequency);
    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
}

TEST_CASE("travel just below half-map progress does not confirm a scout") {
    auto analysis = scoutingAnalysis({workerEdit(40000.0, 2, 11436)});
    smp::applyScoutingUnitClassification(analysis, {scoutTravel(0, 0.49999)});
    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(analysis.assignments == 0);
}

TEST_CASE("travel at exactly half-map progress confirms a scout") {
    auto analysis = scoutingAnalysis({workerEdit(40000.0, 2, 11436)});
    smp::applyScoutingUnitClassification(analysis, {scoutTravel(0, 0.5)});
    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
}

TEST_CASE("far travel does not turn an authoritative non-worker singleton into a scout") {
    auto nonWorker = workerEdit(40000.0, 2, 11436);
    nonWorker.selectedUnitTypes = {"Zealot"};
    auto analysis = scoutingAnalysis({nonWorker});
    smp::applyScoutingUnitClassification(analysis, {scoutTravel(0, 0.75)});
    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(analysis.assignments == 1);
    REQUIRE(analysis.excludedScoutingUnitEdits == 0);
}

TEST_CASE("unknown singleton type is provisionally confirmed by far travel") {
    auto unknown = workerEdit(40000.0, 2, 11436);
    unknown.selectedUnitTypes.clear();
    auto analysis = scoutingAnalysis({unknown});
    smp::applyScoutingUnitClassification(analysis, {scoutTravel(0, 0.75)});
    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(analysis.assignments == 0);
    REQUIRE(analysis.excludedScoutingUnitEdits == 1);
}

TEST_CASE("unknown singleton type with local travel is uncertain instead of army") {
    auto unknown = workerEdit(40000.0, 2, 11436);
    unknown.selectedUnitTypes.clear();
    auto analysis = scoutingAnalysis({unknown});
    smp::applyScoutingUnitClassification(analysis, {scoutTravel(0, 0.10)});
    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(analysis.uncertainEdits == 1);
    REQUIRE(analysis.assignments == 0);
    REQUIRE(analysis.excludedScoutingUnitEdits == 0);
}

TEST_CASE("builder candidate may later be promoted by attributable scout travel") {
    auto analysis = scoutingAnalysis({workerEdit(40000.0, 2, 11436)});
    smp::applyScoutingUnitClassification(
        analysis, {scoutTravel(0, 0.10), scoutTravel(0, 0.80)});
    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
}

TEST_CASE("true worker overwrite starts a new scouting candidate") {
    auto analysis = scoutingAnalysis({workerEdit(40000.0, 2, 11436),
                                      workerEdit(50000.0, 2, 11447)});
    smp::applyScoutingUnitClassification(
        analysis, {scoutTravel(0, 0.75), scoutTravel(1, 0.80)});
    smp::analyzeScoutingUnitActivity(analysis, {}, qpcFrequency);
    REQUIRE(analysis.scoutingUnitActivities.size() == 2);
    REQUIRE(analysis.scoutingUnitActivities[0].assignmentGeneration == 1);
    REQUIRE(analysis.scoutingUnitActivities[1].assignmentGeneration == 2);
}

TEST_CASE("Add cancels an early scouting candidate even after far travel") {
    auto analysis = scoutingAnalysis(
        {workerEdit(40000.0, 2, 11436),
         workerEdit(80000.0, 2, 11436, smp::ArmyControlGroupOperation::Add)});
    smp::applyScoutingUnitClassification(analysis, {scoutTravel(0, 0.75)});
    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(analysis.edits[1].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(analysis.uncertainEdits == 1);
    REQUIRE(analysis.assignments == 0);
    REQUIRE(analysis.additions == 1);
}

TEST_CASE("missing or unattributable position evidence fails closed") {
    auto analysis = scoutingAnalysis({workerEdit(40000.0, 2, 11436)});
    smp::applyScoutingUnitClassification(analysis, {scoutTravel(1, 0.75)});
    smp::analyzeScoutingUnitActivity(analysis, {}, qpcFrequency);
    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(analysis.assignments == 0);
    REQUIRE(analysis.scoutingUnitActivities.empty());
}

TEST_CASE("replay correlation detects one no-Build scout beside a local builder") {
    smp::AnalysisResult live;
    live.activeDurationSeconds = 10.0;
    live.mechanicalEvents = {
        event(smp::MechanicalInputType::ControlGroupSelect, 0,
              smp::ModifierNone, 4),
        event(smp::MechanicalInputType::ControlGroupSelect, 1000,
              smp::ModifierNone, 5),
        event(smp::MechanicalInputType::ControlGroupAssign, 2000,
              smp::ModifierCtrl, 1),
        event(smp::MechanicalInputType::ControlGroupAssign, 2167,
              smp::ModifierCtrl, 1),
        event(smp::MechanicalInputType::ControlGroupAssign, 3000,
              smp::ModifierCtrl, 1),
        event(smp::MechanicalInputType::ControlGroupAssign, 3500,
              smp::ModifierCtrl, 2),
        event(smp::MechanicalInputType::ControlGroupAssign, 3667,
              smp::ModifierCtrl, 2),
    };

    smp::ReplayData replay;
    replay.totalFrames = 240;
    replay.mapWidthPixels = 200.0;
    replay.mapHeightPixels = 200.0;
    replay.players = {{0, "player", 1, "Protoss"}};
    replay.startLocations = {{1, 0.0, 0.0}};
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
        {56, 0, 20.0, 20.0, 5},
        {76, 0, 90.0, 90.0, 9},
        {92, 0, 60.0, 60.0, 13},
    };
    replay.buildEvents = {{60, 0, 6}};

    smp::MacroHotkeyProfile hotkeys;
    const auto correlated = smp::correlateProductionVisitsWithReplay(
        live, hotkeys, qpcFrequency, correlatedBase(live), replay, "test");
    const auto& groups = correlated.armyControlGroupManagement;
    REQUIRE(correlated.replayCorrelation.available);
    REQUIRE(groups.edits.size() == 5);
    REQUIRE(groups.edits[0].selectedUnitTypes.size() == 1);
    REQUIRE(groups.edits[0].selectedUnitTypes[0] == "Probe");
    REQUIRE(groups.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(groups.edits[1].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(groups.edits[2].selectedUnitTypes.size() == 1);
    REQUIRE(groups.edits[2].selectedUnitTypes[0] == "Zealot");
    REQUIRE(groups.edits[2].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(groups.edits[3].selectedUnitTypes.empty());
    REQUIRE(groups.edits[3].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(groups.edits[4].selectedUnitTypes.empty());
    REQUIRE(groups.edits[4].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(groups.assignments == 1);
    REQUIRE(groups.uncertainEdits == 2);
    REQUIRE(groups.excludedScoutingUnitEdits == 2);
    REQUIRE(groups.scoutingUnitActivities.size() == 1);
    REQUIRE(groups.scoutingUnitActivities[0].group == 2);
    REQUIRE(groups.scoutingUnitActivities[0].assignmentGeneration == 1);
    REQUIRE(groups.scoutingUnitActivities[0].assignedQpc == 3500);
}

TEST_CASE("assignment after two minutes remains an army group") {
    smp::ArmyControlGroupAnalysis analysis;
    analysis.available = true;
    analysis.edits = {
        scopedEdit(120000.0, 2, smp::ArmyControlGroupOperation::Assign),
        scopedEdit(121000.0, 2, smp::ArmyControlGroupOperation::Assign),
    };
    smp::applyScoutingUnitClassification(analysis);
    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(analysis.edits[1].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(analysis.assignments == 2);
}

TEST_CASE("production-building evidence takes precedence over early scouting heuristic") {
    smp::ArmyControlGroupAnalysis analysis;
    analysis.available = true;
    analysis.edits = {
        scopedEdit(50000.0, 5, smp::ArmyControlGroupOperation::Assign,
                   smp::ArmyControlGroupScope::ProductionBuilding),
    };
    smp::applyScoutingUnitClassification(analysis);
    REQUIRE(analysis.edits[0].scope ==
            smp::ArmyControlGroupScope::ProductionBuilding);
    REQUIRE(analysis.excludedProductionBuildingEdits == 1);
    REQUIRE(analysis.excludedScoutingUnitEdits == 0);
}

TEST_CASE("normal scouting usage measures assignment through the last physical command") {
    auto analysis = scoutingAnalysis({scopedEdit(
        40000.0, 1, smp::ArmyControlGroupOperation::Assign,
        smp::ArmyControlGroupScope::ScoutingUnit)});
    smp::AnalysisResult result;
    result.mechanicalEvents = {
        event(smp::MechanicalInputType::ControlGroupSelect, 55000,
              smp::ModifierNone, 1),
        event(smp::MechanicalInputType::MouseRightDown, 57000),
        event(smp::MechanicalInputType::MouseRightUp, 57020),
        event(smp::MechanicalInputType::ControlGroupSelect, 70000,
              smp::ModifierNone, 1),
        event(smp::MechanicalInputType::MouseRightDown, 72000),
        event(smp::MechanicalInputType::ControlGroupSelect, 125000,
              smp::ModifierNone, 1),
        event(smp::MechanicalInputType::MouseRightDown, 127000),
    };
    smp::analyzeScoutingUnitActivity(analysis, result, qpcFrequency);
    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    const auto& scout = analysis.scoutingUnitActivities.front();
    REQUIRE(scout.group == 1);
    REQUIRE(scout.assignmentGeneration == 1);
    REQUIRE(scout.selectionCount == 3);
    REQUIRE(scout.commandCount == 3);
    REQUIRE(*scout.firstCommandQpc == 57000);
    REQUIRE(*scout.lastCommandQpc == 127000);
    REQUIRE_NEAR(*scout.scoutingActivityDurationMs, 87000.0, 0.001);
    REQUIRE_NEAR(*scout.assignmentToLastCommandMs, 87000.0, 0.001);
    REQUIRE_NEAR(*scout.assignmentToLastSelectionMs, 85000.0, 0.001);
    REQUIRE_NEAR(*scout.firstToLastCommandMs, 70000.0, 0.001);
}

TEST_CASE("scouting selections without commands leave activity duration unavailable") {
    auto analysis = scoutingAnalysis({scopedEdit(
        40000.0, 1, smp::ArmyControlGroupOperation::Assign,
        smp::ArmyControlGroupScope::ScoutingUnit)});
    smp::AnalysisResult result;
    result.mechanicalEvents = {
        event(smp::MechanicalInputType::ControlGroupSelect, 60000,
              smp::ModifierNone, 1),
        event(smp::MechanicalInputType::ControlGroupSelect, 80000,
              smp::ModifierNone, 1),
        event(smp::MechanicalInputType::ControlGroupSelect, 100000,
              smp::ModifierNone, 1),
    };
    smp::analyzeScoutingUnitActivity(analysis, result, qpcFrequency);
    const auto& scout = analysis.scoutingUnitActivities.front();
    REQUIRE(scout.selectionCount == 3);
    REQUIRE(scout.commandCount == 0);
    REQUIRE(*scout.lastSelectionQpc == 100000);
    REQUIRE_NEAR(*scout.assignmentToLastSelectionMs, 60000.0, 0.001);
    REQUIRE(!scout.scoutingActivityDurationMs.has_value());
    REQUIRE(!scout.lastCommandQpc.has_value());
}

TEST_CASE("multiple right-click commands remain active after one scout selection") {
    auto analysis = scoutingAnalysis({scopedEdit(
        40000.0, 1, smp::ArmyControlGroupOperation::Assign,
        smp::ArmyControlGroupScope::ScoutingUnit)});
    smp::AnalysisResult result;
    result.mechanicalEvents = {
        event(smp::MechanicalInputType::ControlGroupSelect, 50000,
              smp::ModifierNone, 1),
        event(smp::MechanicalInputType::MouseRightDown, 52000),
        event(smp::MechanicalInputType::MouseRightDown, 54000),
        event(smp::MechanicalInputType::MouseRightDown, 58000),
    };
    smp::analyzeScoutingUnitActivity(analysis, result, qpcFrequency);
    const auto& scout = analysis.scoutingUnitActivities.front();
    REQUIRE(scout.selectionCount == 1);
    REQUIRE(scout.commandCount == 3);
    REQUIRE(*scout.lastCommandQpc == 58000);
    REQUIRE_NEAR(*scout.scoutingActivityDurationMs, 18000.0, 0.001);
}

TEST_CASE("another control group interrupts scout context but location recall does not") {
    auto analysis = scoutingAnalysis({scopedEdit(
        40000.0, 1, smp::ArmyControlGroupOperation::Assign,
        smp::ArmyControlGroupScope::ScoutingUnit)});
    smp::AnalysisResult result;
    result.mechanicalEvents = {
        event(smp::MechanicalInputType::ControlGroupSelect, 50000,
              smp::ModifierNone, 1),
        event(smp::MechanicalInputType::LocationRecall, 51000,
              smp::ModifierNone, 2),
        event(smp::MechanicalInputType::MouseRightDown, 52000),
        event(smp::MechanicalInputType::ControlGroupSelect, 53000,
              smp::ModifierNone, 2),
        event(smp::MechanicalInputType::MouseRightDown, 54000),
    };
    smp::analyzeScoutingUnitActivity(analysis, result, qpcFrequency);
    const auto& scout = analysis.scoutingUnitActivities.front();
    REQUIRE(scout.commandCount == 1);
    REQUIRE(*scout.lastCommandQpc == 52000);
}

TEST_CASE("direct or box selection interrupts scout command attribution") {
    auto analysis = scoutingAnalysis({scopedEdit(
        40000.0, 1, smp::ArmyControlGroupOperation::Assign,
        smp::ArmyControlGroupScope::ScoutingUnit)});
    smp::AnalysisResult result;
    result.mechanicalEvents = {
        event(smp::MechanicalInputType::ControlGroupSelect, 50000,
              smp::ModifierNone, 1),
        event(smp::MechanicalInputType::MouseRightDown, 51000),
        event(smp::MechanicalInputType::MouseLeftDown, 52000),
        event(smp::MechanicalInputType::MouseLeftUp, 52200),
        event(smp::MechanicalInputType::MouseRightDown, 53000),
    };
    smp::analyzeScoutingUnitActivity(analysis, result, qpcFrequency);
    const auto& scout = analysis.scoutingUnitActivities.front();
    REQUIRE(scout.commandCount == 1);
    REQUIRE(*scout.lastCommandQpc == 51000);
}

TEST_CASE("control-group overwrite ends the scouting assignment generation") {
    auto analysis = scoutingAnalysis({
        scopedEdit(40000.0, 1, smp::ArmyControlGroupOperation::Assign,
                   smp::ArmyControlGroupScope::ScoutingUnit),
        scopedEdit(180000.0, 1, smp::ArmyControlGroupOperation::Assign,
                   smp::ArmyControlGroupScope::Army),
    });
    smp::AnalysisResult result;
    result.mechanicalEvents = {
        event(smp::MechanicalInputType::ControlGroupSelect, 70000,
              smp::ModifierNone, 1),
        event(smp::MechanicalInputType::MouseRightDown, 72000),
        event(smp::MechanicalInputType::ControlGroupSelect, 190000,
              smp::ModifierNone, 1),
        event(smp::MechanicalInputType::MouseRightDown, 192000),
    };
    smp::analyzeScoutingUnitActivity(analysis, result, qpcFrequency);
    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    const auto& scout = analysis.scoutingUnitActivities.front();
    REQUIRE(scout.commandCount == 1);
    REQUIRE(*scout.lastCommandQpc == 72000);
    REQUIRE_NEAR(*scout.scoutingActivityDurationMs, 32000.0, 0.001);
}

TEST_CASE("multiple scouting generations and groups retain independent activity records") {
    auto analysis = scoutingAnalysis({
        scopedEdit(40000.0, 1, smp::ArmyControlGroupOperation::Assign,
                   smp::ArmyControlGroupScope::ScoutingUnit),
        scopedEdit(45000.0, 2, smp::ArmyControlGroupOperation::Assign,
                   smp::ArmyControlGroupScope::ScoutingUnit),
        scopedEdit(80000.0, 1, smp::ArmyControlGroupOperation::Assign,
                   smp::ArmyControlGroupScope::ScoutingUnit),
    });
    smp::AnalysisResult result;
    result.mechanicalEvents = {
        event(smp::MechanicalInputType::ControlGroupSelect, 50000,
              smp::ModifierNone, 1),
        event(smp::MechanicalInputType::MouseRightDown, 52000),
        event(smp::MechanicalInputType::ControlGroupSelect, 60000,
              smp::ModifierNone, 2),
        event(smp::MechanicalInputType::MouseRightDown, 63000),
        event(smp::MechanicalInputType::MouseRightDown, 65000),
        event(smp::MechanicalInputType::ControlGroupSelect, 90000,
              smp::ModifierNone, 1),
        event(smp::MechanicalInputType::MouseRightDown, 92000),
    };
    smp::analyzeScoutingUnitActivity(analysis, result, qpcFrequency);
    REQUIRE(analysis.scoutingUnitActivities.size() == 3);
    REQUIRE(analysis.scoutingUnitActivities[0].group == 1);
    REQUIRE(analysis.scoutingUnitActivities[0].assignmentGeneration == 1);
    REQUIRE(analysis.scoutingUnitActivities[0].commandCount == 1);
    REQUIRE(analysis.scoutingUnitActivities[1].group == 2);
    REQUIRE(analysis.scoutingUnitActivities[1].commandCount == 2);
    REQUIRE(analysis.scoutingUnitActivities[2].group == 1);
    REQUIRE(analysis.scoutingUnitActivities[2].assignmentGeneration == 2);
    REQUIRE(analysis.scoutingUnitActivities[2].commandCount == 1);
    REQUIRE(*analysis.scoutingUnitActivities[2].lastCommandQpc == 92000);
}

TEST_CASE("scouting activity duration uses QPC instead of active or replay time") {
    auto scoutEdit = scopedEdit(40000.0, 1,
                                smp::ArmyControlGroupOperation::Assign,
                                smp::ArmyControlGroupScope::ScoutingUnit);
    scoutEdit.operationQpc = 100000;
    auto analysis = scoutingAnalysis({scoutEdit});
    smp::AnalysisResult result;
    result.mechanicalEvents = {
        eventAt(smp::MechanicalInputType::ControlGroupSelect, 110000, 55000.0, 1),
        eventAt(smp::MechanicalInputType::MouseRightDown, 187000, 999999.0),
    };
    smp::analyzeScoutingUnitActivity(analysis, result, qpcFrequency);
    const auto& scout = analysis.scoutingUnitActivities.front();
    REQUIRE_NEAR(*scout.scoutingActivityDurationMs, 87000.0, 0.001);
    REQUIRE_NEAR(*scout.lastCommandActiveMs, 999999.0, 0.001);
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

TEST_CASE("missing replay unit types do not make a valid post-cutoff edit uncertain") {
    const auto live = correlatedLive(smp::ArmyControlGroupOperation::Assign);
    const auto replay = correlatedReplay("", smp::ArmyControlGroupOperation::Assign, false);
    smp::MacroHotkeyProfile hotkeys;
    const auto analysis = smp::correlateProductionVisitsWithReplay(
        live, hotkeys, qpcFrequency, correlatedBase(live), replay, "test");
    const auto& edit = analysis.armyControlGroupManagement.edits.front();
    REQUIRE(edit.scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(edit.selectedUnitTypes.empty());
    REQUIRE(analysis.armyControlGroupManagement.assignments == 1);
    REQUIRE(analysis.armyControlGroupManagement.uncertainEdits == 0);
}

TEST_CASE("ambiguous replay edit matching remains uncertain") {
    const auto live = correlatedLive(smp::ArmyControlGroupOperation::Assign);
    auto replay = correlatedReplay("", smp::ArmyControlGroupOperation::Assign, false);
    replay.controlGroupEdits.push_back(
        {2895, 0, 5, smp::ArmyControlGroupOperation::Assign, 4});
    smp::MacroHotkeyProfile hotkeys;
    const auto analysis = smp::correlateProductionVisitsWithReplay(
        live, hotkeys, qpcFrequency, correlatedBase(live), replay, "test");
    const auto& edit = analysis.armyControlGroupManagement.edits.front();
    REQUIRE(edit.scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(edit.bindingConfidence ==
            smp::ArmyControlGroupBindingConfidence::Ambiguous);
    REQUIRE(!edit.replayConfirmed);
    REQUIRE(analysis.armyControlGroupManagement.uncertainEdits == 1);
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
    production.armyControlGroupManagement.edits.push_back(
        scopedEdit(40000.0, 1, smp::ArmyControlGroupOperation::Assign,
                   smp::ArmyControlGroupScope::ScoutingUnit));
    smp::ScoutingUnitActivity scoutActivity;
    scoutActivity.group = 1;
    scoutActivity.assignmentGeneration = 1;
    scoutActivity.assignedQpc = 40000;
    scoutActivity.assignedActiveMs = 40000.0;
    scoutActivity.firstSelectionQpc = 55000;
    scoutActivity.lastSelectionQpc = 125000;
    scoutActivity.firstSelectionActiveMs = 55000.0;
    scoutActivity.lastSelectionActiveMs = 125000.0;
    scoutActivity.firstCommandQpc = 57000;
    scoutActivity.lastCommandQpc = 127000;
    scoutActivity.firstCommandActiveMs = 57000.0;
    scoutActivity.lastCommandActiveMs = 127000.0;
    scoutActivity.selectionCount = 3;
    scoutActivity.commandCount = 3;
    scoutActivity.assignmentToLastSelectionMs = 85000.0;
    scoutActivity.assignmentToLastCommandMs = 87000.0;
    scoutActivity.scoutingActivityDurationMs = 87000.0;
    scoutActivity.firstToLastCommandMs = 70000.0;
    production.armyControlGroupManagement.scoutingUnitActivities.push_back(
        scoutActivity);
    smp::rebuildArmyControlGroupStatistics(production.armyControlGroupManagement);
    smp::MacroHotkeyProfile hotkeys;
    const auto encoded = smp::analysisToJson(result, "test", production, hotkeys);
    const auto& army = encoded["army_control_group_management"];
    REQUIRE(army["assignments"].asInt() == 0);
    REQUIRE(army["additions"].asInt() == 1);
    REQUIRE(army["excluded_scouting_unit_edits"].asInt() == 1);
    REQUIRE(army["scouting_unit_detected"].asBool());
    REQUIRE(army["scouting_unit_count"].asInt() == 1);
    REQUIRE(army["scouting_selection_count"].asInt() == 3);
    REQUIRE(army["scouting_command_count"].asInt() == 3);
    REQUIRE_NEAR(army["average_scouting_activity_duration_ms"].asNumber(),
                 87000.0, 0.001);
    const auto& scout = army["scouting_unit_activity"].asArray().front();
    REQUIRE(scout["group"].asInt() == 1);
    REQUIRE(scout["assignment_generation"].asInt() == 1);
    REQUIRE(scout["selection_count"].asInt() == 3);
    REQUIRE(scout["command_count"].asInt() == 3);
    REQUIRE_NEAR(scout["activity_duration_ms"].asNumber(), 87000.0, 0.001);
    REQUIRE_NEAR(scout["assignment_to_last_selection_ms"].asNumber(),
                 85000.0, 0.001);
    REQUIRE_NEAR(scout["first_to_last_command_ms"].asNumber(),
                 70000.0, 0.001);
    REQUIRE_NEAR(army["additions_per_minute"].asNumber(), 1.0, 0.001);
    REQUIRE(army["addition_methods"]["box_select"]["edit_count"].asInt() == 1);
    REQUIRE_NEAR(army["addition_methods"]["box_select"]
                         ["median_selection_to_operation_ms"].asNumber(),
                 120.0, 0.001);
    REQUIRE(army["by_group"]["2"]["additions"].asInt() == 1);
    REQUIRE(army["edits"].asArray().size() == 2);
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
    auto scout = assign;
    scout.scope = smp::ArmyControlGroupScope::ScoutingUnit;
    auto productionGroup = assign;
    productionGroup.scope = smp::ArmyControlGroupScope::ProductionBuilding;
    production.armyControlGroupManagement.edits = {assign, add, scout, productionGroup};
    smp::ScoutingUnitActivity scoutActivity;
    scoutActivity.group = 1;
    scoutActivity.assignmentGeneration = 1;
    scoutActivity.assignedActiveMs = 40000.0;
    scoutActivity.scoutingActivityDurationMs = 87000.0;
    scoutActivity.lastCommandActiveMs = 127000.0;
    scoutActivity.selectionCount = 3;
    scoutActivity.commandCount = 3;
    production.armyControlGroupManagement.scoutingUnitActivities.push_back(
        scoutActivity);
    smp::rebuildArmyControlGroupStatistics(production.armyControlGroupManagement);
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, result, production));
    REQUIRE(session.stats().armyControlGroups.excludedScoutingUnitEdits == 1);
    REQUIRE(session.stats().armyControlGroups.excludedProductionBuildingEdits == 1);
    REQUIRE_NEAR(session.stats().armyControlGroups.editsPerMinute(), 2.0,
                 0.001);
    const auto report = smp::formatAutomaticSessionReport(session);
    REQUIRE(report.find("ARMY CONTROL-GROUP MANAGEMENT") != std::string::npos);
    REQUIRE(report.find("Assignments") != std::string::npos);
    REQUIRE(report.find("Additions") != std::string::npos);
    REQUIRE(report.find("Scouting unit excluded") != std::string::npos);
    REQUIRE(report.find("SCOUTING UNIT ACTIVITY") != std::string::npos);
    REQUIRE(report.find("Activity duration") != std::string::npos);
    REQUIRE(report.find("87.0 s") != std::string::npos);
    REQUIRE(report.find("Last commanded") != std::string::npos);
    REQUIRE(report.find("Production groups excluded") != std::string::npos);
    REQUIRE(report.find("ASSIGNMENT SELECTION METHOD") != std::string::npos);
    REQUIRE(report.find("ADDITION SELECTION METHOD") != std::string::npos);
    REQUIRE(report.find("Box select timing") != std::string::npos);
    REQUIRE(report.find("Ctrl-click type timing") != std::string::npos);
}
