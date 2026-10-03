#include "test_framework.h"

#include "analysis/ability_activity.h"
#include "cli/automatic_session_eligibility.h"
#include "cli/report.h"
#include "cli/session_summary_paths.h"
#include "storage/nav_retention.h"
#include "storage/session.h"

#include <chrono>
#include <fstream>
#include <limits>

namespace {

struct Fixture {
    smp::AnalysisResult recording;
    smp::ReplayExtractionResult replay;
    Fixture() {
        recording.activeDurationSeconds = 10.0;
        replay.available = true;
        replay.parser = "fixture";
        replay.replay.totalFrames = 240;
        replay.replay.players = {{0, "recorded-player"}, {1, "other-player"}};
        for (int index = 0; index < 4; ++index) {
            smp::MechanicalInputEvent event;
            event.type = smp::MechanicalInputType::ControlGroupSelect;
            event.value = index + 1;
            event.virtualKey = static_cast<std::uint16_t>('1' + index);
            event.timestampTicks = static_cast<std::uint64_t>(index * 1000);
            event.activeMs = index * 1000.0;
            recording.mechanicalEvents.push_back(event);
            replay.replay.controlGroupSelections.push_back({index * 24, 0, index + 1});
            replay.replay.controlGroupSelections.push_back({index * 24, 1, 9 - index});
        }
        recording.navigationEvents = {
            {100, 100.0, smp::CameraNavigationType::ControlGroupJump, 1},
            {200, 200.0, smp::CameraNavigationType::LocationHotkey, 1},
            {300, 300.0, smp::CameraNavigationType::MinimapJump},
            {400, 400.0, smp::CameraNavigationType::EdgeScroll}
        };
        recording.navigationEvents.back().edgeDirection = smp::EdgeDirection::Left;
    }
};

struct TemporarySession {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("smp-session-eligibility-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path history = root / "sessionSummaries" / "2026-10-03_120000_session.json";
    ~TemporarySession() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

smp::ProductionAnalysis richProduction() {
    smp::ProductionAnalysis production;
    for (const auto product : {smp::MacroProductType::Worker, smp::MacroProductType::Army}) {
        std::vector<smp::MacroCycle> cycles;
        for (int index = 0; index < 2; ++index) {
            smp::MacroCycle cycle;
            cycle.productType = product;
            cycle.startActiveMs = index * 3000.0;
            cycle.endActiveMs = cycle.startActiveMs + 1000.0;
            cycle.durationMs = 1000.0;
            cycles.push_back(cycle);
        }
        auto summary = smp::summarizeProductMacroCycles(product, std::move(cycles), {});
        if (product == smp::MacroProductType::Worker) production.workerMacroCycles = std::move(summary);
        else production.armyMacroCycles = std::move(summary);
    }
    production.visitsAvailable = true;
    production.armyControlGroupManagement.available = true;
    production.armyCommandActivity.available = true;
    production.armyCommandActivity.commandCount = 3;
    production.armyCommandActivity.gapDurationsMs = {100, 200};
    production.abilityActivity = smp::analyzeAbilityActivity({{1, 0, 1000.0, "Stim"}}, 10.0);
    return production;
}

void requireExclusionLeavesSessionUnchanged(const Fixture& fixture, const std::string& reason) {
    TemporarySession files;
    smp::AutomaticSessionState session;
    Fixture valid;
    const auto production = richProduction();
    REQUIRE(smp::accountAutomaticSessionGame(session, 1, valid.recording, production,
        smp::automaticSessionEligibility(valid.recording, valid.replay)));
    smp::writeSeparatedAutomaticSessionHistory(files.history, session);
    const auto before = smp::json::stringify(smp::json::parseFile(files.history));
    const auto report = smp::formatAutomaticSessionReport(session);
    const auto lastGame = &*session.lastGame();
    const auto lastProduction = &*session.lastGameProduction();
    const auto stats = session.stats();
    const auto eligibility = smp::automaticSessionEligibility(fixture.recording, fixture.replay);
    REQUIRE(!eligibility.included);
    REQUIRE(eligibility.reason == reason);
    REQUIRE(smp::accountAutomaticSessionGame(session, 2, fixture.recording, production, eligibility));
    smp::writeSeparatedAutomaticSessionHistory(files.history, session);
    REQUIRE(smp::json::stringify(smp::json::parseFile(files.history)) == before);
    REQUIRE(smp::formatAutomaticSessionReport(session) == report);
    REQUIRE(session.stats().games == stats.games);
    REQUIRE(session.stats().activeSeconds == stats.activeSeconds);
    REQUIRE(session.stats().workerMacro.accessStyleDurationsMs == stats.workerMacro.accessStyleDurationsMs);
    REQUIRE(session.stats().armyMacro.accessStyleDurationsMs == stats.armyMacro.accessStyleDurationsMs);
    REQUIRE(session.stats().workerMacro.gapDurationsMs == stats.workerMacro.gapDurationsMs);
    REQUIRE(session.stats().armyMacro.gapDurationsMs == stats.armyMacro.gapDurationsMs);
    REQUIRE(session.stats().armyCommands.gapDurationsMs == stats.armyCommands.gapDurationsMs);
    REQUIRE(&*session.lastGame() == lastGame);
    REQUIRE(&*session.lastGameProduction() == lastProduction);
    REQUIRE(!session.addFinalizedGame(2, fixture.recording, production));
    REQUIRE(!session.markExcludedGeneration(2));
    REQUIRE(!session.markAbortedGeneration(2));
}

} // namespace

TEST_CASE("automatic session excludes zero or one live CG selection without any aggregate mutation") {
    for (const std::size_t count : {0, 1}) {
        Fixture fixture;
        fixture.recording.mechanicalEvents.resize(count);
        requireExclusionLeavesSessionUnchanged(fixture,
            "Live recording contains too few control-group selections for replay-player identification");
    }
}

TEST_CASE("automatic session excludes ambiguous player identity without any aggregate mutation") {
    Fixture fixture;
    for (auto& event : fixture.replay.replay.controlGroupSelections)
        if (event.playerId == 1) event.group = static_cast<int>(event.replayFrame / 24) + 1;
    requireExclusionLeavesSessionUnchanged(fixture, "Replay player match is ambiguous");
}

TEST_CASE("automatic session excludes no confident match and no replay players") {
    Fixture fixture;
    for (auto& event : fixture.replay.replay.controlGroupSelections) event.group = 9;
    requireExclusionLeavesSessionUnchanged(fixture,
        "No replay player has a high-confidence control-group sequence match");
    fixture.replay.replay.players.clear();
    requireExclusionLeavesSessionUnchanged(fixture, "Replay contains no players");
}

TEST_CASE("automatic session excludes unavailable replay using actual extraction reason") {
    Fixture fixture;
    fixture.replay.available = false;
    fixture.replay.unavailableReason = "Unable to pin this generation's replay source";
    requireExclusionLeavesSessionUnchanged(fixture,
        "Replay unavailable: Unable to pin this generation's replay source");
}

TEST_CASE("confidently identified ten second automatic game is included without a duration cutoff") {
    Fixture fixture;
    const auto eligibility = smp::automaticSessionEligibility(fixture.recording, fixture.replay);
    REQUIRE(eligibility.included);
    smp::AutomaticSessionState session;
    REQUIRE(smp::accountAutomaticSessionGame(session, 1, fixture.recording, richProduction(), eligibility));
    REQUIRE(session.stats().games == 1);
    REQUIRE(session.stats().activeSeconds == 10.0);
    REQUIRE(session.stats().navigationTransitions() == 4);
    REQUIRE(session.stats().workerMacro.cycles == 2);
    REQUIRE(session.stats().armyCommands.commandCount == 3);
    REQUIRE(session.stats().abilityActivity.totalUses == 1);
}

TEST_CASE("identified player remains session eligible when downstream timeline anchors fail") {
    Fixture fixture;
    for (auto& event : fixture.recording.mechanicalEvents)
        event.activeMs = std::numeric_limits<double>::quiet_NaN();
    const auto eligibility = smp::automaticSessionEligibility(fixture.recording, fixture.replay);
    REQUIRE(eligibility.included);
    const auto production = smp::correlateProductionVisitsWithReplay(
        fixture.recording, {}, 1000, richProduction(), fixture.replay.replay, "fixture");
    REQUIRE(!production.replayCorrelation.available);
    REQUIRE(production.replayCorrelation.unavailableReason == "Replay/live timeline has no usable anchors");
    smp::AutomaticSessionState session;
    REQUIRE(smp::accountAutomaticSessionGame(session, 1, fixture.recording, production, eligibility));
    REQUIRE(session.stats().games == 1);
    REQUIRE(session.stats().activeSeconds == 10.0);
    REQUIRE(session.stats().navigationTransitions() == 4);
    REQUIRE(session.stats().workerMacro.gamesUnavailable == 1);
}

TEST_CASE("identified player remains session eligible when custom production hotkeys are unavailable") {
    Fixture fixture;
    smp::MacroHotkeyProfile unavailable;
    unavailable.unavailableReason = "Custom production hotkeys unavailable";
    auto production = smp::analyzeProductionVisits(fixture.recording, unavailable, 1000);
    production = smp::correlateProductionVisitsWithReplay(
        fixture.recording, unavailable, 1000, std::move(production), fixture.replay.replay, "fixture");
    REQUIRE(!production.replayCorrelation.available);
    REQUIRE(production.replayCorrelation.unavailableReason == unavailable.unavailableReason);
    smp::AutomaticSessionState session;
    REQUIRE(smp::accountAutomaticSessionGame(session, 1, fixture.recording, production,
        smp::automaticSessionEligibility(fixture.recording, fixture.replay)));
    REQUIRE(session.stats().games == 1);
    REQUIRE(session.stats().navigationTransitions() == 4);
}

TEST_CASE("valid excluded valid history contains exactly two games and only eligible aggregates") {
    TemporarySession files;
    const auto referencePath = files.history.parent_path() / "reference_session.json";
    smp::AutomaticSessionState session;
    smp::AutomaticSessionState reference;
    Fixture fixture;
    const auto production = richProduction();
    for (const std::uint64_t generation : {1, 2, 3}) {
        fixture.recording.activeDurationSeconds = generation * 10.0;
        auto replay = fixture.replay;
        if (generation == 2) replay.available = false;
        REQUIRE(smp::accountAutomaticSessionGame(session, generation, fixture.recording, production,
            smp::automaticSessionEligibility(fixture.recording, replay)));
        smp::writeSeparatedAutomaticSessionHistory(files.history, session);
        if (generation != 2) {
            REQUIRE(reference.addFinalizedGame(generation, fixture.recording, production));
            smp::writeSeparatedAutomaticSessionHistory(referencePath, reference);
        }
    }
    REQUIRE(session.stats().games == 2);
    REQUIRE(session.stats().activeSeconds == 40.0);
    const auto history = smp::json::parseFile(files.history);
    const auto expected = smp::json::parseFile(referencePath);
    REQUIRE(history["schema_version"].asInt() == 3);
    REQUIRE(history["games"].asArray().size() == 2);
    REQUIRE(history["games"].asArray()[0]["ordinal"].asInt() == 1);
    REQUIRE(history["games"].asArray()[1]["ordinal"].asInt() == 2);
    for (const auto field : {"games", "overall", "matchups"})
        REQUIRE(smp::json::stringify(history[field]) == smp::json::stringify(expected[field]));
    REQUIRE(smp::formatAutomaticSessionReport(session) == smp::formatAutomaticSessionReport(reference));
}

TEST_CASE("excluded generations share exactly once accounting with included and aborted generations") {
    Fixture fixture;
    smp::AutomaticSessionState session;
    REQUIRE(!session.markExcludedGeneration(0));
    REQUIRE(session.markExcludedGeneration(1));
    REQUIRE(!session.markExcludedGeneration(1));
    REQUIRE(!session.markAbortedGeneration(1));
    REQUIRE(!session.addFinalizedGame(1, fixture.recording));
    REQUIRE(session.addFinalizedGame(2, fixture.recording));
    REQUIRE(!session.markExcludedGeneration(2));
    REQUIRE(session.markAbortedGeneration(3));
    REQUIRE(!session.markExcludedGeneration(3));
    REQUIRE(session.stats().games == 1);
}

TEST_CASE("excluded automatic diagnostic preserves generation and exact evidence reason") {
    Fixture fixture;
    fixture.recording.mechanicalEvents.clear();
    REQUIRE(smp::automaticGameExclusionDiagnostic(7,
        smp::automaticSessionEligibility(fixture.recording, fixture.replay)) ==
        "AUTO_GAME_EXCLUDED generation=7 reason=player_unidentified detail=\"Live recording contains too few control-group selections for replay-player identification\"");
    fixture.replay.available = false;
    fixture.replay.unavailableReason = "Replay source unreadable";
    REQUIRE(smp::automaticGameExclusionDiagnostic(8,
        smp::automaticSessionEligibility(fixture.recording, fixture.replay)) ==
        "AUTO_GAME_EXCLUDED generation=8 reason=player_unidentified detail=\"Replay unavailable: Replay source unreadable\"");
}

TEST_CASE("all excluded session preserves individual evidence and remains managed by normal NAV retention") {
    TemporarySession files;
    const auto sessions = files.root / "sessions";
    smp::AutomaticSessionState session;
    Fixture fixture;
    fixture.replay.available = false;
    std::vector<std::filesystem::path> navs;
    std::vector<std::filesystem::path> jsons;
    std::vector<std::filesystem::path> raws;
    for (const std::uint64_t generation : {1, 2}) {
        smp::SessionWriter writer(sessions, 1000, 1, true);
        writer.setActiveTimelineAnchor({0, 1'700'000'000'000'000'000});
        smp::RawInputEvent event;
        event.timestampTicks = 1000;
        REQUIRE(writer.submitRaw(event));
        writer.stop();
        const auto nav = writer.writeNavigation(fixture.recording);
        // Deterministic chronology for the retention policy's ordering.
        smp::writeNavSession(nav, fixture.recording, writer.sessionId(), 1000, generation * 1000,
                            smp::QpcWallClockAnchor{0, 1'700'000'000'000'000'000});
        const auto json = smp::writeAnalysisJson(nav,
            smp::analysisToJson(fixture.recording, writer.sessionId()));
        navs.push_back(nav); jsons.push_back(json); raws.push_back(writer.rawPath());
        REQUIRE(smp::accountAutomaticSessionGame(session, generation, fixture.recording, {},
            smp::automaticSessionEligibility(fixture.recording, fixture.replay)));
        smp::writeSeparatedAutomaticSessionHistory(files.history, session);
        const auto history = smp::json::parseFile(files.history);
        REQUIRE(history["overall"]["games"].asInt() == 0);
        REQUIRE(history["games"].asArray().empty());
        REQUIRE(history["matchups"].asObject().empty());
        REQUIRE(std::filesystem::is_regular_file(nav));
        REQUIRE(std::filesystem::is_regular_file(json));
        REQUIRE(std::filesystem::is_regular_file(writer.rawPath()));
        const smp::NavRetentionPolicy policy = generation == 1
            ? smp::NavRetentionPolicy{smp::NavRetentionMode::KeepAll, 10}
            : smp::NavRetentionPolicy{smp::NavRetentionMode::KeepLastGames, 1};
        const auto retention = smp::recordFinalizedAutomaticNavAndApplyRetention(
            sessions, nav, json, files.history, policy);
        REQUIRE(retention.registrationPersisted);
        if (generation == 1) REQUIRE(retention.cleanup.removedPaths.empty());
        else REQUIRE(retention.cleanup.removedPaths.size() == 1);
    }
    REQUIRE(!std::filesystem::exists(navs.front())); // Only normal retention pruned this NAV.
    REQUIRE(std::filesystem::exists(navs.back()));
    for (const auto& json : jsons) REQUIRE(std::filesystem::exists(json));
    for (const auto& raw : raws) REQUIRE(std::filesystem::exists(raw));
    REQUIRE(session.empty());
}
