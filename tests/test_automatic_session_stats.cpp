#include "test_framework.h"

#include "analysis/ability_activity.h"
#include "cli/automatic_session_stats.h"
#include "cli/report.h"

#include <iostream>
#include <sstream>
#include <utility>

namespace {

smp::CameraNavigationEvent navigation(smp::CameraNavigationType type,
                                      smp::EdgeDirection direction = smp::EdgeDirection::None) {
    smp::CameraNavigationEvent event;
    event.type = type;
    event.edgeDirection = direction;
    return event;
}

void addEvents(smp::AnalysisResult& result, smp::CameraNavigationType type, int count) {
    for (int index = 0; index < count; ++index)
        result.navigationEvents.push_back(navigation(type));
}

smp::ProductMacroCycleAnalysis cycles(smp::MacroProductType type,
                                      std::initializer_list<double> durations,
                                      std::size_t productionVisits = 0) {
    std::vector<smp::MacroCycle> values;
    for (const double duration : durations) {
        smp::MacroCycle cycle;
        cycle.productType = type;
        cycle.durationMs = duration;
        values.push_back(cycle);
    }
    std::vector<smp::ProductionVisit> visits;
    for (std::size_t index = 0; index < productionVisits; ++index) {
        smp::ProductionVisit visit;
        visit.productType = type;
        visit.accessMethod = static_cast<smp::ProductionAccessMethod>(index % 4);
        visits.push_back(visit);
    }
    return smp::summarizeProductMacroCycles(type, std::move(values), visits);
}

smp::ProductMacroCycleAnalysis timedCycles(
    smp::MacroProductType type,
    std::initializer_list<std::pair<double, double>> timesSeconds) {
    std::vector<smp::MacroCycle> values;
    for (const auto& [startSeconds, endSeconds] : timesSeconds) {
        smp::MacroCycle cycle;
        cycle.productType = type;
        cycle.startActiveMs = startSeconds * 1000.0;
        cycle.endActiveMs = endSeconds * 1000.0;
        cycle.durationMs = (endSeconds - startSeconds) * 1000.0;
        values.push_back(cycle);
    }
    return smp::summarizeProductMacroCycles(type, std::move(values), {});
}

smp::ProductMacroCycleAnalysis repeatedCycles(smp::MacroProductType type,
                                               std::size_t count) {
    std::vector<smp::MacroCycle> values;
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        smp::MacroCycle cycle;
        cycle.productType = type;
        cycle.startActiveMs = static_cast<double>(index) * 2000.0;
        cycle.endActiveMs = cycle.startActiveMs + 1000.0;
        cycle.durationMs = 1000.0;
        values.push_back(cycle);
    }
    return smp::summarizeProductMacroCycles(type, std::move(values), {});
}

smp::ProductionAnalysis production(smp::ProductMacroCycleAnalysis worker,
                                   smp::ProductMacroCycleAnalysis army) {
    smp::ProductionAnalysis result;
    result.visitsAvailable = true;
    result.workerMacroCycles = std::move(worker);
    result.armyMacroCycles = std::move(army);
    return result;
}

smp::AbilityActivityAnalysis abilityActivity(double activeDurationSeconds,
                                              std::size_t uses) {
    std::vector<smp::AbilityCommandCandidate> candidates;
    candidates.reserve(uses);
    for (std::size_t index = 0; index < uses; ++index) {
        candidates.push_back(
            {static_cast<std::int64_t>(index), index,
             1000.0 + static_cast<double>(index), "Psionic Storm"});
    }
    return smp::analyzeAbilityActivity(std::move(candidates),
                                       activeDurationSeconds);
}

class CoutCapture {
  public:
    CoutCapture() : previous_(std::cout.rdbuf(output_.rdbuf())) {}
    ~CoutCapture() {
        std::cout.rdbuf(previous_);
    }
    [[nodiscard]] std::string str() const {
        return output_.str();
    }

  private:
    std::ostringstream output_;
    std::streambuf* previous_{};
};

} // namespace

