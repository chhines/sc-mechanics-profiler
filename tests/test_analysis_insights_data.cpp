#include "test_framework.h"

#include "analysis/army_control_group.h"
#include "analysis/production_visit.h"
#include "app/game_analysis_visualization_model.h"
#include "storage/session.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t qpcFrequency = 1000;
constexpr double ownSpawnX = 0.0;
constexpr double ownSpawnY = 0.0;
constexpr double enemySpawnX = 3200.0;
constexpr double enemySpawnY = 0.0;

smp::ArmyControlGroupEdit scoutAssignment(double activeMs = 35000.0,
                                           std::uint32_t unitTag = 1001) {
    smp::ArmyControlGroupEdit edit;
    edit.operationActiveMs = activeMs;
    edit.operationQpc = static_cast<std::uint64_t>(activeMs);
    edit.group = 2;
    edit.operation = smp::ArmyControlGroupOperation::Assign;
    edit.selectionMethod = smp::ArmySelectionMethod::ExistingSelection;
    edit.scope = smp::ArmyControlGroupScope::Army;
    edit.replayConfirmed = true;
    edit.bindingConfidence =
        smp::ArmyControlGroupBindingConfidence::ReplayConfirmed;
    edit.selectedUnitTags = {unitTag};
    edit.selectedUnitTypes = {"Probe"};
    edit.selectedUnitCount = 1;
    return edit;
}

smp::ScoutingUnitCommandEvidence command(double activeMs, double targetX,
                                         std::uint32_t unitTag = 1001) {
    return {0, unitTag, ownSpawnX, ownSpawnY, enemySpawnX, enemySpawnY,
            targetX, 0.0, activeMs};
}

smp::ArmyControlGroupAnalysis analyzeScout(
    std::vector<smp::ScoutingUnitCommandEvidence> evidence) {
    smp::ArmyControlGroupAnalysis analysis;
    analysis.available = true;
    analysis.edits = {scoutAssignment()};
    smp::rebuildArmyControlGroupStatistics(analysis);
    smp::applyScoutingUnitClassification(analysis, evidence);
    smp::AnalysisResult live;
    smp::analyzeScoutingUnitActivity(analysis, live, qpcFrequency);
    return analysis;
}

} // namespace

TEST_CASE("scouting insights record command gaps and final return home") {
    const auto analysis = analyzeScout({
        command(50000.0, 2300.0),
        command(60000.0, 2700.0),
        command(76000.0, 100.0),
    });

    REQUIRE(analysis.scoutingUnitCandidateCount == 1);
    REQUIRE(analysis.unconfirmedScoutingUnitCandidateCount == 0);
    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    const auto& scout = analysis.scoutingUnitActivities.front();
    REQUIRE(scout.outcomeAvailable);
    REQUIRE(scout.returnedHome);
    REQUIRE(!scout.resumedAfterTemporaryReturn);
    REQUIRE(scout.commandActiveMs.size() == 3);
    REQUIRE_NEAR(*scout.longestCommandGapMs, 16000.0, 0.001);
}

TEST_CASE("scouting insights distinguish temporary return from final outcome") {
    const auto analysis = analyzeScout({
        command(50000.0, 2300.0),
        command(60000.0, 100.0),
        command(70000.0, 2600.0),
        command(90000.0, 120.0),
    });

    const auto& scout = analysis.scoutingUnitActivities.front();
    REQUIRE(scout.returnedHome);
    REQUIRE(scout.resumedAfterTemporaryReturn);
    REQUIRE_NEAR(*scout.longestCommandGapMs, 20000.0, 0.001);
}

TEST_CASE("scouting insights use no observed return instead of death inference") {
    const auto analysis = analyzeScout({
        command(50000.0, 2300.0),
        command(70000.0, 2600.0),
        command(80000.0, 1000.0),
    });

    const auto& scout = analysis.scoutingUnitActivities.front();
    REQUIRE(scout.outcomeAvailable);
    REQUIRE(!scout.returnedHome);
    REQUIRE(!scout.resumedAfterTemporaryReturn);
}

TEST_CASE("scouting insights retain an unconfirmed early-worker candidate") {
    const auto analysis = analyzeScout({command(50000.0, 400.0)});
    REQUIRE(analysis.scoutingUnitCandidateCount == 1);
    REQUIRE(analysis.unconfirmedScoutingUnitCandidateCount == 1);
    REQUIRE(analysis.scoutingUnitActivities.empty());
}

TEST_CASE("scouting insight fields survive derived JSON visualization loading") {
    const auto controlGroups = analyzeScout({
        command(50000.0, 2300.0),
        command(60000.0, 100.0),
        command(70000.0, 2600.0),
        command(90000.0, 120.0),
    });

    smp::AnalysisResult live;
    live.activeDurationSeconds = 120.0;
    smp::ProductionAnalysis production;
    production.visitsAvailable = true;
    production.workerMacroCycles.available = true;
    production.workerMacroCycles.productType = smp::MacroProductType::Worker;
    production.armyMacroCycles.available = true;
    production.armyMacroCycles.productType = smp::MacroProductType::Army;
    production.armyControlGroupManagement = controlGroups;
    smp::MacroHotkeyProfile hotkeys;

    auto derived = smp::analysisToJson(live, "insight-test", production, hotkeys);
    const auto& groupJson = derived["army_control_group_management"];
    REQUIRE(groupJson["scouting_outcome_data_available"].asBool(false));
    REQUIRE(groupJson["scouting_candidate_count"].asInt() == 1);
    REQUIRE(groupJson["unconfirmed_scouting_candidate_count"].asInt() == 0);
    REQUIRE(groupJson["scouting_unit_activity"].isArray());
    REQUIRE(groupJson["scouting_unit_activity"].asArray().size() == 1);

    const auto model = smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE(model.scoutingOutcomeDataAvailable);
    REQUIRE(model.scoutingCandidateCount == 1);
    REQUIRE(model.unconfirmedScoutingCandidateCount == 0);
    REQUIRE(model.scoutingActivities.size() == 1);
    const auto& scout = model.scoutingActivities.front();
    REQUIRE(scout.unitTag == 1001);
    REQUIRE(scout.commandActiveMs.size() == 4);
    REQUIRE(scout.returnedHome);
    REQUIRE(scout.resumedAfterTemporaryReturn);
    REQUIRE_NEAR(*scout.longestCommandGapMs, 20000.0, 0.001);
}
