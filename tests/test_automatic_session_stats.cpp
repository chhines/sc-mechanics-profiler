#include "test_framework.h"

#include "cli/automatic_session_stats.h"
#include "cli/report.h"

#include <iostream>
#include <sstream>

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

smp::ProductionAnalysis production(smp::ProductMacroCycleAnalysis worker,
                                   smp::ProductMacroCycleAnalysis army) {
    smp::ProductionAnalysis result;
    result.visitsAvailable = true;
    result.workerMacroCycles = std::move(worker);
    result.armyMacroCycles = std::move(army);
    return result;
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