TEST_CASE("one finalized automatic game becomes last game and the complete session aggregate") {
    smp::AnalysisResult game;
    game.activeDurationSeconds = 120.0;
    game.navigationEvents = {
        navigation(smp::CameraNavigationType::ControlGroupJump),
        navigation(smp::CameraNavigationType::LocationHotkey),
        navigation(smp::CameraNavigationType::MinimapJump),
        navigation(smp::CameraNavigationType::EdgeScroll, smp::EdgeDirection::Left),
    };

    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, game));
    REQUIRE(session.stats().games == 1);
    REQUIRE_NEAR(session.stats().activeSeconds, 120.0, 0.001);
    REQUIRE(session.stats().navigationTransitions() == 4);
    REQUIRE(session.stats().controlGroupJumps == 1);
    REQUIRE(session.stats().locationHotkeyJumps == 1);
    REQUIRE(session.stats().minimapJumps == 1);
    REQUIRE(session.stats().edgePans == 1);
    REQUIRE(session.stats().edgeLeft == 1);
    REQUIRE(session.lastGame().has_value());
}

TEST_CASE("automatic session rate is weighted by total active time") {
    smp::AnalysisResult gameA;
    gameA.activeDurationSeconds = 60.0;
    addEvents(gameA, smp::CameraNavigationType::ControlGroupJump, 10);
    smp::AnalysisResult gameB;
    gameB.activeDurationSeconds = 540.0;
    addEvents(gameB, smp::CameraNavigationType::MinimapJump, 90);

    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, gameA));
    REQUIRE(session.addFinalizedGame(2, gameB));
    REQUIRE(session.stats().games == 2);
    REQUIRE(session.stats().navigationTransitions() == 100);
    REQUIRE_NEAR(session.stats().activeSeconds, 600.0, 0.001);
    REQUIRE_NEAR(session.stats().navigationTransitionsPerMinute(), 10.0, 0.001);
}

TEST_CASE("automatic session distribution is derived from aggregate counts") {
    smp::AnalysisResult game;
    game.activeDurationSeconds = 600.0;
    addEvents(game, smp::CameraNavigationType::ControlGroupJump, 50);
    addEvents(game, smp::CameraNavigationType::LocationHotkey, 10);
    addEvents(game, smp::CameraNavigationType::MinimapJump, 30);
    addEvents(game, smp::CameraNavigationType::EdgeScroll, 10);
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, game));
    const auto& stats = session.stats();
    REQUIRE_NEAR(stats.methodPercentage(stats.controlGroupJumps), 50.0, 0.001);
    REQUIRE_NEAR(stats.methodPercentage(stats.locationHotkeyJumps), 10.0, 0.001);
    REQUIRE_NEAR(stats.methodPercentage(stats.minimapJumps), 30.0, 0.001);
    REQUIRE_NEAR(stats.methodPercentage(stats.edgePans), 10.0, 0.001);
}

TEST_CASE("automatic session ignores duplicate finalization for the same generation") {
    smp::AnalysisResult game;
    game.activeDurationSeconds = 60.0;
    addEvents(game, smp::CameraNavigationType::ControlGroupJump, 10);
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(7, game));
    REQUIRE(!session.addFinalizedGame(7, game));
    REQUIRE(session.stats().games == 1);
}

TEST_CASE("aborted automatic recording closes its generation without adding a game") {
    smp::AnalysisResult partial;
    partial.activeDurationSeconds = 1.0;
    smp::AutomaticSessionState session;
    REQUIRE(session.markAbortedGeneration(1));
    REQUIRE(session.empty());
    REQUIRE(session.stats().games == 0);
    REQUIRE(!session.addFinalizedGame(1, partial));
    REQUIRE(session.stats().workerMacro.gamesUnavailable == 0);
    REQUIRE(session.stats().armyMacro.gamesUnavailable == 0);
}

