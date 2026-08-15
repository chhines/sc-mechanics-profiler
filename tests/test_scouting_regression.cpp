#include "test_framework.h"

#include "analysis/army_control_group.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t qpcFrequency = 1000;

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

smp::ScoutingUnitTravelEvidence travel(std::size_t assignmentEditIndex,
                                       double progress) {
    return {assignmentEditIndex, 0.0, 0.0, 100.0, 0.0,
            progress * 100.0, 0.0};
}

smp::ArmyControlGroupAnalysis analyze(
    std::vector<smp::ArmyControlGroupEdit> edits,
    std::vector<smp::ScoutingUnitTravelEvidence> evidence = {}) {
    smp::ArmyControlGroupAnalysis analysis;
    analysis.available = true;
    analysis.edits = std::move(edits);
    smp::rebuildArmyControlGroupStatistics(analysis);
    smp::applyScoutingUnitClassification(analysis, evidence);
    smp::analyzeScoutingUnitActivity(analysis, {}, qpcFrequency);
    return analysis;
}

} // namespace

TEST_CASE("scouting regression: worker scout with strong travel is excluded from army stats") {
    const auto analysis = analyze({edit(35000.0, 2, 1001, "Probe")},
                                  {travel(0, 0.75)});

    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(analysis.assignments == 0);
    REQUIRE(analysis.excludedScoutingUnitEdits == 1);
    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
}

TEST_CASE("scouting regression: local builder remains uncertain rather than becoming army") {
    const auto analysis = analyze({edit(35000.0, 2, 1001, "Probe")},
                                  {travel(0, 0.10)});

    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(analysis.assignments == 0);
    REQUIRE(analysis.uncertainEdits == 1);
    REQUIRE(analysis.excludedScoutingUnitEdits == 0);
}

TEST_CASE("scouting regression: repeated assignment of same scout is one generation") {
    const auto analysis = analyze({edit(35000.0, 2, 1001, "Probe"),
                                   edit(35200.0, 2, 1001, "Probe"),
                                   edit(35400.0, 2, 1001, "Probe")},
                                  {travel(2, 0.75)});

    REQUIRE(analysis.excludedScoutingUnitEdits == 3);
    REQUIRE(analysis.scoutingUnitActivities.size() == 1);
    REQUIRE(analysis.scoutingUnitActivities[0].assignmentGeneration == 1);
    REQUIRE(analysis.scoutingUnitActivities[0].assignedQpc == 35000);
}

TEST_CASE("scouting regression: overwriting scout group with army starts army generation") {
    const auto analysis = analyze({edit(35000.0, 2, 1001, "Probe"),
                                   edit(70000.0, 2, 2001, "Zealot")},
                                  {travel(0, 0.75)});

    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(analysis.edits[1].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(analysis.excludedScoutingUnitEdits == 1);
    REQUIRE(analysis.assignments == 1);
}

TEST_CASE("scouting regression: early combat singleton cannot become scout from travel") {
    const auto analysis = analyze({edit(35000.0, 2, 2001, "Zealot")},
                                  {travel(0, 0.90)});

    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(analysis.assignments == 1);
    REQUIRE(analysis.excludedScoutingUnitEdits == 0);
}

TEST_CASE("scouting regression: unknown singleton needs strong travel to become scout") {
    const auto strong = analyze({edit(35000.0, 2, 1001, "")},
                                {travel(0, 0.75)});
    REQUIRE(strong.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
    REQUIRE(strong.excludedScoutingUnitEdits == 1);

    const auto local = analyze({edit(35000.0, 2, 1001, "")},
                               {travel(0, 0.10)});
    REQUIRE(local.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(local.uncertainEdits == 1);
}

TEST_CASE("scouting regression: shift add terminates scout candidacy") {
    const auto analysis = analyze({edit(35000.0, 2, 1001, "Probe"),
                                   edit(36000.0, 2, 2001, "Zealot",
                                        smp::ArmyControlGroupOperation::Add)},
                                  {travel(0, 0.90)});

    REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::Uncertain);
    REQUIRE(analysis.edits[1].scope == smp::ArmyControlGroupScope::Army);
    REQUIRE(analysis.excludedScoutingUnitEdits == 0);
}

TEST_CASE("scouting regression: all three race workers are eligible scout candidates") {
    for (const std::string worker : {"Probe", "SCV", "Drone"}) {
        const auto analysis = analyze({edit(35000.0, 2, 1001, worker)},
                                      {travel(0, 0.75)});
        REQUIRE(analysis.edits[0].scope == smp::ArmyControlGroupScope::ScoutingUnit);
        REQUIRE(analysis.excludedScoutingUnitEdits == 1);
    }
}
