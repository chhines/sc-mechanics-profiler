#include "test_framework.h"

#include "analysis/ability_activity.h"
#include "analysis/replay_analysis.h"
#include "app/game_analysis_visualization_model.h"
#include "storage/session.h"

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t testQpcFrequency = 1000;

smp::AbilityCommandCandidate abilityCandidate(
    double activeMs, std::string ability, std::size_t commandIndex = 0) {
    return {static_cast<std::int64_t>(activeMs / 42.0), commandIndex,
            activeMs, std::move(ability)};
}

void requireAbility(std::string_view kind, std::string_view order,
                    std::string_view expected) {
    const auto ability = smp::abilityCommandName(kind, order);
    REQUIRE(ability.has_value());
    REQUIRE(*ability == expected);
}

smp::MechanicalInputEvent controlGroupSelect(std::uint64_t activeMs,
                                             int group) {
    return {activeMs, static_cast<double>(activeMs),
            smp::MechanicalInputType::ControlGroupSelect, 0, 0,
            smp::ModifierNone, group, 100, 100};
}

} // namespace

TEST_CASE("targeted ability commands map to canonical display names") {
    requireAbility("Targeted Order", "CastPsionicStorm", "Psionic Storm");
    requireAbility("Targeted Order", "CastRecall", "Recall");
    requireAbility("Targeted Order", "CastPlague", "Plague");
    requireAbility("Targeted Order", "CastEMPShockwave", "EMP Shockwave");
    const auto analysis = smp::analyzeAbilityActivity(
        {
            abilityCandidate(1000.0, "Psionic Storm", 0),
            abilityCandidate(2000.0, "Recall", 1),
            abilityCandidate(3000.0, "Plague", 2),
            abilityCandidate(4000.0, "EMP Shockwave", 3),
        },
        60.0);
    REQUIRE(analysis.totalUses() == 4);
}

TEST_CASE("direct screp ability commands map to canonical display names") {
    requireAbility("Stim", "", "Stim");
    requireAbility("Siege", "", "Siege");
    requireAbility("Unsiege", "", "Unsiege");
    requireAbility("Burrow", "", "Burrow");
    requireAbility("Cloack", "", "Cloak");
    requireAbility("Decloack", "", "Decloak");
}

TEST_CASE("Scanner Sweep is retained only as Ability Activity") {
    const std::string fixture = R"json({
      "Header":{"Frames":100,"Players":[{"ID":0,"Name":"P"}]},
      "Commands":{"Cmds":[
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Targeted Order"},
         "Order":{"Name":"CastScannerSweep"},"Pos":{"X":12,"Y":34}}
      ]}}
    )json";

    const auto replay = smp::parseScrepReplayJson(fixture);
    REQUIRE(replay.abilityCommands.size() == 1);
    REQUIRE(replay.abilityCommands[0].ability == "Scanner Sweep");
    REQUIRE(replay.abilityCommands[0].rawKind == "Targeted Order");
    REQUIRE(replay.abilityCommands[0].rawOrder == "CastScannerSweep");
    REQUIRE(replay.unitCommands.empty());
}

TEST_CASE("ordinary army and production commands are not abilities") {
    const std::vector<std::pair<std::string_view, std::string_view>> commands{
        {"Right Click", ""},
        {"Targeted Order", "Move"},
        {"Targeted Order", "AttackMove"},
        {"Targeted Order", "Patrol"},
        {"Targeted Order", "CarrierAttack"},
        {"Targeted Order", "ReaverAttack"},
        {"Targeted Order", "EnterTransport"},
        {"Targeted Order", "SuicideUnit"},
        {"Targeted Order", "SuicideLocation"},
        {"Stop", ""},
        {"Hold Position", ""},
        {"Unload", ""},
        {"Train", ""},
        {"Build", ""},
        {"Unit Morph", ""},
        {"Upgrade", ""},
        {"Tech", ""},
        {"Targeted Order", "RallyPointTile"},
    };
    for (const auto& [kind, order] : commands)
        REQUIRE(!smp::abilityCommandName(kind, order).has_value());
}

TEST_CASE("one Stim packet counts once regardless of selected-unit count") {
    const std::string fixture = R"json({
      "Header":{"Frames":100,"Players":[{"ID":0,"Name":"P"}]},
      "Commands":{"Cmds":[
        {"Frame":9,"PlayerID":0,"Type":{"Name":"Select"},
         "UnitTags":[1,2,3,4,5,6,7,8,9,10,11,12]},
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Stim"}}
      ]}}
    )json";

    const auto replay = smp::parseScrepReplayJson(fixture);
    REQUIRE(replay.abilityCommands.size() == 1);
    REQUIRE(replay.abilityCommands[0].ability == "Stim");
}