TEST_CASE("valid game followed by aborted recording leaves every aggregate unchanged") {
    smp::AnalysisResult completed;
    completed.activeDurationSeconds = 60.0;
    addEvents(completed, smp::CameraNavigationType::ControlGroupJump, 10);
    smp::ProductionAnalysis unavailable;
    unavailable.workerMacroCycles.productType = smp::MacroProductType::Worker;
    unavailable.workerMacroCycles.unavailableReason = "replay unavailable";
    unavailable.armyMacroCycles.productType = smp::MacroProductType::Army;
    unavailable.armyMacroCycles.unavailableReason = "replay unavailable";
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, completed, unavailable));
    const auto before = session.stats();

    REQUIRE(session.markAbortedGeneration(2));
    REQUIRE(session.stats().games == before.games);
    REQUIRE_NEAR(session.stats().activeSeconds, before.activeSeconds, 0.001);
    REQUIRE(session.stats().navigationTransitions() == before.navigationTransitions());
    REQUIRE(session.stats().workerMacro.gamesUnavailable == before.workerMacro.gamesUnavailable);
    REQUIRE(session.stats().armyMacro.gamesUnavailable == before.armyMacro.gamesUnavailable);
    REQUIRE(!session.addFinalizedGame(2, completed, unavailable));
}

TEST_CASE("completed automatic generation is counted exactly once when shutdown follows") {
    smp::AnalysisResult game;
    game.activeDurationSeconds = 60.0;
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(7, game));
    REQUIRE(!session.markAbortedGeneration(7));
    REQUIRE(!session.addFinalizedGame(7, game));
    REQUIRE(session.stats().games == 1);
}

TEST_CASE("stale completion events cannot revive completed or aborted generations") {
    smp::AnalysisResult game;
    game.activeDurationSeconds = 60.0;
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, game));
    REQUIRE(session.markAbortedGeneration(2));
    REQUIRE(!session.addFinalizedGame(1, game));
    REQUIRE(!session.addFinalizedGame(2, game));
    REQUIRE(session.stats().games == 1);
}

TEST_CASE("automatic session pools worker and army cycle durations independently across games") {
    smp::AnalysisResult gameA;
    smp::AnalysisResult gameB;
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(
        1, gameA, production(cycles(smp::MacroProductType::Worker, {700.0, 900.0}, 3),
                             cycles(smp::MacroProductType::Army, {1200.0}, 2))));
    REQUIRE(session.addFinalizedGame(
        2, gameB, production(cycles(smp::MacroProductType::Worker, {2000.0}, 1),
                             cycles(smp::MacroProductType::Army, {800.0, 1600.0}, 4))));
    const auto& stats = session.stats();
    REQUIRE(stats.workerMacro.gamesAnalyzed == 2);
    REQUIRE(stats.workerMacro.cycles == 3);
    REQUIRE(stats.workerMacro.productionVisits == 4);
    REQUIRE_NEAR(*stats.workerMacro.averageDurationMs(), 1200.0, 0.001);
    REQUIRE_NEAR(*stats.workerMacro.bestDurationMs, 700.0, 0.001);
    REQUIRE_NEAR(*stats.workerMacro.slowestDurationMs, 2000.0, 0.001);
    REQUIRE(stats.armyMacro.cycles == 3);
    REQUIRE_NEAR(*stats.armyMacro.averageDurationMs(), 1200.0, 0.001);
}

TEST_CASE("automatic session macro gaps use consecutive end-to-next-start timestamps") {
    smp::AnalysisResult game;
    const auto stats = smp::automaticSessionStatsForGame(
        game,
        production(timedCycles(smp::MacroProductType::Worker,
                               {{0.0, 4.0}, {12.0, 15.0}, {40.0, 43.0}}),
                   timedCycles(smp::MacroProductType::Army,
                               {{10.0, 12.0}})));

    REQUIRE(stats.workerMacro.gapDurationsMs.size() == 2);
    REQUIRE_NEAR(stats.workerMacro.gapDurationsMs[0], 8000.0, 0.001);
    REQUIRE_NEAR(stats.workerMacro.gapDurationsMs[1], 25000.0, 0.001);
    REQUIRE(stats.armyMacro.gapDurationsMs.empty());
    REQUIRE(!stats.armyMacro.medianGapMs().has_value());
    REQUIRE(!stats.armyMacro.p90GapMs().has_value());
}

