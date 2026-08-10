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
    REQUIRE(session.lastGame()->navigationEvents.size() == game.navigationEvents.size());
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
    REQUIRE(session.stats().navigationTransitions() == 10);
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
