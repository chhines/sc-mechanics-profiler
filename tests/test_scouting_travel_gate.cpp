#include "test_framework.h"

#include "analysis/scouting_travel_gate.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t qpcFrequency = 1000;

smp::ArmyControlGroupEdit assignment(double activeMs, std::uint32_t unitTag = 1001,
                                      std::string worker = "Probe") {
    smp::ArmyControlGroupEdit edit;
    edit.operationActiveMs = activeMs;
    edit.operationQpc = static_cast<std::uint64_t>(activeMs);
    edit.group = 2;
    edit.operation = smp::ArmyControlGroupOperation::Assign;
    edit.selectionMethod = smp::ArmySelectionMethod::ExistingSelection;
    edit.scope = smp::ArmyControlGroupScope::Army;
    edit.replayConfirmed = true;
    edit.bindingConfidence = smp::ArmyControlGroupBindingConfidence::ReplayConfirmed;
    edit.selectedUnitTags = {unitTag};
    edit.selectedUnitTypes = {std::move(worker)};
    edit.selectedUnitCount = 1;
    return edit;
}

smp::ScoutingUnitCommandEvidence command(double activeMs, double targetX, double targetY,
                                         double ownX, double ownY,
                                         double enemyX, double enemyY,
                                         std::uint32_t unitTag = 1001) {
    return {0, unitTag, ownX, ownY, enemyX, enemyY,
            targetX, targetY, activeMs};
}

smp::ArmyControlGroupAnalysis analyze(
    double assignedMs,
    std::vector<smp::ScoutingUnitCommandEvidence> evidence) {
    smp::ArmyControlGroupAnalysis result;
    result.available = true;
    result.edits = {assignment(assignedMs)};
    smp::rebuildArmyControlGroupStatistics(result);
    smp::applyTravelGatedScoutingUnitClassification(result, evidence);
    smp::AnalysisResult live;
    smp::analyzeTravelGatedScoutingUnitActivity(result, live, qpcFrequency);
    return result;
}

} // namespace

TEST_CASE("travel-gated scouting rejects an impossible immediate cross-map scout") {
    constexpr double ownX = 288.0;
    constexpr double ownY = 272.0;
    constexpr double enemyX = 3808.0;
    constexpr double enemyY = 3824.0;
    const auto result = analyze(
        44809.6411,
        {
            command(45281.0232, 3659.0, 3972.0, ownX, ownY, enemyX, enemyY),
            command(45487.9246, 3666.0, 3964.0, ownX, ownY, enemyX, enemyY),
            command(45694.8261, 3667.0, 3963.0, ownX, ownY, enemyX, enemyY),
            command(48102.9462, 629.0, 383.0, ownX, ownY, enemyX, enemyY),
        });

    REQUIRE(result.scoutingUnitCandidateCount == 1);
    REQUIRE(result.unconfirmedScoutingUnitCandidateCount == 1);
    REQUIRE(result.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(result.scoutingUnitActivities.empty());
}

TEST_CASE("travel-gated scouting confirms later enemy-side evidence after plausible travel") {
    constexpr double ownX = 288.0;
    constexpr double ownY = 272.0;
    constexpr double enemyX = 3808.0;
    constexpr double enemyY = 3824.0;
    const auto result = analyze(
        92463.5630,
        {
            command(93035.2694, 3600.0, 3900.0, ownX, ownY, enemyX, enemyY),
            command(93245.4330, 3650.0, 3900.0, ownX, ownY, enemyX, enemyY),
            command(105190.7764, 3680.0, 3880.0, ownX, ownY, enemyX, enemyY),
            command(105359.9269, 3700.0, 3860.0, ownX, ownY, enemyX, enemyY),
        });

    REQUIRE(result.scoutingUnitCandidateCount == 1);
    REQUIRE(result.unconfirmedScoutingUnitCandidateCount == 0);
    REQUIRE(result.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(result.scoutingUnitActivities.size() == 1);
    REQUIRE(result.scoutingUnitActivities[0].commandCount == 2);
    REQUIRE_NEAR(*result.scoutingUnitActivities[0].firstCommandActiveMs,
                 105190.7764, 0.001);
}

TEST_CASE("travel-gated scouting resets the travel clock after an aborted home order") {
    constexpr double ownX = 0.0;
    constexpr double ownY = 0.0;
    constexpr double enemyX = 3200.0;
    constexpr double enemyY = 0.0;
    const auto result = analyze(
        35000.0,
        {
            command(36000.0, 3000.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(37000.0, 100.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(40000.0, 3000.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(43000.0, 3000.0, 0.0, ownX, ownY, enemyX, enemyY),
        });

    REQUIRE(result.unconfirmedScoutingUnitCandidateCount == 1);
    REQUIRE(result.scoutingUnitActivities.empty());
}

TEST_CASE("travel-gated scouting does not treat the return order as arrival home") {
    constexpr double ownX = 0.0;
    constexpr double ownY = 0.0;
    constexpr double enemyX = 3200.0;
    constexpr double enemyY = 0.0;
    const auto result = analyze(
        35000.0,
        {
            command(42000.0, 2600.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(50000.0, 2700.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(52000.0, 100.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(54000.0, 120.0, 0.0, ownX, ownY, enemyX, enemyY),
        });

    REQUIRE(result.scoutingUnitActivities.size() == 1);
    REQUIRE(!result.scoutingUnitActivities[0].returnedHome);
    REQUIRE_NEAR(*result.scoutingUnitActivities[0].lastCommandActiveMs,
                 54000.0, 0.001);
}

TEST_CASE("travel-gated scouting confirms return only after plausible return travel") {
    constexpr double ownX = 0.0;
    constexpr double ownY = 0.0;
    constexpr double enemyX = 3200.0;
    constexpr double enemyY = 0.0;
    const auto result = analyze(
        35000.0,
        {
            command(42000.0, 2600.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(50000.0, 2700.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(52000.0, 100.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(58000.0, 120.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(62000.0, 80.0, 0.0, ownX, ownY, enemyX, enemyY),
        });

    REQUIRE(result.scoutingUnitActivities.size() == 1);
    REQUIRE(result.scoutingUnitActivities[0].returnedHome);
    REQUIRE_NEAR(*result.scoutingUnitActivities[0].lastCommandActiveMs,
                 58000.0, 0.001);
    REQUIRE(result.scoutingUnitActivities[0].commandCount == 4);
}

TEST_CASE("travel-gated scouting records a resumed excursion after a return order") {
    constexpr double ownX = 0.0;
    constexpr double ownY = 0.0;
    constexpr double enemyX = 3200.0;
    constexpr double enemyY = 0.0;
    const auto result = analyze(
        35000.0,
        {
            command(42000.0, 2600.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(50000.0, 2700.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(52000.0, 100.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(53000.0, 2800.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(60000.0, 100.0, 0.0, ownX, ownY, enemyX, enemyY),
            command(66000.0, 100.0, 0.0, ownX, ownY, enemyX, enemyY),
        });

    REQUIRE(result.scoutingUnitActivities.size() == 1);
    REQUIRE(result.scoutingUnitActivities[0].resumedAfterTemporaryReturn);
    REQUIRE(result.scoutingUnitActivities[0].returnedHome);
}