TEST_CASE("automatic session macro gaps retain overlaps as zero-length observations") {
    smp::AnalysisResult game;
    const auto stats = smp::automaticSessionStatsForGame(
        game,
        production(timedCycles(smp::MacroProductType::Worker,
                               {{0.0, 12.0}, {10.0, 13.0}}),
                   timedCycles(smp::MacroProductType::Army, {})));

    REQUIRE(stats.workerMacro.gapDurationsMs.size() == 1);
    REQUIRE_NEAR(stats.workerMacro.gapDurationsMs[0], 0.0, 0.001);
    REQUIRE_NEAR(*stats.workerMacro.medianGapMs(), 0.0, 0.001);
    REQUIRE_NEAR(*stats.workerMacro.p90GapMs(), 0.0, 0.001);
}

TEST_CASE("automatic session macro gap percentiles pool observations across games") {
    smp::AnalysisResult game;
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(
        1, game,
        production(timedCycles(smp::MacroProductType::Worker,
                               {{0.0, 4.0}, {12.0, 15.0}, {40.0, 43.0}}),
                   timedCycles(smp::MacroProductType::Army, {}))));
    REQUIRE(session.addFinalizedGame(
        2, game,
        production(timedCycles(smp::MacroProductType::Worker,
                               {{0.0, 12.0}, {10.0, 13.0}}),
                   timedCycles(smp::MacroProductType::Army, {}))));

    const auto& worker = session.stats().workerMacro;
    REQUIRE(worker.gapDurationsMs.size() == 3);
    REQUIRE_NEAR(*worker.medianGapMs(), 8000.0, 0.001);
    REQUIRE_NEAR(*worker.p90GapMs(), 21600.0, 0.001);
}

TEST_CASE("automatic session macro cycles per minute uses pooled analyzed time") {
    smp::AnalysisResult gameA;
    gameA.activeDurationSeconds = 60.0;
    smp::AnalysisResult gameB;
    gameB.activeDurationSeconds = 240.0;
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(
        1, gameA,
        production(repeatedCycles(smp::MacroProductType::Worker, 10),
                   repeatedCycles(smp::MacroProductType::Army, 0))));
    REQUIRE(session.addFinalizedGame(
        2, gameB,
        production(repeatedCycles(smp::MacroProductType::Worker, 20),
                   repeatedCycles(smp::MacroProductType::Army, 0))));
    smp::AnalysisResult gameC;
    gameC.activeDurationSeconds = 60.0;
    REQUIRE(session.addFinalizedGame(
        3, gameC,
        production(repeatedCycles(smp::MacroProductType::Worker, 0),
                   repeatedCycles(smp::MacroProductType::Army, 0))));
    smp::AnalysisResult unavailableGame;
    unavailableGame.activeDurationSeconds = 300.0;
    smp::ProductionAnalysis unavailable;
    unavailable.workerMacroCycles.unavailableReason = "replay unavailable";
    unavailable.armyMacroCycles.unavailableReason = "replay unavailable";
    REQUIRE(session.addFinalizedGame(4, unavailableGame, unavailable));

    const auto& worker = session.stats().workerMacro;
    REQUIRE(worker.gamesAnalyzed == 3);
    REQUIRE(worker.gamesUnavailable == 1);
    REQUIRE_NEAR(worker.analyzedActiveSeconds, 360.0, 0.001);
    REQUIRE(worker.cycles == 30);
    REQUIRE_NEAR(*worker.cyclesPerMinute(), 5.0, 0.001);
}

TEST_CASE("automatic session macro gap counts use strict thresholds per analyzed game") {
    smp::ProductMacroSessionStats stats;
    stats.gamesAnalyzed = 2;
    stats.gapDurationsMs = {10000.0, 10000.1, 20000.0, 20000.1};
    REQUIRE_NEAR(*stats.gapsOverPerGame(10000.0), 1.5, 0.001);
    REQUIRE_NEAR(*stats.gapsOverPerGame(20000.0), 0.5, 0.001);
    REQUIRE_NEAR(*stats.longestGapMs(), 20000.1, 0.001);
}