TEST_CASE("Ability Activity totals and rates use active game minutes") {
    const auto analysis = smp::analyzeAbilityActivity(
        {
            abilityCandidate(10000.0, "Psionic Storm", 0),
            abilityCandidate(20000.0, "Psionic Storm", 1),
            abilityCandidate(30000.0, "Recall", 2),
            abilityCandidate(40000.0, "Stasis Field", 3),
            abilityCandidate(121000.0, "Plague", 4),
        },
        120.0);

    REQUIRE(analysis.available);
    REQUIRE(analysis.totalUses() == 4);
    REQUIRE_NEAR(*analysis.abilitiesPerMinute(), 2.0, 0.001);
    REQUIRE_NEAR(*analysis.usesPerMinute("Psionic Storm"), 1.0, 0.001);
    REQUIRE_NEAR(*analysis.usesPerMinute("Recall"), 0.5, 0.001);
    REQUIRE_NEAR(*analysis.usesPerMinute("Stasis Field"), 0.5, 0.001);
    const auto statistics = analysis.statistics();
    REQUIRE(statistics.size() == 3);
    REQUIRE(statistics[0].ability == "Psionic Storm");
    REQUIRE(statistics[0].uses == 2);
    REQUIRE(statistics[1].ability == "Recall");
    REQUIRE(statistics[2].ability == "Stasis Field");
}

TEST_CASE("zero Ability Activity remains available without fabricated rows") {
    const auto analysis = smp::analyzeAbilityActivity({}, 60.0);
    REQUIRE(analysis.available);
    REQUIRE(analysis.totalUses() == 0);
    REQUIRE_NEAR(*analysis.abilitiesPerMinute(), 0.0, 0.001);
    REQUIRE(analysis.statistics().empty());

    const auto zeroDuration = smp::analyzeAbilityActivity({}, 0.0);
    REQUIRE(zeroDuration.available);
    REQUIRE(!zeroDuration.abilitiesPerMinute().has_value());
}

TEST_CASE("Ability Activity counts only the identified replay player") {
    smp::AnalysisResult live;
    live.activeDurationSeconds = 130.0;
    live.mechanicalEvents = {
        controlGroupSelect(120000, 1),
        controlGroupSelect(121000, 2),
    };
    smp::ProductionAnalysis base;
    base.visitsAvailable = true;
    base.armyControlGroupManagement =
        smp::detectArmyControlGroupManagement(live, testQpcFrequency);

    smp::ReplayData replay;
    replay.totalFrames = 3100;
    replay.players = {{0, "player"}, {1, "opponent"}};
    replay.controlGroupSelections = {{2857, 0, 1, 0}, {2881, 0, 2, 1}};
    replay.abilityCommands = {
        {2890, 0, 2, "Psionic Storm", "Targeted Order",
         "CastPsionicStorm"},
        {2891, 1, 3, "Plague", "Targeted Order", "CastPlague"},
    };

    smp::MacroHotkeyProfile hotkeys;
    const auto correlated = smp::correlateProductionVisitsWithReplay(
        live, hotkeys, testQpcFrequency, std::move(base), replay, "test");
    REQUIRE(correlated.abilityActivity.available);
    REQUIRE(correlated.abilityActivity.totalUses() == 1);
    REQUIRE(correlated.abilityActivity.observations[0].ability ==
            "Psionic Storm");
}

TEST_CASE("Ability Activity survives derived JSON and visualization loading") {
    smp::AnalysisResult live;
    live.activeDurationSeconds = 120.0;
    smp::ProductionAnalysis production;
    production.abilityActivity = smp::analyzeAbilityActivity(
        {
            abilityCandidate(10000.0, "Psionic Storm", 0),
            abilityCandidate(20000.0, "Psionic Storm", 1),
            abilityCandidate(30000.0, "Recall", 2),
            abilityCandidate(40000.0, "Stasis Field", 3),
        },
        live.activeDurationSeconds);
    smp::MacroHotkeyProfile hotkeys;
    auto encoded =
        smp::analysisToJson(live, "ability-test", production, hotkeys);

    const auto& abilities = encoded["ability_activity"];
    REQUIRE(abilities["available"].asBool(false));
    REQUIRE(abilities["total_uses"].asInt() == 4);
    REQUIRE_NEAR(abilities["abilities_per_minute"].asNumber(), 2.0, 0.001);
    REQUIRE(abilities["by_ability"].asObject().size() == 3);
    REQUIRE(abilities["by_ability"]["Psionic Storm"]["uses"].asInt() == 2);
    REQUIRE_NEAR(
        abilities["by_ability"]["Psionic Storm"]["uses_per_minute"]
            .asNumber(),
        1.0, 0.001);

    const auto model =
        smp::buildGameAnalysisVisualizationModel(nullptr, &encoded);
    REQUIRE(model.abilityActivityStatus.available);
    REQUIRE(model.totalAbilityUses.has_value());
    REQUIRE(*model.totalAbilityUses == 4);
    REQUIRE_NEAR(*model.abilitiesPerMinute, 2.0, 0.001);
    REQUIRE(model.abilityActivityBreakdown.size() == 3);
    REQUIRE(model.abilityActivityBreakdown[0].ability == "Psionic Storm");
    REQUIRE(model.abilityActivityBreakdown[0].uses == 2);
    REQUIRE(model.abilityActivityBreakdown[1].ability == "Recall");
    REQUIRE(model.abilityActivityBreakdown[2].ability == "Stasis Field");

    encoded.asObject().erase("ability_activity");
    const auto legacy =
        smp::buildGameAnalysisVisualizationModel(nullptr, &encoded);
    REQUIRE(!legacy.abilityActivityStatus.available);
    REQUIRE(!legacy.totalAbilityUses.has_value());
    REQUIRE(!legacy.abilitiesPerMinute.has_value());
    REQUIRE(legacy.abilityActivityBreakdown.empty());
}