TEST_CASE("automatic session Army command KPIs pool counts time and raw gaps") {
    smp::AnalysisResult gameA;
    gameA.activeDurationSeconds = 60.0;
    auto productionA = production(
        repeatedCycles(smp::MacroProductType::Worker, 0),
        repeatedCycles(smp::MacroProductType::Army, 0));
    productionA.armyCommandActivity.available = true;
    productionA.armyCommandActivity.commandCount = 10;
    productionA.armyCommandActivity.gapDurationsMs = {1000.0, 2000.0};

    smp::AnalysisResult gameB;
    gameB.activeDurationSeconds = 240.0;
    auto productionB = production(
        repeatedCycles(smp::MacroProductType::Worker, 0),
        repeatedCycles(smp::MacroProductType::Army, 0));
    productionB.armyCommandActivity.available = true;
    productionB.armyCommandActivity.commandCount = 20;
    productionB.armyCommandActivity.gapDurationsMs = {10000.0};

    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, gameA, productionA));
    REQUIRE(session.addFinalizedGame(2, gameB, productionB));
    const auto& commands = session.stats().armyCommands;
    REQUIRE(commands.gamesAnalyzed == 2);
    REQUIRE(commands.commandCount == 30);
    REQUIRE_NEAR(commands.analyzedActiveSeconds, 300.0, 0.001);
    REQUIRE_NEAR(*commands.commandsPerMinute(), 6.0, 0.001);
    REQUIRE(commands.gapDurationsMs.size() == 3);
    REQUIRE_NEAR(*commands.medianGapMs(), 2000.0, 0.001);
    REQUIRE_NEAR(*commands.p90GapMs(), 8400.0, 0.001);
    REQUIRE_NEAR(*commands.longestGapMs(), 10000.0, 0.001);
}

TEST_CASE("automatic session does not create Army command gaps across games") {
    smp::AnalysisResult game;
    game.activeDurationSeconds = 60.0;
    auto perGame = production(
        repeatedCycles(smp::MacroProductType::Worker, 0),
        repeatedCycles(smp::MacroProductType::Army, 0));
    perGame.armyCommandActivity.available = true;
    perGame.armyCommandActivity.commandCount = 1;

    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, game, perGame));
    REQUIRE(session.addFinalizedGame(2, game, perGame));
    REQUIRE(session.stats().armyCommands.commandCount == 2);
    REQUIRE(session.stats().armyCommands.gapDurationsMs.empty());
    REQUIRE(!session.stats().armyCommands.medianGapMs().has_value());
}

TEST_CASE("automatic session Army command rate includes available zero and excludes unavailable time") {
    smp::AnalysisResult availableGame;
    availableGame.activeDurationSeconds = 60.0;
    auto withCommands = production(
        repeatedCycles(smp::MacroProductType::Worker, 0),
        repeatedCycles(smp::MacroProductType::Army, 0));
    withCommands.armyCommandActivity.available = true;
    withCommands.armyCommandActivity.commandCount = 10;
    auto availableZero = production(
        repeatedCycles(smp::MacroProductType::Worker, 0),
        repeatedCycles(smp::MacroProductType::Army, 0));
    availableZero.armyCommandActivity.available = true;

    smp::AnalysisResult unavailableGame;
    unavailableGame.activeDurationSeconds = 300.0;
    auto unavailable = production(
        repeatedCycles(smp::MacroProductType::Worker, 0),
        repeatedCycles(smp::MacroProductType::Army, 0));
    unavailable.armyCommandActivity.available = false;

    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, availableGame, withCommands));
    REQUIRE(session.addFinalizedGame(2, availableGame, availableZero));
    REQUIRE(session.addFinalizedGame(3, unavailableGame, unavailable));
    const auto& commands = session.stats().armyCommands;
    REQUIRE(commands.gamesAnalyzed == 2);
    REQUIRE(commands.gamesUnavailable == 1);
    REQUIRE_NEAR(commands.analyzedActiveSeconds, 120.0, 0.001);
    REQUIRE_NEAR(*commands.commandsPerMinute(), 5.0, 0.001);
}

TEST_CASE("automatic session Ability rate includes available zero and excludes unavailable time") {
    smp::AnalysisResult game;
    game.activeDurationSeconds = 60.0;
    auto withAbilities = production(
        repeatedCycles(smp::MacroProductType::Worker, 0),
        repeatedCycles(smp::MacroProductType::Army, 0));
    withAbilities.abilityActivity = abilityActivity(60.0, 10);
    auto availableZero = production(
        repeatedCycles(smp::MacroProductType::Worker, 0),
        repeatedCycles(smp::MacroProductType::Army, 0));
    availableZero.abilityActivity = abilityActivity(60.0, 0);

    smp::AnalysisResult unavailableGame;
    unavailableGame.activeDurationSeconds = 300.0;
    auto unavailable = production(
        repeatedCycles(smp::MacroProductType::Worker, 0),
        repeatedCycles(smp::MacroProductType::Army, 0));
    unavailable.abilityActivity.available = false;

    smp::AutomaticSessionState session;
    REQUIRE(withAbilities.abilityActivity.totalUses() == 10);
    REQUIRE(withAbilities.abilityActivity.observations.size() == 10);
    REQUIRE(withAbilities.abilityActivity.usesByAbility.at("Psionic Storm") ==
            10);
    REQUIRE(session.addFinalizedGame(1, game, withAbilities));
    REQUIRE(session.addFinalizedGame(2, game, availableZero));
    REQUIRE(session.addFinalizedGame(3, unavailableGame, unavailable));
    const auto& abilities = session.stats().abilityActivity;
    REQUIRE(abilities.gamesAnalyzed == 2);
    REQUIRE(abilities.gamesUnavailable == 1);
    REQUIRE_NEAR(abilities.analyzedActiveSeconds, 120.0, 0.001);
    REQUIRE(abilities.totalUses == 10);
    REQUIRE_NEAR(*abilities.abilitiesPerMinute(), 5.0, 0.001);
}

TEST_CASE("automatic session multitasking average and peak pool windows") {
    smp::AnalysisResult first;
    first.activeDurationSeconds = 10.0;
    first.navigationEvents.push_back(
        navigation(smp::CameraNavigationType::ControlGroupJump));
    first.navigationEvents.back().activeMs = 1000.0;
    first.navigationEvents.push_back(
        navigation(smp::CameraNavigationType::ControlGroupJump));
    first.navigationEvents.back().activeMs = 6000.0;

    smp::AnalysisResult second;
    second.activeDurationSeconds = 5.0;
    second.navigationEvents.push_back(
        navigation(smp::CameraNavigationType::ControlGroupJump));
    second.navigationEvents.back().activeMs = 1000.0;
    auto secondProduction = production(
        timedCycles(smp::MacroProductType::Worker, {{1.0, 2.0}}),
        timedCycles(smp::MacroProductType::Army, {{1.0, 2.0}}));
    secondProduction.armyControlGroupManagement.available = true;
    smp::ArmyControlGroupEdit edit;
    edit.operationActiveMs = 1000.0;
    edit.scope = smp::ArmyControlGroupScope::Army;
    secondProduction.armyControlGroupManagement.edits.push_back(edit);
    auto firstProduction = production(
        repeatedCycles(smp::MacroProductType::Worker, 0),
        repeatedCycles(smp::MacroProductType::Army, 0));
    firstProduction.armyControlGroupManagement.available = true;

    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, first, firstProduction));
    REQUIRE(session.addFinalizedGame(2, second, secondProduction));
    const auto& multitasking = session.stats().multitasking;
    REQUIRE(multitasking.activeWindowCount == 3);
    REQUIRE(multitasking.totalDiversityAcrossActiveWindows == 6);
    REQUIRE_NEAR(*multitasking.averageActiveDiversity(), 2.0, 0.001);
    REQUIRE_NEAR(*multitasking.peak(), 4.0, 0.001);
}

TEST_CASE("automatic session multitasking rejects each missing replay-backed input") {
    smp::AnalysisResult game;
    game.activeDurationSeconds = 60.0;
    game.navigationEvents.push_back(
        navigation(smp::CameraNavigationType::ControlGroupJump));

    for (int missing = 0; missing < 3; ++missing) {
        auto inputs = production(
            repeatedCycles(smp::MacroProductType::Worker, 0),
            repeatedCycles(smp::MacroProductType::Army, 0));
        inputs.armyControlGroupManagement.available = true;
        if (missing == 0)
            inputs.workerMacroCycles.available = false;
        else if (missing == 1)
            inputs.armyMacroCycles.available = false;
        else
            inputs.armyControlGroupManagement.available = false;

        const auto stats = smp::automaticSessionStatsForGame(game, inputs);
        REQUIRE(stats.multitasking.gamesAnalyzed == 0);
        REQUIRE(stats.multitasking.gamesUnavailable == 1);
        REQUIRE(stats.multitasking.activeWindowCount == 0);
        REQUIRE(!stats.multitasking.averageActiveDiversity().has_value());
        REQUIRE(!stats.multitasking.peak().has_value());
    }
}

TEST_CASE("automatic session multitasking accepts a genuine zero Scout-command class") {
    smp::AnalysisResult game;
    game.activeDurationSeconds = 5.0;
    game.navigationEvents.push_back(
        navigation(smp::CameraNavigationType::ControlGroupJump));
    game.navigationEvents.back().activeMs = 1000.0;
    auto inputs = production(
        timedCycles(smp::MacroProductType::Worker, {{1.0, 2.0}}),
        repeatedCycles(smp::MacroProductType::Army, 0));
    inputs.armyControlGroupManagement.available = true;
    REQUIRE(inputs.armyControlGroupManagement.scoutingUnitActivities.empty());

    const auto stats = smp::automaticSessionStatsForGame(game, inputs);
    REQUIRE(stats.multitasking.gamesAnalyzed == 1);
    REQUIRE(stats.multitasking.gamesUnavailable == 0);
    REQUIRE(stats.multitasking.activeWindowCount == 1);
    REQUIRE_NEAR(*stats.multitasking.averageActiveDiversity(), 2.0, 0.001);
    REQUIRE_NEAR(*stats.multitasking.peak(), 2.0, 0.001);
}

TEST_CASE("session access method percentages pool production visits rather than cycles") {
    smp::AnalysisResult game;
    auto worker = cycles(smp::MacroProductType::Worker, {1000.0}, 4);
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(
        1, game, production(worker, cycles(smp::MacroProductType::Army, {}))));
    const auto& stats = session.stats().workerMacro;
    REQUIRE_NEAR(stats.accessMethodPercentage(smp::ProductionAccessMethod::ControlGroup), 25.0, 0.001);
    REQUIRE_NEAR(stats.accessMethodPercentage(smp::ProductionAccessMethod::LocationHotkeyClick), 25.0, 0.001);
    REQUIRE_NEAR(stats.accessMethodPercentage(smp::ProductionAccessMethod::MinimapClick), 25.0, 0.001);
    REQUIRE_NEAR(stats.accessMethodPercentage(smp::ProductionAccessMethod::ScreenClick), 25.0, 0.001);
}

TEST_CASE("automatic session pools macro access style counts and speed across games") {
    smp::AnalysisResult game;
    auto firstWorker = cycles(smp::MacroProductType::Worker, {100.0, 300.0});
    firstWorker.cycles[0].macroAccessStyle =
        smp::MacroAccessStyle::ControlGroupOnly;
    firstWorker.cycles[1].macroAccessStyle =
        smp::MacroAccessStyle::LocationHotkeyClick;
    auto secondWorker = cycles(smp::MacroProductType::Worker, {500.0, 700.0});
    secondWorker.cycles[0].macroAccessStyle =
        smp::MacroAccessStyle::ControlGroupOnly;
    secondWorker.cycles[1].macroAccessStyle = smp::MacroAccessStyle::Mixed;
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(
        1, game,
        production(firstWorker, cycles(smp::MacroProductType::Army, {}))));
    REQUIRE(session.addFinalizedGame(
        2, game,
        production(secondWorker, cycles(smp::MacroProductType::Army, {}))));

    const auto& stats = session.stats().workerMacro;
    REQUIRE_NEAR(stats.accessStylePercentage(
                     smp::MacroAccessStyle::ControlGroupOnly),
                 50.0, 0.001);
    REQUIRE_NEAR(stats.accessStylePercentage(
                     smp::MacroAccessStyle::LocationHotkeyClick),
                 25.0, 0.001);
    REQUIRE_NEAR(stats.accessStylePercentage(smp::MacroAccessStyle::Mixed),
                 25.0, 0.001);
    const auto controlGroup =
        stats.accessStyleStatistics(smp::MacroAccessStyle::ControlGroupOnly);
    REQUIRE(controlGroup.cycleCount == 2);
    REQUIRE_NEAR(*controlGroup.averageDurationMs, 300.0, 0.001);
    REQUIRE_NEAR(*controlGroup.medianDurationMs, 300.0, 0.001);
    REQUIRE_NEAR(*controlGroup.bestDurationMs, 100.0, 0.001);
}

TEST_CASE("automatic report shows separate worker and army macro sections") {
    smp::AnalysisResult game;
    game.activeDurationSeconds = 60.0;
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(
        1, game, production(cycles(smp::MacroProductType::Worker, {700.0, 1500.0}, 3),
                             cycles(smp::MacroProductType::Army, {900.0}, 2))));
    CoutCapture capture;
    smp::printAutomaticSessionReport(session);
    const auto output = capture.str();
    REQUIRE(output.find("WORKER MACRO") != std::string::npos);
    REQUIRE(output.find("ARMY MACRO") != std::string::npos);
    REQUIRE(output.find("MACRO CYCLES") == std::string::npos);
    REQUIRE(output.find("Production visits") != std::string::npos);
    REQUIRE(output.find("ACCESS METHOD") != std::string::npos);
    REQUIRE(output.find("MACRO ACCESS STYLES") != std::string::npos);
    REQUIRE(output.find("MACRO SPEED BY ACCESS STYLE") != std::string::npos);
    REQUIRE(output.find("Control-group only") != std::string::npos);
    REQUIRE(output.find("1.10 s") != std::string::npos);
}

TEST_CASE("unavailable replay games do not contribute fake zero macro cycles") {
    smp::AnalysisResult game;
    smp::ProductionAnalysis unavailable;
    unavailable.workerMacroCycles.productType = smp::MacroProductType::Worker;
    unavailable.workerMacroCycles.unavailableReason = "replay correlation failed";
    unavailable.armyMacroCycles.productType = smp::MacroProductType::Army;
    unavailable.armyMacroCycles.unavailableReason = "replay correlation failed";
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, game, unavailable));
    REQUIRE(session.stats().workerMacro.gamesAnalyzed == 0);
    REQUIRE(session.stats().workerMacro.gamesUnavailable == 1);
    REQUIRE(session.stats().workerMacro.cycles == 0);
    REQUIRE(!session.stats().workerMacro.averageDurationMs());

    CoutCapture capture;
    smp::printAutomaticSessionReport(session);
    const auto output = capture.str();
    REQUIRE(output.find("Unavailable: replay correlation failed") != std::string::npos);
    REQUIRE(output.find("Games unavailable") != std::string::npos);
}

TEST_CASE("automatic session report omits the edge-pan direction table") {
    smp::AnalysisResult game;
    game.activeDurationSeconds = 60.0;
    game.navigationEvents.push_back(
        navigation(smp::CameraNavigationType::EdgeScroll, smp::EdgeDirection::Left));
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, game));
    CoutCapture capture;
    smp::printAutomaticSessionReport(session);
    const auto output = capture.str();
    REQUIRE(output.find("Edge pans") != std::string::npos);
    REQUIRE(output.find("EDGE PAN") == std::string::npos);
    REQUIRE(output.find("Left") == std::string::npos);
}

TEST_CASE("empty automatic session report says no games were recorded") {
    smp::AutomaticSessionState session;
    CoutCapture capture;
    smp::printAutomaticSessionReport(session);
    const auto output = capture.str();
    REQUIRE(output.find("SESSION SUMMARY") != std::string::npos);
    REQUIRE(output.find("No games recorded this session.") != std::string::npos);
    REQUIRE(output.find("LAST GAME") == std::string::npos);
}
