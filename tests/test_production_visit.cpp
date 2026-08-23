#include "test_framework.h"

#include "analysis/production_visit.h"
#include "analysis/replay_analysis.h"
#include "storage/session.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t testQpcFrequency = 1000;

const std::string& realisticSettingsJson() {
    static const std::string fixture = R"json({
  "General settings": {"Starcraft-Game Custom Hotkeys": true},
  "Hotkeys": "STR_MAKE_P_PROBE=e\nSTR_MAKE_P_ZEALOT=z\nSTR_MAKE_P_DRAGOON=d\nSTR_MAKE_P_DTEMPLAR=q\nSTR_MAKE_P_OBSERVER=q\nSTR_MAKE_P_CORSAIR=e\nSTR_MAKE_P_SHUTTLE=s\nSTR_MAKE_P_REAVER=v\nSTR_MAKE_P_ARBITER=a\nSTR_MAKE_P_INTERCEPTOR=i\nSTR_MAKE_P_SCARAB=r\nSTR_MAKE_T_SCV=s\nSTR_MAKE_T_MARINE=m\nSTR_MAKE_Z_DRONE=w\nSTR_MAKE_Z_HYDRALISK=h\nSTR_ATTACK=a\nSTR_STOP=s"
})json";
    return fixture;
}

smp::MacroHotkeyProfile profile() {
    return smp::parseStarCraftHotkeyProfile(realisticSettingsJson());
}

smp::MechanicalInputEvent mechanical(smp::MechanicalInputType type, std::uint64_t ticks,
                                     std::uint16_t key = 0, int value = -1,
                                     std::uint16_t modifiers = smp::ModifierNone,
                                     int cursorX = 0, int cursorY = 0) {
    smp::MechanicalInputEvent event;
    event.timestampTicks = ticks;
    event.activeMs = static_cast<double>(ticks);
    event.type = type;
    event.virtualKey = key;
    event.modifiers = modifiers;
    event.value = value;
    event.cursorX = cursorX;
    event.cursorY = cursorY;
    return event;
}

void select(std::vector<smp::MechanicalInputEvent>& events, int group, std::uint64_t ticks) {
    events.push_back(mechanical(smp::MechanicalInputType::ControlGroupSelect, ticks,
                                static_cast<std::uint16_t>('0' + group), group));
}

void key(std::vector<smp::MechanicalInputEvent>& events, char value, std::uint64_t ticks,
         std::uint16_t modifiers = smp::ModifierNone) {
    events.push_back(mechanical(smp::MechanicalInputType::KeyPress, ticks,
                                static_cast<std::uint16_t>(value), -1, modifiers));
}

void visit(std::vector<smp::MechanicalInputEvent>& events, int group, char productionKey,
           int presses, std::uint64_t startTicks) {
    select(events, group, startTicks);
    for (int index = 0; index < presses; ++index)
        key(events, productionKey, startTicks + 100 + static_cast<std::uint64_t>(index) * 50);
}

std::vector<smp::LikelyProductionGroup> established(std::initializer_list<std::pair<int, char>> groups) {
    std::vector<smp::LikelyProductionGroup> result;
    for (const auto& [group, productionKey] : groups)
        result.push_back({group, {static_cast<std::uint16_t>(productionKey)}});
    return result;
}

smp::ProductionAnalysis heuristicBase(const smp::AnalysisResult& live,
                                      const std::vector<smp::LikelyProductionGroup>& groups) {
    smp::ProductionAnalysis analysis;
    analysis.visitsAvailable = true;
    analysis.likelyProductionGroups = groups;
    analysis.productionVisits = smp::detectHeuristicProductionVisitsForLikelyGroups(
        live, profile(), testQpcFrequency, groups);
    return analysis;
}

smp::ReplayData replayWithPlayers() {
    smp::ReplayData replay;
    replay.totalFrames = 240;
    replay.players = {{0, "matched-player"}, {1, "other-player"}};
    return replay;
}

void addAnchor(smp::AnalysisResult& live, smp::ReplayData& replay, int group,
               std::uint64_t liveTicks, std::int64_t replayFrame) {
    select(live.mechanicalEvents, group, liveTicks);
    replay.controlGroupSelections.push_back({replayFrame, 0, group});
}

void addWrongPlayerReverseAnchors(smp::ReplayData& replay) {
    replay.controlGroupSelections.push_back({0, 1, 4});
    replay.controlGroupSelections.push_back({24, 1, 3});
    replay.controlGroupSelections.push_back({48, 1, 2});
    replay.controlGroupSelections.push_back({72, 1, 1});
}

void addReplaySelection(smp::ReplayData& replay, std::int64_t frame,
                        smp::ReplaySelectionKind kind = smp::ReplaySelectionKind::Select,
                        std::initializer_list<std::uint32_t> unitTags = {100}) {
    smp::ReplaySelectionEvent selection;
    selection.replayFrame = frame;
    selection.playerId = 0;
    selection.kind = kind;
    selection.unitTags.assign(unitTags.begin(), unitTags.end());
    replay.selections.push_back(std::move(selection));
}

smp::ProductionAnalysis correlate(const smp::AnalysisResult& live, smp::ReplayData replay,
                                  smp::ProductionAnalysis base) {
    return smp::correlateProductionVisitsWithReplay(live, profile(), testQpcFrequency,
                                                    std::move(base), replay, "fixture-parser");
}

smp::ProductionVisit classifiedVisit(smp::MacroProductType type, std::uint64_t start,
                                     std::uint64_t end,
                                     smp::ProductionAccessMethod access =
                                         smp::ProductionAccessMethod::ControlGroup) {
    smp::ProductionVisit visit;
    visit.productType = type;
    visit.accessMethod = access;
    visit.selectionAccess =
        access == smp::ProductionAccessMethod::ControlGroup
            ? smp::ProductionSelectionAccess::ControlGroup
            : smp::ProductionSelectionAccess::DirectClick;
    visit.startTimestampTicks = start;
    visit.endTimestampTicks = end;
    visit.contextTimestampTicks = start;
    visit.firstProductionTimestampTicks = end;
    visit.startActiveMs = static_cast<double>(start);
    visit.endActiveMs = static_cast<double>(end);
    visit.contextActiveMs = static_cast<double>(start);
    visit.firstProductionActiveMs = static_cast<double>(end);
    smp::refreshProductionVisitTiming(visit, testQpcFrequency);
    visit.replayConfirmed = true;
    visit.physicalProductionPresses = 1;
    visit.replayProductionCommands = 1;
    return visit;
}

smp::ProductMacroCycleAnalysis groupedPairWithGap(smp::MacroProductType type,
                                                   std::uint64_t gapMs) {
    auto first = classifiedVisit(type, 0, 1000);
    auto second = classifiedVisit(type, 1000 + gapMs, 1100 + gapMs);
    first.productionContext = smp::makeControlGroupProductionContext(5);
    second.productionContext = smp::makeControlGroupProductionContext(6);
    return smp::groupProductionVisits({first, second}, type, testQpcFrequency);
}

} // namespace

TEST_CASE("hotkey snapshot preserves ambiguous production bindings without treating attack as production") {
    const auto hotkeys = profile();
    REQUIRE(hotkeys.available);
    const auto eCommands = hotkeys.compatibleProductionCommands('E');
    REQUIRE(eCommands.size() == 2);
    REQUIRE(std::find(eCommands.begin(), eCommands.end(), "STR_MAKE_P_PROBE") != eCommands.end());
    REQUIRE(std::find(eCommands.begin(), eCommands.end(), "STR_MAKE_P_CORSAIR") != eCommands.end());
    REQUIRE(hotkeys.compatibleProductionCommands('A').size() == 1);
    REQUIRE(std::none_of(hotkeys.productionCommands.begin(), hotkeys.productionCommands.end(),
                         [](const auto& binding) { return binding.command == "STR_ATTACK"; }));
}

TEST_CASE("current control-group heuristic becomes one ProductionVisit and preserves repeated presses") {
    smp::AnalysisResult live;
    visit(live.mechanicalEvents, 5, 'D', 4, 1000);
    const auto visits = smp::detectHeuristicProductionVisitsForLikelyGroups(
        live, profile(), testQpcFrequency, established({{5, 'D'}}));
    REQUIRE(visits.size() == 1);
    REQUIRE(visits[0].accessMethod == smp::ProductionAccessMethod::ControlGroup);
    REQUIRE(visits[0].controlGroup == 5);
    REQUIRE(visits[0].productionContext.kind == smp::ProductionContextKind::ControlGroup);
    REQUIRE(visits[0].productionContext.controlGroup == 5);
    REQUIRE(visits[0].contextTimestampTicks == visits[0].startTimestampTicks);
    REQUIRE_NEAR(visits[0].contextActiveMs, visits[0].startActiveMs, 0.001);
    REQUIRE(visits[0].physicalProductionPresses == 4);
    REQUIRE_NEAR(visits[0].startActiveMs, 1000.0, 0.001);
    REQUIRE_NEAR(visits[0].firstProductionActiveMs, 1100.0, 0.001);
    REQUIRE_NEAR(visits[0].endActiveMs, 1250.0, 0.001);
    REQUIRE_NEAR(visits[0].executionDurationMs, 100.0, 0.001);
    REQUIRE_NEAR(visits[0].productionBurstSpanMs, 150.0, 0.001);
    REQUIRE_NEAR(visits[0].durationMs, 250.0, 0.001);
    REQUIRE(visits[0].productType == smp::MacroProductType::Unknown);
}

TEST_CASE("control-group production candidates inherit the assignment generation at each select") {
    smp::AnalysisResult live;
    select(live.mechanicalEvents, 5, 100);
    key(live.mechanicalEvents, 'D', 150);
    live.mechanicalEvents.push_back(
        mechanical(smp::MechanicalInputType::ControlGroupAssign, 500, '5', 5,
                   smp::ModifierCtrl));
    select(live.mechanicalEvents, 5, 600);
    key(live.mechanicalEvents, 'D', 650);
    live.mechanicalEvents.push_back(
        mechanical(smp::MechanicalInputType::ControlGroupAssign, 1000, '6', 6,
                   smp::ModifierCtrl));
    select(live.mechanicalEvents, 5, 1100);
    key(live.mechanicalEvents, 'D', 1150);
    live.mechanicalEvents.push_back(
        mechanical(smp::MechanicalInputType::ControlGroupAssign, 1500, '5', 5,
                   smp::ModifierCtrl));
    select(live.mechanicalEvents, 5, 1600);
    key(live.mechanicalEvents, 'D', 1650);

    const auto candidates =
        smp::detectControlGroupProductionCandidates(live, profile(), testQpcFrequency);
    REQUIRE(candidates.size() == 4);
    REQUIRE(candidates[0].visit.productionContext.assignmentGeneration == 0);
    REQUIRE(candidates[1].visit.productionContext.assignmentGeneration == 1);
    REQUIRE(candidates[2].visit.productionContext.assignmentGeneration == 1);
    REQUIRE(candidates[3].visit.productionContext.assignmentGeneration == 2);
}

TEST_CASE("production group inference keeps repeat and mouse ambiguity protections") {
    std::vector<smp::MechanicalInputEvent> strong;
    visit(strong, 5, 'D', 3, 0);
    visit(strong, 5, 'D', 2, 2000);
    const auto inferred = smp::inferLikelyProductionGroups(strong, profile(), testQpcFrequency);
    REQUIRE(std::any_of(inferred.begin(), inferred.end(), [](const auto& group) { return group.group == 5; }));

    std::vector<smp::MechanicalInputEvent> ambiguous;
    select(ambiguous, 1, 0);
    key(ambiguous, 'A', 100);
    ambiguous.push_back(mechanical(smp::MechanicalInputType::MouseLeftDown, 150));
    select(ambiguous, 1, 1000);
    key(ambiguous, 'A', 1100);
    ambiguous.push_back(mechanical(smp::MechanicalInputType::MouseRightDown, 1150));
    REQUIRE(smp::inferLikelyProductionGroups(ambiguous, profile(), testQpcFrequency).empty());
}

TEST_CASE("replay player identification chooses the monotonic matching sequence and tolerates omissions") {
    smp::AnalysisResult live;
    smp::ReplayData replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    addAnchor(live, replay, 3, 2000, 48);
    addAnchor(live, replay, 4, 3000, 72);
    addWrongPlayerReverseAnchors(replay);
    addReplaySelection(replay, 12);
    replay.selections.push_back({36, 1, smp::ReplaySelectionKind::Select, {500}, 0});
    auto match = smp::identifyReplayPlayer(live.mechanicalEvents, replay);
    REQUIRE(match.available);
    REQUIRE(match.playerId == 0);
    REQUIRE(match.playerName == "matched-player");
    REQUIRE(match.matchedEventIndices.size() == 4);

    live.mechanicalEvents.erase(live.mechanicalEvents.begin() + 1);
    replay.controlGroupSelections.erase(replay.controlGroupSelections.begin() + 2);
    match = smp::identifyReplayPlayer(live.mechanicalEvents, replay);
    REQUIRE(match.available);
    REQUIRE(match.playerId == 0);
    REQUIRE(match.matchedEventIndices.size() >= 2);
}

TEST_CASE("ambiguous replay players fail correlation instead of guessing or using a name") {
    smp::AnalysisResult live;
    smp::ReplayData replay;
    replay.players = {{3, "first"}, {7, "second"}};
    for (int index = 0; index < 4; ++index) {
        select(live.mechanicalEvents, index + 1, static_cast<std::uint64_t>(index * 1000));
        replay.controlGroupSelections.push_back({index * 24, 3, index + 1});
        replay.controlGroupSelections.push_back({index * 24, 7, index + 1});
    }
    const auto match = smp::identifyReplayPlayer(live.mechanicalEvents, replay);
    REQUIRE(!match.available);
    REQUIRE(match.unavailableReason.find("ambiguous") != std::string::npos);
}

TEST_CASE("one control-group selection is insufficient to identify a replay player") {
    smp::AnalysisResult live;
    select(live.mechanicalEvents, 4, 1000);
    auto replay = replayWithPlayers();
    replay.controlGroupSelections.push_back({24, 0, 4});
    const auto match = smp::identifyReplayPlayer(live.mechanicalEvents, replay);
    REQUIRE(!match.available);
    REQUIRE(match.unavailableReason.find("too few") != std::string::npos);
}

TEST_CASE("realistic screp JSON extracts selection production and scout geometry") {
    const std::string fixture = R"json({
      "Header":{"Frames":400,"MapWidth":128,"MapHeight":64,
        "Players":[{"ID":0,"Name":"P","SlotID":3,"Race":{"Name":"Protoss"}},
                   {"ID":1,"Name":"Z","SlotID":5,"Race":{"Name":"Zerg"}}]},
      "MapData":{"StartLocations":[{"SlotID":3,"X":3808,"Y":1760}]},
      "Commands":{"Cmds":[
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Hotkey"},"HotkeyType":{"Name":"Select"},"Group":4},
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Hotkey"},"HotkeyType":{"Name":"Assign"},"Group":1},
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Hotkey"},"HotkeyType":{"Name":"Add"},"Group":2},
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Select"},"UnitTags":[1001]},
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Select Add"},"UnitTags":[1002]},
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Select Remove"},"UnitTags":[1001]},
        {"Frame":11,"PlayerID":0,"Type":{"Name":"Train"},"Unit":{"Name":"Probe","ID":64}},
        {"Frame":12,"PlayerID":0,"Type":{"Name":"Unit Morph"},"Unit":{"Name":"Hydralisk","ID":38}},
        {"Frame":13,"PlayerID":0,"Type":{"Name":"Build"},"Order":{"Name":"PlaceProtossBuilding"},"Unit":{"Name":"Nexus","ID":154}},
        {"Frame":14,"PlayerID":0,"Type":{"Name":"Build"},"Order":"PlaceBuilding","Unit":"Barracks"},
        {"Frame":15,"PlayerID":0,"Type":{"Name":"Upgrade"}},
        {"Frame":16,"PlayerID":0,"Type":{"Name":"Train Fighter"}},
        {"Frame":17,"PlayerID":0,"Type":{"Name":"Right Click"},"Pos":{"X":2048,"Y":1024}}
      ]}}
    )json";
    const auto replay = smp::parseScrepReplayJson(fixture);
    REQUIRE(replay.players.size() == 2);
    REQUIRE(replay.players[0].slotId == 3);
    REQUIRE(replay.players[0].race == "Protoss");
    REQUIRE_NEAR(replay.mapWidthPixels, 4096.0, 0.001);
    REQUIRE_NEAR(replay.mapHeightPixels, 2048.0, 0.001);
    REQUIRE(replay.startLocations.size() == 1);
    REQUIRE(replay.startLocations[0].slotId == 3);
    REQUIRE_NEAR(replay.startLocations[0].x, 3808.0, 0.001);
    REQUIRE(replay.unitCommands.size() == 1);
    REQUIRE(replay.unitCommands[0].playerId == 0);
    REQUIRE(replay.unitCommands[0].kind == "Right Click");
    REQUIRE_NEAR(*replay.unitCommands[0].targetX, 2048.0, 0.001);
    REQUIRE(replay.buildEvents.size() == 2);
    REQUIRE(replay.buildEvents[0].replayFrame == 13);
    REQUIRE(replay.buildEvents[0].order == "PlaceProtossBuilding");
    REQUIRE(replay.buildEvents[0].unit == "Nexus");
    REQUIRE(replay.buildEvents[1].order == "PlaceBuilding");
    REQUIRE(replay.buildEvents[1].unit == "Barracks");
    REQUIRE(replay.controlGroupSelections.size() == 1);
    REQUIRE(replay.controlGroupEdits.size() == 2);
    REQUIRE(replay.controlGroupEdits[0].operation == smp::ArmyControlGroupOperation::Assign);
    REQUIRE(replay.controlGroupEdits[1].operation == smp::ArmyControlGroupOperation::Add);
    REQUIRE(replay.selections.size() == 3);
    REQUIRE(replay.selections[0].kind == smp::ReplaySelectionKind::Select);
    REQUIRE(replay.selections[1].kind == smp::ReplaySelectionKind::Add);
    REQUIRE(replay.selections[2].kind == smp::ReplaySelectionKind::Remove);
    REQUIRE(replay.selections[0].unitTags[0] == 1001);
    REQUIRE(replay.productionEvents.size() == 3);
    REQUIRE(replay.productionEvents[0].unit == "Probe");
    REQUIRE(replay.productionEvents[1].unit == "Hydralisk");
    REQUIRE(replay.productionEvents[2].unit == "Interceptor/Scarab");
}

TEST_CASE("missing replay and malformed replay-parser output fail closed") {
    const auto missingPath = std::filesystem::temp_directory_path() /
                             "starcraft-mechanics-profiler-definitely-missing.rep";
    std::error_code ignored;
    std::filesystem::remove(missingPath, ignored);
    const auto missing = smp::extractReplayWithBundledScrep(missingPath);
    REQUIRE(!missing.available);
    REQUIRE(missing.unavailableReason.find("missing") != std::string::npos);

    bool malformedRejected = false;
    try {
        (void)smp::parseScrepReplayJson("{not replay JSON");
    } catch (...) {
        malformedRejected = true;
    }
    REQUIRE(malformedRejected);

    bool incompleteRejected = false;
    try {
        (void)smp::parseScrepReplayJson(R"json({"Header":{"Frames":10}})json");
    } catch (...) {
        incompleteRejected = true;
    }
    REQUIRE(incompleteRejected);
}

TEST_CASE("CG4 E is replay-confirmed as Worker while keeping physical duration and false presses") {
    smp::AnalysisResult live;
    smp::ReplayData replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    addAnchor(live, replay, 4, 2000, 48);
    key(live.mechanicalEvents, 'E', 2100);
    key(live.mechanicalEvents, 'E', 2200);
    key(live.mechanicalEvents, 'E', 2300);
    addAnchor(live, replay, 3, 3000, 72);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back({51, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
    replay.productionEvents.push_back({53, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});

    auto analyzed = correlate(live, replay, heuristicBase(live, established({{4, 'E'}})));
    REQUIRE(analyzed.replayCorrelation.available);
    REQUIRE(analyzed.productionVisits.size() == 1);
    const auto& visit = analyzed.productionVisits[0];
    REQUIRE(visit.productType == smp::MacroProductType::Worker);
    REQUIRE(visit.physicalProductionPresses == 3);
    REQUIRE(visit.replayProductionCommands == 2);
    REQUIRE_NEAR(visit.endActiveMs, 2300.0, 0.001);
}

TEST_CASE("CG5 D D D is replay-confirmed as one Army ProductionVisit") {
    smp::AnalysisResult live;
    smp::ReplayData replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    addAnchor(live, replay, 5, 2000, 48);
    key(live.mechanicalEvents, 'D', 2100);
    key(live.mechanicalEvents, 'D', 2200);
    key(live.mechanicalEvents, 'D', 2300);
    addAnchor(live, replay, 3, 3000, 72);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back({51, 0, smp::ReplayProductionKind::Train, "Dragoon", 0x42});
    replay.productionEvents.push_back({53, 0, smp::ReplayProductionKind::Train, "Dragoon", 0x42});
    replay.productionEvents.push_back({55, 0, smp::ReplayProductionKind::Train, "Dragoon", 0x42});
    const auto analyzed = correlate(live, replay, heuristicBase(live, established({{5, 'D'}})));
    REQUIRE(analyzed.productionVisits.size() == 1);
    REQUIRE(analyzed.productionVisits[0].productType == smp::MacroProductType::Army);
    REQUIRE(analyzed.productionVisits[0].producedUnits.size() == 3);
    REQUIRE(analyzed.armyMacroCycles.cycles.size() == 1);
}

TEST_CASE("replay confirmation extends a long continuous Probe burst beyond 750 ms") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    addAnchor(live, replay, 4, 2000, 48);
    for (const auto ticks : {2400ULL, 2520ULL, 2680ULL, 2810ULL, 2930ULL, 3050ULL, 3170ULL})
        key(live.mechanicalEvents, 'E', ticks);
    addAnchor(live, replay, 3, 4000, 96);
    addWrongPlayerReverseAnchors(replay);
    for (const auto frame : {58, 61, 64, 67, 70, 73, 76})
        replay.productionEvents.push_back(
            {frame, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});

    const auto analyzed = correlate(live, replay, heuristicBase(live, established({{4, 'E'}})));
    REQUIRE(analyzed.productionVisits.size() == 1);
    const auto& production = analyzed.productionVisits[0];
    REQUIRE(production.productType == smp::MacroProductType::Worker);
    REQUIRE(production.controlGroup == 4);
    REQUIRE(production.physicalProductionPresses == 7);
    REQUIRE(production.replayProductionCommands == 7);
    REQUIRE_NEAR(production.firstProductionActiveMs, 2400.0, 0.001);
    REQUIRE_NEAR(production.endActiveMs, 3170.0, 0.001);
    REQUIRE_NEAR(production.executionDurationMs, 400.0, 0.001);
    REQUIRE_NEAR(production.productionBurstSpanMs, 770.0, 0.001);
    REQUIRE_NEAR(production.durationMs, 1170.0, 0.001);
    REQUIRE(analyzed.workerMacroCycles.cycles.size() == 1);
    REQUIRE_NEAR(analyzed.workerMacroCycles.cycles[0].durationMs, 400.0, 0.001);
    REQUIRE_NEAR(analyzed.workerMacroCycles.cycles[0].fullSpanMs, 1170.0, 0.001);
    REQUIRE(analyzed.replayCorrelation.extendedProductionVisits == 1);
    REQUIRE(analyzed.replayCorrelation.extendedPhysicalProductionPresses == 4);
}

TEST_CASE("confirmed production preserves compatible failed presses without requiring replay commands") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    addAnchor(live, replay, 4, 2000, 48);
    for (const auto ticks : {2400ULL, 2520ULL, 2640ULL, 2760ULL, 2880ULL, 3000ULL})
        key(live.mechanicalEvents, 'E', ticks);
    addAnchor(live, replay, 3, 4000, 96);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back({58, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
    replay.productionEvents.push_back({61, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});

    const auto analyzed = correlate(live, replay, heuristicBase(live, established({{4, 'E'}})));
    REQUIRE(analyzed.productionVisits.size() == 1);
    REQUIRE(analyzed.productionVisits[0].physicalProductionPresses == 6);
    REQUIRE(analyzed.productionVisits[0].replayProductionCommands == 2);
    REQUIRE_NEAR(analyzed.productionVisits[0].endActiveMs, 3000.0, 0.001);
    REQUIRE(analyzed.replayCorrelation.extendedPhysicalProductionPresses == 3);
}

TEST_CASE("a gap beyond the continuation threshold ends a confirmed production burst") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    addAnchor(live, replay, 4, 2000, 48);
    key(live.mechanicalEvents, 'E', 2400);
    key(live.mechanicalEvents, 'E', 2520);
    key(live.mechanicalEvents, 'E', 3200);
    addAnchor(live, replay, 3, 4000, 96);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back({58, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
    replay.productionEvents.push_back({61, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
    replay.productionEvents.push_back({77, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});

    const auto analyzed = correlate(live, replay, heuristicBase(live, established({{4, 'E'}})));
    REQUIRE(analyzed.productionVisits.size() == 1);
    REQUIRE(analyzed.productionVisits[0].physicalProductionPresses == 2);
    REQUIRE_NEAR(analyzed.productionVisits[0].endActiveMs, 2520.0, 0.001);
    REQUIRE(analyzed.replayCorrelation.matchedReplayProductionEvents == 2);
    REQUIRE(analyzed.replayCorrelation.unmatchedReplayProductionEvents == 1);
    REQUIRE(analyzed.replayCorrelation.extendedProductionVisits == 0);
}

TEST_CASE("a control-group context switch separates confirmed Worker and Army bursts") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    addAnchor(live, replay, 4, 2000, 48);
    key(live.mechanicalEvents, 'E', 2100);
    key(live.mechanicalEvents, 'E', 2200);
    addAnchor(live, replay, 5, 2400, 58);
    key(live.mechanicalEvents, 'Z', 2500);
    key(live.mechanicalEvents, 'Z', 2600);
    addAnchor(live, replay, 3, 4000, 96);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back({51, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
    replay.productionEvents.push_back({53, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
    replay.productionEvents.push_back({61, 0, smp::ReplayProductionKind::Train, "Zealot", 0x41});
    replay.productionEvents.push_back({63, 0, smp::ReplayProductionKind::Train, "Zealot", 0x41});

    const auto analyzed = correlate(live, replay, heuristicBase(live, {}));
    REQUIRE(analyzed.productionVisits.size() == 2);
    REQUIRE(analyzed.productionVisits[0].productType == smp::MacroProductType::Worker);
    REQUIRE_NEAR(analyzed.productionVisits[0].endActiveMs, 2200.0, 0.001);
    REQUIRE(analyzed.productionVisits[1].productType == smp::MacroProductType::Army);
    REQUIRE(analyzed.productionVisits[1].controlGroup == 5);
}

TEST_CASE("replay-confirmed location click extends through its continuous physical burst") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    live.mechanicalEvents.push_back(
        mechanical(smp::MechanicalInputType::LocationRecall, 1800, VK_F3, 3));
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftDown, 2000));
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftUp, 2020));
    for (const auto ticks : {2200ULL, 2500ULL, 2800ULL, 3100ULL})
        key(live.mechanicalEvents, 'E', ticks);
    addAnchor(live, replay, 3, 4000, 96);
    addAnchor(live, replay, 4, 5000, 120);
    addWrongPlayerReverseAnchors(replay);
    addReplaySelection(replay, 48);
    for (const auto frame : {53, 60, 67, 74})
        replay.productionEvents.push_back(
            {frame, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});

    const auto analyzed = correlate(live, replay, heuristicBase(live, {}));
    REQUIRE(analyzed.productionVisits.size() == 1);
    const auto& production = analyzed.productionVisits[0];
    REQUIRE(production.productType == smp::MacroProductType::Worker);
    REQUIRE(production.accessMethod == smp::ProductionAccessMethod::LocationHotkeyClick);
    REQUIRE_NEAR(production.startActiveMs, 1800.0, 0.001);
    REQUIRE_NEAR(production.contextActiveMs, 2000.0, 0.001);
    REQUIRE_NEAR(production.endActiveMs, 3100.0, 0.001);
    REQUIRE(production.physicalProductionPresses == 4);
}

TEST_CASE("replay semantics reject an incompatible production key inside a confirmed burst") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    addAnchor(live, replay, 4, 2000, 48);
    key(live.mechanicalEvents, 'E', 2100);
    key(live.mechanicalEvents, 'E', 2200);
    key(live.mechanicalEvents, 'D', 2300);
    addAnchor(live, replay, 3, 3000, 72);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back({51, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
    replay.productionEvents.push_back({53, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});

    const auto analyzed = correlate(live, replay, heuristicBase(live, established({{4, 'E'}})));
    REQUIRE(analyzed.productionVisits.size() == 1);
    REQUIRE(analyzed.productionVisits[0].physicalProductionPresses == 2);
    const std::vector<std::uint16_t> expectedKeys{'E', 'E'};
    REQUIRE(analyzed.productionVisits[0].physicalProductionKeys == expectedKeys);
    REQUIRE_NEAR(analyzed.productionVisits[0].endActiveMs, 2200.0, 0.001);
}

TEST_CASE("harmless mouse activity does not split a replay-confirmed production burst") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    addAnchor(live, replay, 4, 2000, 48);
    key(live.mechanicalEvents, 'E', 2100);
    live.mechanicalEvents.push_back(
        mechanical(smp::MechanicalInputType::MouseWheel, 2150));
    key(live.mechanicalEvents, 'E', 2200);
    addAnchor(live, replay, 3, 3000, 72);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back({51, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
    replay.productionEvents.push_back({53, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});

    const auto analyzed = correlate(live, replay, heuristicBase(live, {}));
    REQUIRE(analyzed.productionVisits.size() == 1);
    REQUIRE(analyzed.productionVisits[0].physicalProductionPresses == 2);
    REQUIRE_NEAR(analyzed.productionVisits[0].endActiveMs, 2200.0, 0.001);
}

TEST_CASE("a QPC active-time discontinuity stops confirmed burst continuation") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    addAnchor(live, replay, 4, 2000, 48);
    key(live.mechanicalEvents, 'E', 2100);
    key(live.mechanicalEvents, 'E', 2200);
    auto afterPause = mechanical(smp::MechanicalInputType::KeyPress, 5000, 'E');
    afterPause.activeMs = 2300.0;
    live.mechanicalEvents.push_back(afterPause);
    addAnchor(live, replay, 3, 6000, 72);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back({49, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
    replay.productionEvents.push_back({50, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});

    const auto analyzed = correlate(live, replay, heuristicBase(live, established({{4, 'E'}})));
    REQUIRE(analyzed.productionVisits.size() == 1);
    REQUIRE(analyzed.productionVisits[0].physicalProductionPresses == 2);
    REQUIRE_NEAR(analyzed.productionVisits[0].endActiveMs, 2200.0, 0.001);
    REQUIRE(analyzed.replayCorrelation.extendedProductionVisits == 0);
}

TEST_CASE("without replay confirmation long physical input remains limited to the short window") {
    smp::AnalysisResult live;
    select(live.mechanicalEvents, 4, 2000);
    for (const auto ticks : {2400ULL, 2520ULL, 2680ULL, 2810ULL, 2930ULL, 3050ULL, 3170ULL})
        key(live.mechanicalEvents, 'E', ticks);
    const auto visits = smp::detectHeuristicProductionVisitsForLikelyGroups(
        live, profile(), testQpcFrequency, established({{4, 'E'}}));
    REQUIRE(visits.size() == 1);
    REQUIRE(visits[0].physicalProductionPresses == 3);
    REQUIRE_NEAR(visits[0].endActiveMs, 2680.0, 0.001);
    REQUIRE(visits[0].durationMs <= smp::productionVisitWindowMs);
}

TEST_CASE("replay evidence recovers a one-off control-group visit missed by the heuristic") {
    smp::AnalysisResult live;
    smp::ReplayData replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    live.mechanicalEvents.push_back(
        mechanical(smp::MechanicalInputType::ControlGroupAssign, 1500, '8', 8,
                   smp::ModifierCtrl));
    addAnchor(live, replay, 8, 2000, 48);
    key(live.mechanicalEvents, 'V', 2100);
    addAnchor(live, replay, 3, 3000, 72);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back(
        {51, 0, smp::ReplayProductionKind::Train, "Reaver", 0x53});

    const auto analyzed = correlate(live, replay, heuristicBase(live, {}));
    REQUIRE(analyzed.productionVisits.size() == 1);
    REQUIRE(analyzed.productionVisits[0].replayConfirmed);
    REQUIRE(analyzed.productionVisits[0].productType == smp::MacroProductType::Army);
    REQUIRE(analyzed.productionVisits[0].controlGroup == 8);
    REQUIRE(analyzed.productionVisits[0].productionContext.kind ==
            smp::ProductionContextKind::ControlGroup);
    REQUIRE(analyzed.productionVisits[0].productionContext.assignmentGeneration == 1);
    REQUIRE(analyzed.productionVisits[0].physicalProductionKeys.size() == 1);
    REQUIRE(analyzed.productionVisits[0].physicalProductionKeys[0] == 'V');
    REQUIRE(analyzed.replayCorrelation.replayCreatedControlGroupVisits == 1);
}

TEST_CASE("ordered context and physical keys keep rapid worker and army visits correctly matched") {
    smp::AnalysisResult live;
    smp::ReplayData replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    addAnchor(live, replay, 4, 2000, 48);
    key(live.mechanicalEvents, 'E', 2050);
    addAnchor(live, replay, 5, 2150, 54);
    key(live.mechanicalEvents, 'D', 2250);
    key(live.mechanicalEvents, 'D', 2300);
    addAnchor(live, replay, 3, 3000, 72);
    addWrongPlayerReverseAnchors(replay);
    // Probe maps closer to the later group 5 visit, so nearest-time-only matching would be risky.
    replay.productionEvents.push_back(
        {53, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
    replay.productionEvents.push_back(
        {56, 0, smp::ReplayProductionKind::Train, "Dragoon", 0x42});
    replay.productionEvents.push_back(
        {58, 0, smp::ReplayProductionKind::Train, "Dragoon", 0x42});

    const auto analyzed = correlate(live, replay, heuristicBase(live, {}));
    REQUIRE(analyzed.productionVisits.size() == 2);
    REQUIRE(analyzed.productionVisits[0].controlGroup == 4);
    REQUIRE(analyzed.productionVisits[0].productType == smp::MacroProductType::Worker);
    REQUIRE(analyzed.productionVisits[0].producedUnits.size() == 1);
    REQUIRE(analyzed.productionVisits[0].producedUnits[0] == "Probe");
    REQUIRE(analyzed.productionVisits[1].controlGroup == 5);
    REQUIRE(analyzed.productionVisits[1].productType == smp::MacroProductType::Army);
    REQUIRE(analyzed.productionVisits[1].producedUnits.size() == 2);
    REQUIRE(analyzed.productionVisits[1].producedUnits[0] == "Dragoon");
}

TEST_CASE("physical-key compatibility accepts shared E semantics and rejects impossible D Probe") {
    const auto hotkeys = profile();
    const smp::ReplayProductionEvent probe{0, 0, smp::ReplayProductionKind::Train,
                                           "Probe", 0x40};
    const smp::ReplayProductionEvent corsair{0, 0, smp::ReplayProductionKind::Train,
                                             "Corsair", 0x3c};
    const smp::ReplayProductionEvent dragoon{0, 0, smp::ReplayProductionKind::Train,
                                             "Dragoon", 0x42};
    const smp::ReplayProductionEvent fighter{
        0, 0, smp::ReplayProductionKind::TrainFighter, "Interceptor/Scarab", -1};
    REQUIRE(smp::replayProductionCompatibleWithPhysicalKey(probe, 'E', hotkeys));
    REQUIRE(smp::replayProductionCompatibleWithPhysicalKey(corsair, 'E', hotkeys));
    REQUIRE(smp::replayProductionCompatibleWithPhysicalKey(dragoon, 'D', hotkeys));
    REQUIRE(!smp::replayProductionCompatibleWithPhysicalKey(probe, 'D', hotkeys));
    REQUIRE(smp::replayProductionCompatibleWithPhysicalKey(fighter, 'I', hotkeys));
    REQUIRE(smp::replayProductionCompatibleWithPhysicalKey(fighter, 'R', hotkeys));

    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    addAnchor(live, replay, 8, 2000, 48);
    key(live.mechanicalEvents, 'D', 2100);
    addAnchor(live, replay, 3, 3000, 72);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back(
        {51, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
    const auto analyzed = correlate(live, replay, heuristicBase(live, {}));
    REQUIRE(analyzed.productionVisits.empty());
    REQUIRE(analyzed.replayCorrelation.matchedReplayProductionEvents == 0);
}

TEST_CASE("location recall click uses replay semantics for ambiguous E worker versus army") {
    const auto run = [](const char* unit, int unitId, smp::MacroProductType expected) {
        smp::AnalysisResult live;
        smp::ReplayData replay = replayWithPlayers();
        addAnchor(live, replay, 1, 0, 0);
        addAnchor(live, replay, 2, 1000, 24);
        live.mechanicalEvents.push_back(mechanical(
            smp::MechanicalInputType::LocationAssign, 1500, VK_F3, 3,
            smp::ModifierShift));
        live.mechanicalEvents.push_back(
            mechanical(smp::MechanicalInputType::LocationRecall, 1900, VK_F3, 3));
        live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftDown, 2050));
        live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftUp, 2070));
        key(live.mechanicalEvents, 'E', 2150);
        addAnchor(live, replay, 3, 3000, 72);
        addAnchor(live, replay, 4, 4000, 96);
        addWrongPlayerReverseAnchors(replay);
        addReplaySelection(replay, 49);
        replay.productionEvents.push_back({52, 0, smp::ReplayProductionKind::Train, unit, unitId});
        const auto analyzed = correlate(live, replay, heuristicBase(live, {}));
        REQUIRE(analyzed.productionVisits.size() == 1);
        REQUIRE(analyzed.productionVisits[0].productType == expected);
        REQUIRE(analyzed.productionVisits[0].accessMethod ==
                smp::ProductionAccessMethod::LocationHotkeyClick);
        REQUIRE(analyzed.productionVisits[0].locationHotkey == 3);
        REQUIRE(analyzed.productionVisits[0].productionContext.kind ==
                smp::ProductionContextKind::ReplaySelection);
        REQUIRE(analyzed.productionVisits[0].productionContext.unitTags ==
                std::vector<std::uint32_t>{100});
        REQUIRE_NEAR(analyzed.productionVisits[0].startActiveMs, 1900.0, 0.001);
        REQUIRE_NEAR(analyzed.productionVisits[0].contextActiveMs, 2050.0, 0.001);
    };
    run("Probe", 0x40, smp::MacroProductType::Worker);
    run("Corsair", 0x3c, smp::MacroProductType::Army);
}

TEST_CASE("location hotkey remains the fallback identity when replay has no full Select tag set") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    live.mechanicalEvents.push_back(
        mechanical(smp::MechanicalInputType::LocationRecall, 1900, VK_F3, 3));
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftDown, 2050));
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftUp, 2070));
    key(live.mechanicalEvents, 'E', 2150);
    addAnchor(live, replay, 3, 3000, 72);
    addAnchor(live, replay, 4, 4000, 96);
    addWrongPlayerReverseAnchors(replay);
    addReplaySelection(replay, 49, smp::ReplaySelectionKind::Add, {100});
    replay.productionEvents.push_back(
        {52, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});

    const auto analyzed = correlate(live, replay, heuristicBase(live, {}));
    REQUIRE(analyzed.productionVisits.size() == 1);
    REQUIRE(analyzed.productionVisits[0].productionContext.kind ==
            smp::ProductionContextKind::LocationHotkey);
    REQUIRE(analyzed.productionVisits[0].productionContext.locationHotkey == 3);
    REQUIRE(analyzed.productionVisits[0].productionContext.assignmentGeneration == 0);
}

TEST_CASE("location fallback identity uses the assignment generation at the initiating recall") {
    const auto run = [](int assignedLocation) {
        smp::AnalysisResult live;
        auto replay = replayWithPlayers();
        addAnchor(live, replay, 1, 0, 0);
        addAnchor(live, replay, 2, 1000, 24);
        live.mechanicalEvents.push_back(
            mechanical(smp::MechanicalInputType::LocationRecall, 1900, VK_F3, 3));
        live.mechanicalEvents.push_back(
            mechanical(smp::MechanicalInputType::MouseLeftDown, 2050));
        live.mechanicalEvents.push_back(
            mechanical(smp::MechanicalInputType::MouseLeftUp, 2070));
        key(live.mechanicalEvents, 'E', 2150);
        live.mechanicalEvents.push_back(mechanical(
            smp::MechanicalInputType::LocationAssign, 2600,
            static_cast<std::uint16_t>(VK_F1 + assignedLocation - 1), assignedLocation,
            smp::ModifierShift));
        addAnchor(live, replay, 3, 3000, 72);
        live.mechanicalEvents.push_back(
            mechanical(smp::MechanicalInputType::LocationRecall, 3400, VK_F3, 3));
        live.mechanicalEvents.push_back(mechanical(
            smp::MechanicalInputType::LocationAssign, 3500, VK_F3, 3,
            smp::ModifierShift));
        live.mechanicalEvents.push_back(
            mechanical(smp::MechanicalInputType::MouseLeftDown, 3550));
        live.mechanicalEvents.push_back(
            mechanical(smp::MechanicalInputType::MouseLeftUp, 3570));
        key(live.mechanicalEvents, 'E', 3650);
        addAnchor(live, replay, 4, 5000, 120);
        addWrongPlayerReverseAnchors(replay);
        addReplaySelection(replay, 49, smp::ReplaySelectionKind::Add, {100});
        replay.productionEvents.push_back(
            {52, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
        addReplaySelection(replay, 85, smp::ReplaySelectionKind::Add, {200});
        replay.productionEvents.push_back(
            {88, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
        return correlate(live, replay, heuristicBase(live, {}));
    };

    const auto reassigned = run(3);
    REQUIRE(reassigned.productionVisits.size() == 2);
    REQUIRE(reassigned.productionVisits[0].productionContext.assignmentGeneration == 0);
    REQUIRE(reassigned.productionVisits[1].productionContext.assignmentGeneration == 1);
    REQUIRE(!smp::sameProductionContext(reassigned.productionVisits[0].productionContext,
                                        reassigned.productionVisits[1].productionContext));

    const auto unrelatedAssignment = run(4);
    REQUIRE(unrelatedAssignment.productionVisits.size() == 2);
    REQUIRE(unrelatedAssignment.productionVisits[0].productionContext.assignmentGeneration == 0);
    REQUIRE(unrelatedAssignment.productionVisits[1].productionContext.assignmentGeneration == 0);
    REQUIRE(smp::sameProductionContext(unrelatedAssignment.productionVisits[0].productionContext,
                                       unrelatedAssignment.productionVisits[1].productionContext));
}

TEST_CASE("minimap and direct screen clicks are classified as distinct access methods") {
    const auto run = [](bool minimap) {
        smp::AnalysisResult live;
        smp::ReplayData replay = replayWithPlayers();
        addAnchor(live, replay, 1, 0, 0);
        addAnchor(live, replay, 2, 1000, 24);
        if (minimap)
            live.navigationEvents.push_back(
                {1800, 1800.0, smp::CameraNavigationType::MinimapJump, -1, 300, 900});
        live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftDown, 2050));
        live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftUp, 2070));
        key(live.mechanicalEvents, 'D', 2150);
        addAnchor(live, replay, 3, 3000, 72);
        addAnchor(live, replay, 4, 4000, 96);
        addWrongPlayerReverseAnchors(replay);
        addReplaySelection(replay, 49);
        replay.productionEvents.push_back({52, 0, smp::ReplayProductionKind::Train, "Dragoon", 0x42});
        return correlate(live, replay, heuristicBase(live, {}));
    };
    const auto minimap = run(true);
    REQUIRE(minimap.productionVisits[0].accessMethod == smp::ProductionAccessMethod::MinimapClick);
    REQUIRE_NEAR(minimap.productionVisits[0].startActiveMs, 1800.0, 0.001);
    REQUIRE_NEAR(minimap.productionVisits[0].contextActiveMs, 2050.0, 0.001);
    const auto screen = run(false);
    REQUIRE(screen.productionVisits[0].accessMethod == smp::ProductionAccessMethod::ScreenClick);
    REQUIRE(screen.productionVisits[0].selectionAccess ==
            smp::ProductionSelectionAccess::DirectClick);
    REQUIRE_NEAR(screen.productionVisits[0].startActiveMs, 2050.0, 0.001);
    REQUIRE_NEAR(screen.productionVisits[0].contextActiveMs, 2050.0, 0.001);
}

TEST_CASE("drag-selected production context is retained as box selection access") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    live.mechanicalEvents.push_back(mechanical(
        smp::MechanicalInputType::MouseLeftDown, 2050, 0, -1,
        smp::ModifierNone, 100, 100));
    live.mechanicalEvents.push_back(mechanical(
        smp::MechanicalInputType::MouseLeftUp, 2070, 0, -1,
        smp::ModifierNone, 140, 130));
    key(live.mechanicalEvents, 'D', 2150);
    addAnchor(live, replay, 3, 3000, 72);
    addAnchor(live, replay, 4, 4000, 96);
    addWrongPlayerReverseAnchors(replay);
    addReplaySelection(replay, 49);
    replay.productionEvents.push_back(
        {52, 0, smp::ReplayProductionKind::Train, "Dragoon", 0x42});

    const auto analyzed = correlate(live, replay, heuristicBase(live, {}));
    REQUIRE(analyzed.productionVisits.size() == 1);
    REQUIRE(analyzed.productionVisits[0].selectionAccess ==
            smp::ProductionSelectionAccess::BoxSelect);
}

TEST_CASE("most recent qualifying location or minimap action wins access precedence") {
    smp::AnalysisResult live;
    smp::ReplayData replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::LocationRecall, 1500, VK_F3, 3));
    live.navigationEvents.push_back(
        {1900, 1900.0, smp::CameraNavigationType::MinimapJump, -1, 300, 900});
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftDown, 2050));
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftUp, 2070));
    key(live.mechanicalEvents, 'D', 2150);
    key(live.mechanicalEvents, 'D', 2200);
    addAnchor(live, replay, 3, 3000, 72);
    addAnchor(live, replay, 4, 4000, 96);
    addWrongPlayerReverseAnchors(replay);
    addReplaySelection(replay, 49);
    replay.productionEvents.push_back({52, 0, smp::ReplayProductionKind::Train, "Dragoon", 0x42});
    const auto analyzed = correlate(live, replay, heuristicBase(live, {}));
    REQUIRE(analyzed.productionVisits[0].accessMethod == smp::ProductionAccessMethod::MinimapClick);
    REQUIRE(analyzed.productionVisits[0].productType == smp::MacroProductType::Army);
    REQUIRE(analyzed.productionVisits[0].physicalProductionPresses == 2);
}

TEST_CASE("click production requires an ordered replay selection before production") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    live.mechanicalEvents.push_back(
        mechanical(smp::MechanicalInputType::LocationRecall, 1900, VK_F3, 3));
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftDown, 2050));
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftUp, 2070));
    key(live.mechanicalEvents, 'A', 2150);
    addAnchor(live, replay, 3, 3000, 72);
    addAnchor(live, replay, 4, 4000, 96);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back(
        {52, 0, smp::ReplayProductionKind::Train, "Arbiter", 0x47});

    const auto temporalOnly = correlate(live, replay, heuristicBase(live, {}));
    REQUIRE(temporalOnly.productionVisits.empty());

    // A selection after the Train command cannot validate the earlier click-production sequence.
    addReplaySelection(replay, 53);
    const auto wrongOrder = correlate(live, replay, heuristicBase(live, {}));
    REQUIRE(wrongOrder.productionVisits.empty());
}

TEST_CASE("later meaningful navigation invalidates stale click access context") {
    enum class Scenario {
        LocationThenEdge,
        MinimapThenControlGroup,
        EdgeThenLocation,
        LocationThenMinimap,
    };
    const auto run = [](Scenario scenario) {
        smp::AnalysisResult live;
        auto replay = replayWithPlayers();
        addAnchor(live, replay, 1, 0, 0);
        addAnchor(live, replay, 2, 1000, 24);
        if (scenario == Scenario::LocationThenEdge ||
            scenario == Scenario::LocationThenMinimap) {
            live.mechanicalEvents.push_back(
                mechanical(smp::MechanicalInputType::LocationRecall, 1500, VK_F3, 3));
        }
        if (scenario == Scenario::MinimapThenControlGroup) {
            live.navigationEvents.push_back(
                {1500, 1500.0, smp::CameraNavigationType::MinimapJump, -1, 300, 900});
        }
        if (scenario == Scenario::LocationThenEdge || scenario == Scenario::EdgeThenLocation) {
            const std::uint64_t edgeTicks =
                scenario == Scenario::LocationThenEdge ? 1900 : 1500;
            live.navigationEvents.push_back(
                {edgeTicks, static_cast<double>(edgeTicks),
                 smp::CameraNavigationType::EdgeScroll, -1, 0, 500});
        }
        if (scenario == Scenario::MinimapThenControlGroup) {
            live.navigationEvents.push_back(
                {1900, 1900.0, smp::CameraNavigationType::ControlGroupJump, 1, 500, 500});
        }
        if (scenario == Scenario::EdgeThenLocation) {
            live.mechanicalEvents.push_back(
                mechanical(smp::MechanicalInputType::LocationRecall, 1900, VK_F3, 3));
        }
        if (scenario == Scenario::LocationThenMinimap) {
            live.navigationEvents.push_back(
                {1900, 1900.0, smp::CameraNavigationType::MinimapJump, -1, 300, 900});
        }
        live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftDown, 2050));
        live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftUp, 2070));
        key(live.mechanicalEvents, 'E', 2150);
        addAnchor(live, replay, 3, 3000, 72);
        addAnchor(live, replay, 4, 4000, 96);
        addWrongPlayerReverseAnchors(replay);
        addReplaySelection(replay, 49);
        replay.productionEvents.push_back(
            {52, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
        return correlate(live, replay, heuristicBase(live, {}));
    };

    const auto locationThenEdge = run(Scenario::LocationThenEdge);
    REQUIRE(locationThenEdge.productionVisits.size() == 1);
    REQUIRE(locationThenEdge.productionVisits[0].accessMethod ==
            smp::ProductionAccessMethod::ScreenClick);
    const auto minimapThenControlGroup = run(Scenario::MinimapThenControlGroup);
    REQUIRE(minimapThenControlGroup.productionVisits.size() == 1);
    REQUIRE(minimapThenControlGroup.productionVisits[0].accessMethod ==
            smp::ProductionAccessMethod::ScreenClick);
    const auto edgeThenLocation = run(Scenario::EdgeThenLocation);
    REQUIRE(edgeThenLocation.productionVisits.size() == 1);
    REQUIRE(edgeThenLocation.productionVisits[0].accessMethod ==
            smp::ProductionAccessMethod::LocationHotkeyClick);
    const auto locationThenMinimap = run(Scenario::LocationThenMinimap);
    REQUIRE(locationThenMinimap.productionVisits.size() == 1);
    REQUIRE(locationThenMinimap.productionVisits[0].accessMethod ==
            smp::ProductionAccessMethod::MinimapClick);
}

TEST_CASE("production correlation orders a click by its selection context instead of earlier navigation") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 500, 12);
    live.mechanicalEvents.push_back(
        mechanical(smp::MechanicalInputType::LocationRecall, 1000, VK_F3, 3));
    addAnchor(live, replay, 5, 1300, 31);
    key(live.mechanicalEvents, 'D', 1400);
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftDown, 1700));
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftUp, 1720));
    key(live.mechanicalEvents, 'E', 1800);
    addAnchor(live, replay, 3, 3000, 72);
    addAnchor(live, replay, 4, 4000, 96);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back(
        {34, 0, smp::ReplayProductionKind::Train, "Dragoon", 0x42});
    addReplaySelection(replay, 41);
    replay.productionEvents.push_back(
        {43, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});

    const auto analyzed = correlate(live, replay, heuristicBase(live, established({{5, 'D'}})));
    REQUIRE(analyzed.productionVisits.size() == 2);
    REQUIRE(analyzed.productionVisits[0].productType == smp::MacroProductType::Army);
    REQUIRE(analyzed.productionVisits[0].contextTimestampTicks == 1300);
    REQUIRE(analyzed.productionVisits[1].productType == smp::MacroProductType::Worker);
    REQUIRE(analyzed.productionVisits[1].accessMethod ==
            smp::ProductionAccessMethod::LocationHotkeyClick);
    REQUIRE(analyzed.productionVisits[1].startTimestampTicks == 1000);
    REQUIRE(analyzed.productionVisits[1].contextTimestampTicks == 1700);
    REQUIRE(analyzed.productionVisits[1].endTimestampTicks == 1800);
    REQUIRE_NEAR(analyzed.productionVisits[1].startActiveMs, 1000.0, 0.001);
    REQUIRE_NEAR(analyzed.productionVisits[1].contextActiveMs, 1700.0, 0.001);
}

TEST_CASE("intervening army context splits workers despite early navigation for the later worker") {
    smp::AnalysisResult live;
    auto replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 500, 12);
    addAnchor(live, replay, 4, 800, 19);
    key(live.mechanicalEvents, 'E', 900);
    live.mechanicalEvents.push_back(
        mechanical(smp::MechanicalInputType::LocationRecall, 1000, VK_F3, 3));
    addAnchor(live, replay, 5, 1300, 31);
    key(live.mechanicalEvents, 'D', 1400);
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftDown, 1700));
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftUp, 1720));
    key(live.mechanicalEvents, 'E', 1800);
    addAnchor(live, replay, 3, 3000, 72);
    addAnchor(live, replay, 6, 4000, 96);
    addWrongPlayerReverseAnchors(replay);
    replay.productionEvents.push_back(
        {21, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});
    replay.productionEvents.push_back(
        {34, 0, smp::ReplayProductionKind::Train, "Dragoon", 0x42});
    addReplaySelection(replay, 41);
    replay.productionEvents.push_back(
        {43, 0, smp::ReplayProductionKind::Train, "Probe", 0x40});

    const auto analyzed = correlate(
        live, replay, heuristicBase(live, established({{4, 'E'}, {5, 'D'}})));
    REQUIRE(analyzed.productionVisits.size() == 3);
    REQUIRE(analyzed.productionVisits[0].productType == smp::MacroProductType::Worker);
    REQUIRE(analyzed.productionVisits[1].productType == smp::MacroProductType::Army);
    REQUIRE(analyzed.productionVisits[2].productType == smp::MacroProductType::Worker);
    REQUIRE(analyzed.productionVisits[2].startTimestampTicks == 1000);
    REQUIRE(analyzed.productionVisits[2].contextTimestampTicks == 1700);
    REQUIRE(analyzed.workerMacroCycles.cycles.size() == 2);
    REQUIRE(analyzed.armyMacroCycles.cycles.size() == 1);
    REQUIRE(analyzed.workerMacroCycles.cycles[1].startTimestampTicks == 1000);
    REQUIRE(analyzed.workerMacroCycles.cycles[1].endTimestampTicks == 1800);
    REQUIRE_NEAR(analyzed.workerMacroCycles.cycles[1].durationMs, 800.0, 0.001);
}

TEST_CASE("physical Arbiter or Attack A without replay production does not create an army click visit") {
    smp::AnalysisResult live;
    smp::ReplayData replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
    live.mechanicalEvents.push_back(mechanical(smp::MechanicalInputType::MouseLeftDown, 2050));
    key(live.mechanicalEvents, 'A', 2150);
    addAnchor(live, replay, 3, 3000, 72);
    addAnchor(live, replay, 4, 4000, 96);
    addWrongPlayerReverseAnchors(replay);
    const auto analyzed = correlate(live, replay, heuristicBase(live, {}));
    REQUIRE(analyzed.replayCorrelation.available);
    REQUIRE(analyzed.productionVisits.empty());
    REQUIRE(analyzed.armyMacroCycles.cycles.empty());
}

TEST_CASE("production context factories normalize replay tags and compare only exact known identities") {
    const auto first = smp::makeReplaySelectionProductionContext({3, 1, 2, 3});
    const auto reordered = smp::makeReplaySelectionProductionContext({2, 3, 1});
    const auto larger = smp::makeReplaySelectionProductionContext({1, 2, 3, 4});
    REQUIRE(first.kind == smp::ProductionContextKind::ReplaySelection);
    REQUIRE(first.unitTags == std::vector<std::uint32_t>({1, 2, 3}));
    REQUIRE(smp::knownProductionContext(first));
    REQUIRE(smp::sameProductionContext(first, reordered));
    REQUIRE(!smp::sameProductionContext(first, larger));
    REQUIRE(!smp::sameProductionContext(first,
                                        smp::makeControlGroupProductionContext(4)));
    REQUIRE(smp::sameProductionContext(smp::makeControlGroupProductionContext(4),
                                       smp::makeControlGroupProductionContext(4)));
    REQUIRE(!smp::sameProductionContext(smp::makeControlGroupProductionContext(4, 1),
                                        smp::makeControlGroupProductionContext(4, 2)));
    REQUIRE(smp::sameProductionContext(smp::makeLocationHotkeyProductionContext(3),
                                       smp::makeLocationHotkeyProductionContext(3)));
    REQUIRE(!smp::sameProductionContext(smp::makeLocationHotkeyProductionContext(3, 2),
                                        smp::makeLocationHotkeyProductionContext(3, 3)));
    REQUIRE(!smp::knownProductionContext({}));
}

TEST_CASE("repeated known Worker context starts a new macro cycle inside the timing window") {
    auto first = classifiedVisit(smp::MacroProductType::Worker, 1000, 1100);
    auto second = classifiedVisit(smp::MacroProductType::Worker, 2200, 2300);
    first.productionContext = smp::makeControlGroupProductionContext(4);
    second.productionContext = smp::makeControlGroupProductionContext(4);
    const auto grouped = smp::groupProductionVisits({first, second},
                                                    smp::MacroProductType::Worker,
                                                    testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 2);
    REQUIRE(grouped.repeatedContextSplits == 1);
    REQUIRE(grouped.repeatedContextSplitVisitIndices == std::vector<std::size_t>{1});
}

TEST_CASE("distinct Worker contexts of the same product type remain one macro pass") {
    std::vector<smp::ProductionVisit> visits{
        classifiedVisit(smp::MacroProductType::Worker, 1000, 1100),
        classifiedVisit(smp::MacroProductType::Worker, 1600, 1700),
        classifiedVisit(smp::MacroProductType::Worker, 2200, 2300),
    };
    visits[0].productionContext = smp::makeReplaySelectionProductionContext({100});
    visits[1].productionContext = smp::makeReplaySelectionProductionContext({200});
    visits[2].productionContext = smp::makeReplaySelectionProductionContext({300});
    const auto grouped = smp::groupProductionVisits(visits, smp::MacroProductType::Worker,
                                                    testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE(grouped.cycles[0].visitIndices.size() == 3);
    REQUIRE(grouped.repeatedContextSplits == 0);
}

TEST_CASE("Worker context A B A becomes one two-context pass followed by a new pass") {
    std::vector<smp::ProductionVisit> visits{
        classifiedVisit(smp::MacroProductType::Worker, 1000, 1100),
        classifiedVisit(smp::MacroProductType::Worker, 1600, 1700),
        classifiedVisit(smp::MacroProductType::Worker, 2200, 2300),
    };
    visits[0].productionContext = smp::makeReplaySelectionProductionContext({100});
    visits[1].productionContext = smp::makeReplaySelectionProductionContext({200});
    visits[2].productionContext = smp::makeReplaySelectionProductionContext({100});
    const auto grouped = smp::groupProductionVisits(visits, smp::MacroProductType::Worker,
                                                    testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 2);
    REQUIRE(grouped.cycles[0].visitIndices == std::vector<std::size_t>({0, 1}));
    REQUIRE(grouped.cycles[1].visitIndices == std::vector<std::size_t>{2});
    REQUIRE(grouped.repeatedContextSplitVisitIndices == std::vector<std::size_t>{2});
}

TEST_CASE("Army context A B A follows the same repeated-context split rule") {
    std::vector<smp::ProductionVisit> visits{
        classifiedVisit(smp::MacroProductType::Army, 1000, 1100),
        classifiedVisit(smp::MacroProductType::Army, 1600, 1700),
        classifiedVisit(smp::MacroProductType::Army, 2200, 2300),
    };
    visits[0].productionContext = smp::makeControlGroupProductionContext(5);
    visits[1].productionContext = smp::makeControlGroupProductionContext(6);
    visits[2].productionContext = smp::makeControlGroupProductionContext(5);
    const auto grouped = smp::groupProductionVisits(visits, smp::MacroProductType::Army,
                                                    testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 2);
    REQUIRE(grouped.cycles[0].visitIndices == std::vector<std::size_t>({0, 1}));
    REQUIRE(grouped.cycles[1].visitIndices == std::vector<std::size_t>{2});
    REQUIRE(grouped.repeatedContextSplits == 1);
}

TEST_CASE("reassigned fallback context does not split a macro cycle as the same control group") {
    std::vector<smp::ProductionVisit> visits{
        classifiedVisit(smp::MacroProductType::Army, 1000, 1100),
        classifiedVisit(smp::MacroProductType::Army, 1600, 1700),
        classifiedVisit(smp::MacroProductType::Army, 2200, 2300),
    };
    visits[0].productionContext = smp::makeControlGroupProductionContext(5, 0);
    visits[1].productionContext = smp::makeControlGroupProductionContext(6, 0);
    visits[2].productionContext = smp::makeControlGroupProductionContext(5, 1);
    const auto grouped = smp::groupProductionVisits(visits, smp::MacroProductType::Army,
                                                    testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE(grouped.cycles[0].visitIndices == std::vector<std::size_t>({0, 1, 2}));
    REQUIRE(grouped.repeatedContextSplits == 0);
}

TEST_CASE("single-context macro duration ends at the first production press") {
    auto visit = classifiedVisit(smp::MacroProductType::Worker, 1000, 1800);
    visit.firstProductionActiveMs = 1200.0;
    visit.firstProductionTimestampTicks = 1200;
    smp::refreshProductionVisitTiming(visit, testQpcFrequency);
    const auto grouped = smp::groupProductionVisits(
        {visit}, smp::MacroProductType::Worker, testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE_NEAR(grouped.cycles[0].durationMs, 200.0, 0.001);
    REQUIRE_NEAR(grouped.cycles[0].fullSpanMs, 800.0, 0.001);
    REQUIRE_NEAR(visit.productionBurstSpanMs, 600.0, 0.001);
}

TEST_CASE("multi-context macro duration ends at the final visit first production press") {
    auto first = classifiedVisit(smp::MacroProductType::Worker, 1000, 1100);
    first.productionContext = smp::makeControlGroupProductionContext(4);
    auto final = classifiedVisit(smp::MacroProductType::Worker, 1500, 2200,
                                 smp::ProductionAccessMethod::LocationHotkeyClick);
    final.contextActiveMs = 1700.0;
    final.contextTimestampTicks = 1700;
    final.firstProductionActiveMs = 1800.0;
    final.firstProductionTimestampTicks = 1800;
    final.productionContext = smp::makeLocationHotkeyProductionContext(3);
    smp::refreshProductionVisitTiming(final, testQpcFrequency);
    const auto grouped = smp::groupProductionVisits(
        {first, final}, smp::MacroProductType::Worker, testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE_NEAR(grouped.cycles[0].durationMs, 800.0, 0.001);
    REQUIRE_NEAR(grouped.cycles[0].fullSpanMs, 1200.0, 0.001);
    REQUIRE_NEAR(grouped.cycles[0].executionEndActiveMs, 1800.0, 0.001);
    REQUIRE_NEAR(grouped.cycles[0].endActiveMs, 2200.0, 0.001);
}

TEST_CASE("Worker maximum duration uses execution completion instead of final burst end") {
    const auto grouped = [](std::uint64_t finalBurstEnd) {
        auto first = classifiedVisit(smp::MacroProductType::Worker, 0, 400);
        first.firstProductionActiveMs = 200.0;
        first.firstProductionTimestampTicks = 200;
        first.productionContext = smp::makeControlGroupProductionContext(4);
        smp::refreshProductionVisitTiming(first, testQpcFrequency);

        auto final = classifiedVisit(smp::MacroProductType::Worker, 2000,
                                     finalBurstEnd,
                                     smp::ProductionAccessMethod::LocationHotkeyClick);
        final.firstProductionActiveMs = 3000.0;
        final.firstProductionTimestampTicks = 3000;
        final.productionContext = smp::makeLocationHotkeyProductionContext(3);
        smp::refreshProductionVisitTiming(final, testQpcFrequency);
        return smp::groupProductionVisits(
            {first, final}, smp::MacroProductType::Worker, testQpcFrequency);
    };

    const auto shortBurst = grouped(3200);
    const auto longBurst = grouped(9000);
    REQUIRE(shortBurst.cycles.size() == 1);
    REQUIRE(longBurst.cycles.size() == 1);
    REQUIRE_NEAR(shortBurst.cycles[0].durationMs, 3000.0, 0.001);
    REQUIRE_NEAR(longBurst.cycles[0].durationMs, 3000.0, 0.001);
    REQUIRE_NEAR(shortBurst.cycles[0].fullSpanMs, 3200.0, 0.001);
    REQUIRE_NEAR(longBurst.cycles[0].fullSpanMs, 9000.0, 0.001);
}

TEST_CASE("Worker execution duration beyond its maximum still splits the cycle") {
    auto first = classifiedVisit(smp::MacroProductType::Worker, 0, 6500);
    first.firstProductionActiveMs = 200.0;
    first.firstProductionTimestampTicks = 200;
    first.productionContext = smp::makeControlGroupProductionContext(4);
    smp::refreshProductionVisitTiming(first, testQpcFrequency);
    auto final = classifiedVisit(smp::MacroProductType::Worker, 7000, 9000);
    final.firstProductionActiveMs = 8500.0;
    final.firstProductionTimestampTicks = 8500;
    final.productionContext = smp::makeControlGroupProductionContext(5);
    smp::refreshProductionVisitTiming(final, testQpcFrequency);

    const auto grouped = smp::groupProductionVisits(
        {first, final}, smp::MacroProductType::Worker, testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 2);
}

TEST_CASE("Army execution duration beyond its maximum still splits the cycle") {
    auto first = classifiedVisit(smp::MacroProductType::Army, 0, 8500);
    first.firstProductionActiveMs = 200.0;
    first.firstProductionTimestampTicks = 200;
    first.productionContext = smp::makeControlGroupProductionContext(5);
    smp::refreshProductionVisitTiming(first, testQpcFrequency);
    auto final = classifiedVisit(smp::MacroProductType::Army, 9000, 10600);
    final.firstProductionActiveMs = 10500.0;
    final.firstProductionTimestampTicks = 10500;
    final.productionContext = smp::makeControlGroupProductionContext(6);
    smp::refreshProductionVisitTiming(final, testQpcFrequency);

    const auto grouped = smp::groupProductionVisits(
        {first, final}, smp::MacroProductType::Army, testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 2);
}

TEST_CASE("Worker macro grouping uses the 2000 ms inter-visit gap boundary") {
    REQUIRE(groupedPairWithGap(smp::MacroProductType::Worker, 1900).cycles.size() == 1);
    REQUIRE(groupedPairWithGap(smp::MacroProductType::Worker, 2000).cycles.size() == 1);
    REQUIRE(groupedPairWithGap(smp::MacroProductType::Worker, 2001).cycles.size() == 2);
    REQUIRE(groupedPairWithGap(smp::MacroProductType::Worker, 2386).cycles.size() == 2);
}

TEST_CASE("Army macro grouping uses the 2000 ms inter-visit gap boundary") {
    REQUIRE(groupedPairWithGap(smp::MacroProductType::Army, 1900).cycles.size() == 1);
    REQUIRE(groupedPairWithGap(smp::MacroProductType::Army, 2000).cycles.size() == 1);
    REQUIRE(groupedPairWithGap(smp::MacroProductType::Army, 2001).cycles.size() == 2);
    REQUIRE(groupedPairWithGap(smp::MacroProductType::Army, 2472).cycles.size() == 2);
}

TEST_CASE("production burst duration cannot extend macro-cycle merge eligibility") {
    auto first = classifiedVisit(smp::MacroProductType::Worker, 0, 1500);
    first.firstProductionActiveMs = 500.0;
    first.firstProductionTimestampTicks = 500;
    first.productionContext = smp::makeControlGroupProductionContext(5);
    smp::refreshProductionVisitTiming(first, testQpcFrequency);
    auto second = classifiedVisit(smp::MacroProductType::Worker, 2601, 2700);
    second.productionContext = smp::makeControlGroupProductionContext(6);

    const auto grouped = smp::groupProductionVisits(
        {first, second}, smp::MacroProductType::Worker, testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 2);
}

TEST_CASE("changing only production burst end does not affect macro-cycle grouping") {
    auto shortBurst = classifiedVisit(smp::MacroProductType::Army, 0, 550);
    shortBurst.firstProductionActiveMs = 500.0;
    shortBurst.firstProductionTimestampTicks = 500;
    shortBurst.productionContext = smp::makeControlGroupProductionContext(5);
    smp::refreshProductionVisitTiming(shortBurst, testQpcFrequency);
    auto longBurst = shortBurst;
    longBurst.endActiveMs = 1500.0;
    longBurst.endTimestampTicks = 1500;
    smp::refreshProductionVisitTiming(longBurst, testQpcFrequency);
    auto second = classifiedVisit(smp::MacroProductType::Army, 2601, 2700);
    second.productionContext = smp::makeControlGroupProductionContext(6);

    const auto shortBurstGrouping = smp::groupProductionVisits(
        {shortBurst, second}, smp::MacroProductType::Army, testQpcFrequency);
    const auto longBurstGrouping = smp::groupProductionVisits(
        {longBurst, second}, smp::MacroProductType::Army, testQpcFrequency);
    REQUIRE(shortBurstGrouping.cycles.size() == 2);
    REQUIRE(longBurstGrouping.cycles.size() == 2);
    REQUIRE(shortBurstGrouping.cycles[0].visitIndices ==
            longBurstGrouping.cycles[0].visitIndices);
}

TEST_CASE("control-group-only production has no camera access episode") {
    std::vector<smp::ProductionVisit> visits;
    for (int group = 5; group <= 7; ++group) {
        auto production = classifiedVisit(smp::MacroProductType::Army,
                                          static_cast<std::uint64_t>(group * 100),
                                          static_cast<std::uint64_t>(group * 100 + 50));
        production.controlGroup = group;
        production.productionContext = smp::makeControlGroupProductionContext(group);
        visits.push_back(std::move(production));
    }
    smp::AnalysisResult live;
    smp::annotateProductionAccessTelemetry(visits, live);

    for (const auto& production : visits) {
        REQUIRE(production.selectionAccess ==
                smp::ProductionSelectionAccess::ControlGroup);
        REQUIRE(production.cameraAccess == smp::ProductionCameraAccess::None);
        REQUIRE(production.cameraEpisodeId == 0);
    }
}

TEST_CASE("location-hotkey camera episode is inherited by subsequent direct clicks") {
    smp::AnalysisResult live;
    live.navigationEvents.push_back(
        {100, 100.0, smp::CameraNavigationType::LocationHotkey, 2});
    live.mechanicalEvents = {
        mechanical(smp::MechanicalInputType::LocationRecall, 100, VK_F2, 2),
        mechanical(smp::MechanicalInputType::MouseRightDown, 250),
        mechanical(smp::MechanicalInputType::KeyPress, 450, 'A'),
    };
    std::vector<smp::ProductionVisit> visits;
    for (const auto context : {200ULL, 400ULL, 600ULL}) {
        auto production = classifiedVisit(smp::MacroProductType::Army, context, context + 50,
                                          smp::ProductionAccessMethod::ScreenClick);
        production.productionContext =
            smp::makeReplaySelectionProductionContext(
                {static_cast<std::uint32_t>(context)});
        visits.push_back(std::move(production));
    }
    smp::annotateProductionAccessTelemetry(visits, live);

    const auto episode = visits.front().cameraEpisodeId;
    REQUIRE(episode != 0);
    for (const auto& production : visits) {
        REQUIRE(production.selectionAccess ==
                smp::ProductionSelectionAccess::DirectClick);
        REQUIRE(production.cameraAccess ==
                smp::ProductionCameraAccess::LocationHotkey);
        REQUIRE(production.cameraEpisodeId == episode);
        REQUIRE(production.cameraAnchorKind ==
                smp::ProductionCameraAnchorKind::LocationHotkey);
        REQUIRE(production.cameraAnchorId == 2);
        REQUIRE(production.cameraAnchorTimestampTicks == 100);
    }
}

TEST_CASE("control-group double-tap episode covers its group and inherited direct clicks") {
    smp::AnalysisResult live;
    live.navigationEvents.push_back(
        {150, 150.0, smp::CameraNavigationType::ControlGroupJump, 5});
    auto groupVisit = classifiedVisit(smp::MacroProductType::Army, 150, 200);
    groupVisit.controlGroup = 5;
    groupVisit.productionContext = smp::makeControlGroupProductionContext(5);
    auto firstClick = classifiedVisit(smp::MacroProductType::Army, 300, 350,
                                      smp::ProductionAccessMethod::ScreenClick);
    auto secondClick = classifiedVisit(smp::MacroProductType::Army, 500, 550,
                                       smp::ProductionAccessMethod::ScreenClick);
    std::vector<smp::ProductionVisit> visits{groupVisit, firstClick, secondClick};
    smp::annotateProductionAccessTelemetry(visits, live);

    const auto episode = visits.front().cameraEpisodeId;
    REQUIRE(episode != 0);
    for (const auto& production : visits) {
        REQUIRE(production.cameraAccess ==
                smp::ProductionCameraAccess::ControlGroupDoubleTap);
        REQUIRE(production.cameraEpisodeId == episode);
        REQUIRE(production.cameraAnchorKind ==
                smp::ProductionCameraAnchorKind::ControlGroup);
        REQUIRE(production.cameraAnchorId == 5);
        REQUIRE(production.cameraAnchorTimestampTicks == 150);
    }
    REQUIRE(visits[0].selectionAccess ==
            smp::ProductionSelectionAccess::ControlGroup);
    REQUIRE(visits[1].selectionAccess ==
            smp::ProductionSelectionAccess::DirectClick);
}

TEST_CASE("ordinary control-group selection is not a camera double tap") {
    smp::AnalysisResult live;
    select(live.mechanicalEvents, 5, 100);
    auto production = classifiedVisit(smp::MacroProductType::Army, 100, 150);
    production.controlGroup = 5;
    std::vector<smp::ProductionVisit> visits{production};
    smp::annotateProductionAccessTelemetry(visits, live);

    REQUIRE(visits[0].selectionAccess ==
            smp::ProductionSelectionAccess::ControlGroup);
    REQUIRE(visits[0].cameraAccess == smp::ProductionCameraAccess::None);
    REQUIRE(visits[0].cameraEpisodeId == 0);
}

TEST_CASE("later camera navigation replaces the inherited camera episode") {
    smp::AnalysisResult live;
    live.navigationEvents = {
        {100, 100.0, smp::CameraNavigationType::LocationHotkey, 2},
        {400, 400.0, smp::CameraNavigationType::LocationHotkey, 3},
    };
    auto first = classifiedVisit(smp::MacroProductType::Army, 200, 250,
                                 smp::ProductionAccessMethod::ScreenClick);
    auto second = classifiedVisit(smp::MacroProductType::Army, 500, 550,
                                  smp::ProductionAccessMethod::ScreenClick);
    std::vector<smp::ProductionVisit> visits{first, second};
    smp::annotateProductionAccessTelemetry(visits, live);

    REQUIRE(visits[0].cameraAnchorId == 2);
    REQUIRE(visits[1].cameraAnchorId == 3);
    REQUIRE(visits[0].cameraEpisodeId != visits[1].cameraEpisodeId);
}

TEST_CASE("access telemetry annotation does not change macro-cycle grouping") {
    auto first = classifiedVisit(smp::MacroProductType::Army, 100, 200,
                                 smp::ProductionAccessMethod::ScreenClick);
    first.productionContext = smp::makeReplaySelectionProductionContext({100});
    auto second = classifiedVisit(smp::MacroProductType::Army, 800, 900,
                                  smp::ProductionAccessMethod::ScreenClick);
    second.productionContext = smp::makeReplaySelectionProductionContext({200});
    std::vector<smp::ProductionVisit> visits{first, second};
    const auto before = smp::groupProductionVisits(
        visits, smp::MacroProductType::Army, testQpcFrequency);
    smp::AnalysisResult live;
    live.navigationEvents.push_back(
        {50, 50.0, smp::CameraNavigationType::LocationHotkey, 2});
    smp::annotateProductionAccessTelemetry(visits, live);
    const auto after = smp::groupProductionVisits(
        visits, smp::MacroProductType::Army, testQpcFrequency);

    REQUIRE(before.cycles.size() == after.cycles.size());
    REQUIRE(before.cycles[0].visitIndices == after.cycles[0].visitIndices);
    REQUIRE(before.cycles[0].durationMs == after.cycles[0].durationMs);
}

TEST_CASE("all control-group-selected visits derive control-group-only macro style") {
    std::vector<smp::ProductionVisit> visits;
    for (int group = 5; group <= 7; ++group) {
        auto production = classifiedVisit(
            smp::MacroProductType::Army,
            static_cast<std::uint64_t>((group - 5) * 300),
            static_cast<std::uint64_t>((group - 5) * 300 + 100));
        production.controlGroup = group;
        production.productionContext = smp::makeControlGroupProductionContext(group);
        visits.push_back(std::move(production));
    }
    const auto grouped = smp::groupProductionVisits(
        visits, smp::MacroProductType::Army, testQpcFrequency);

    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE(grouped.cycles[0].macroAccessStyle ==
            smp::MacroAccessStyle::ControlGroupOnly);
    REQUIRE(grouped.cycles[0].controlGroupVisitCount == 3);
    REQUIRE(grouped.cycles[0].directClickVisitCount == 0);
    REQUIRE(grouped.accessStyleStatistics[
                smp::macroAccessStyleIndex(smp::MacroAccessStyle::ControlGroupOnly)]
                .cycleCount == 1);
}

TEST_CASE("one location-hotkey episode with multiple clicks derives location-click style") {
    std::vector<smp::ProductionVisit> visits;
    for (std::uint64_t index = 0; index < 3; ++index) {
        auto production = classifiedVisit(
            smp::MacroProductType::Army, 200 + index * 300, 300 + index * 300,
            smp::ProductionAccessMethod::ScreenClick);
        production.cameraAccess = smp::ProductionCameraAccess::LocationHotkey;
        production.cameraAnchorKind =
            smp::ProductionCameraAnchorKind::LocationHotkey;
        production.cameraAnchorId = 2;
        production.cameraAnchorTimestampTicks = 100;
        production.cameraEpisodeId = 1;
        production.productionContext = smp::makeReplaySelectionProductionContext(
            {static_cast<std::uint32_t>(100 + index)});
        visits.push_back(std::move(production));
    }
    visits[0].producedUnits = {"Dragoon"};
    visits[1].producedUnits = {"Observer"};
    visits[2].producedUnits = {"Corsair"};
    const auto grouped = smp::groupProductionVisits(
        visits, smp::MacroProductType::Army, testQpcFrequency);

    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE(grouped.cycles[0].macroAccessStyle ==
            smp::MacroAccessStyle::LocationHotkeyClick);
    REQUIRE(grouped.cycles[0].directClickVisitCount == 3);
    REQUIRE(grouped.cycles[0].cameraEpisodeCount == 1);
}

TEST_CASE("control-group camera center with inherited clicks derives center-click style") {
    auto group = classifiedVisit(smp::MacroProductType::Army, 100, 200);
    group.controlGroup = 5;
    group.cameraAccess = smp::ProductionCameraAccess::ControlGroupDoubleTap;
    group.cameraAnchorKind = smp::ProductionCameraAnchorKind::ControlGroup;
    group.cameraAnchorId = 5;
    group.cameraEpisodeId = 7;
    group.cameraAnchorTimestampTicks = 100;
    group.productionContext = smp::makeControlGroupProductionContext(5);
    auto gateway = classifiedVisit(smp::MacroProductType::Army, 400, 500,
                                   smp::ProductionAccessMethod::ScreenClick);
    gateway.cameraAccess = smp::ProductionCameraAccess::ControlGroupDoubleTap;
    gateway.cameraAnchorKind = smp::ProductionCameraAnchorKind::ControlGroup;
    gateway.cameraAnchorId = 5;
    gateway.cameraEpisodeId = 7;
    gateway.cameraAnchorTimestampTicks = 100;
    gateway.productionContext = smp::makeReplaySelectionProductionContext({200});
    auto robo = gateway;
    robo.startActiveMs = 700.0;
    robo.contextActiveMs = 700.0;
    robo.firstProductionActiveMs = 800.0;
    robo.endActiveMs = 800.0;
    robo.startTimestampTicks = 700;
    robo.contextTimestampTicks = 700;
    robo.firstProductionTimestampTicks = 800;
    robo.endTimestampTicks = 800;
    robo.productionContext = smp::makeReplaySelectionProductionContext({300});
    smp::refreshProductionVisitTiming(robo, testQpcFrequency);
    const auto grouped = smp::groupProductionVisits(
        {group, gateway, robo}, smp::MacroProductType::Army, testQpcFrequency);

    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE(grouped.cycles[0].macroAccessStyle ==
            smp::MacroAccessStyle::ControlGroupCenterClick);
    REQUIRE(grouped.cycles[0].controlGroupVisitCount == 1);
    REQUIRE(grouped.cycles[0].directClickVisitCount == 2);
    REQUIRE(grouped.cycles[0].cameraEpisodeCount == 1);
}

TEST_CASE("materially different access techniques in one cycle derive mixed style") {
    auto group = classifiedVisit(smp::MacroProductType::Army, 100, 200);
    group.controlGroup = 5;
    group.productionContext = smp::makeControlGroupProductionContext(5);
    auto click = classifiedVisit(smp::MacroProductType::Army, 400, 500,
                                 smp::ProductionAccessMethod::ScreenClick);
    click.cameraAccess = smp::ProductionCameraAccess::LocationHotkey;
    click.cameraAnchorKind = smp::ProductionCameraAnchorKind::LocationHotkey;
    click.cameraAnchorId = 2;
    click.cameraEpisodeId = 1;
    click.cameraAnchorTimestampTicks = 300;
    click.productionContext = smp::makeReplaySelectionProductionContext({200});
    const auto grouped = smp::groupProductionVisits(
        {group, click}, smp::MacroProductType::Army, testQpcFrequency);

    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE(grouped.cycles[0].macroAccessStyle == smp::MacroAccessStyle::Mixed);
}

TEST_CASE("unsupported camera access derives other rather than a principal style") {
    auto visit = classifiedVisit(smp::MacroProductType::Army, 100, 200,
                                 smp::ProductionAccessMethod::ScreenClick);
    visit.cameraAccess = smp::ProductionCameraAccess::Minimap;
    visit.cameraAnchorKind = smp::ProductionCameraAnchorKind::Minimap;
    visit.cameraEpisodeId = 1;
    const auto grouped = smp::groupProductionVisits(
        {visit}, smp::MacroProductType::Army, testQpcFrequency);
    REQUIRE(grouped.cycles[0].macroAccessStyle == smp::MacroAccessStyle::Other);
}

TEST_CASE("macro access style speed statistics use deterministic interpolated percentiles") {
    const auto statistics =
        smp::summarizeMacroAccessStyleDurations({100.0, 200.0, 300.0, 400.0});
    REQUIRE(statistics.cycleCount == 4);
    REQUIRE_NEAR(*statistics.averageDurationMs, 250.0, 0.001);
    REQUIRE_NEAR(*statistics.medianDurationMs, 250.0, 0.001);
    REQUIRE_NEAR(*statistics.bestDurationMs, 100.0, 0.001);
    REQUIRE_NEAR(*statistics.p25DurationMs, 175.0, 0.001);
    REQUIRE_NEAR(*statistics.p75DurationMs, 325.0, 0.001);
    REQUIRE_NEAR(*statistics.p90DurationMs, 370.0, 0.001);
}

TEST_CASE("control-group assignment strictly between visits breaks a same-product cycle") {
    auto first = classifiedVisit(smp::MacroProductType::Army, 900, 1000);
    auto second = classifiedVisit(smp::MacroProductType::Army, 1700, 1900);
    first.productionContext = smp::makeControlGroupProductionContext(5);
    second.productionContext = smp::makeControlGroupProductionContext(6);
    const std::vector<smp::MechanicalInputEvent> events{
        mechanical(smp::MechanicalInputType::ControlGroupAssign, 1300, '3', 3,
                   smp::ModifierCtrl)};
    const auto grouped = smp::groupProductionVisits(
        {first, second}, smp::MacroProductType::Army, events, testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 2);
    REQUIRE(grouped.assignmentInterruptionSplits == 1);
    REQUIRE(grouped.assignmentInterruptionSplitDetails[0].previousVisitIndex == 0);
    REQUIRE(grouped.assignmentInterruptionSplitDetails[0].nextVisitIndex == 1);
    REQUIRE(grouped.assignmentInterruptionSplitDetails[0].interruptionType ==
            smp::MechanicalInputType::ControlGroupAssign);
}

TEST_CASE("control-group Add strictly between visits interrupts macro grouping") {
    auto first = classifiedVisit(smp::MacroProductType::Army, 900, 1000);
    auto second = classifiedVisit(smp::MacroProductType::Army, 1700, 1900);
    first.productionContext = smp::makeControlGroupProductionContext(5);
    second.productionContext = smp::makeControlGroupProductionContext(6);
    const std::vector<smp::MechanicalInputEvent> events{
        mechanical(smp::MechanicalInputType::ControlGroupAdd, 1300, '5', 5,
                   smp::ModifierShift)};
    const auto grouped = smp::groupProductionVisits(
        {first, second}, smp::MacroProductType::Army, events, testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 2);
    REQUIRE(grouped.assignmentInterruptionSplits == 1);
    REQUIRE(grouped.assignmentInterruptionSplitDetails[0].interruptionType ==
            smp::MechanicalInputType::ControlGroupAdd);
}

TEST_CASE("location assignment strictly between visits breaks a same-product cycle") {
    auto first = classifiedVisit(smp::MacroProductType::Worker, 900, 1000);
    auto second = classifiedVisit(smp::MacroProductType::Worker, 1700, 1900);
    first.productionContext = smp::makeLocationHotkeyProductionContext(3);
    second.productionContext = smp::makeLocationHotkeyProductionContext(4);
    const std::vector<smp::MechanicalInputEvent> events{
        mechanical(smp::MechanicalInputType::LocationAssign, 1300, VK_F3, 3,
                   smp::ModifierShift)};
    const auto grouped = smp::groupProductionVisits(
        {first, second}, smp::MacroProductType::Worker, events, testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 2);
    REQUIRE(grouped.assignmentInterruptionSplits == 1);
    REQUIRE(grouped.assignmentInterruptionSplitDetails[0].interruptionType ==
            smp::MechanicalInputType::LocationAssign);
}

TEST_CASE("assignment interruption boundaries are strict") {
    auto first = classifiedVisit(smp::MacroProductType::Army, 900, 1000);
    auto second = classifiedVisit(smp::MacroProductType::Army, 1700, 1900);
    first.productionContext = smp::makeControlGroupProductionContext(5);
    second.productionContext = smp::makeControlGroupProductionContext(6);
    const std::vector<smp::MechanicalInputEvent> events{
        mechanical(smp::MechanicalInputType::ControlGroupAssign, 1000, '3', 3,
                   smp::ModifierCtrl),
        mechanical(smp::MechanicalInputType::LocationAssign, 1700, VK_F3, 3,
                   smp::ModifierShift),
    };
    const auto grouped = smp::groupProductionVisits(
        {first, second}, smp::MacroProductType::Army, events, testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE(grouped.assignmentInterruptionSplits == 0);
}

TEST_CASE("control-group selects and right clicks do not interrupt macro grouping") {
    auto first = classifiedVisit(smp::MacroProductType::Army, 900, 1000);
    auto second = classifiedVisit(smp::MacroProductType::Army, 1700, 1900);
    first.productionContext = smp::makeControlGroupProductionContext(5);
    second.productionContext = smp::makeControlGroupProductionContext(6);
    const std::vector<smp::MechanicalInputEvent> events{
        mechanical(smp::MechanicalInputType::MouseRightDown, 1200),
        mechanical(smp::MechanicalInputType::MouseRightUp, 1220),
        mechanical(smp::MechanicalInputType::ControlGroupSelect, 1400, '7', 7),
    };
    const auto grouped = smp::groupProductionVisits(
        {first, second}, smp::MacroProductType::Army, events, testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE(grouped.assignmentInterruptionSplits == 0);
}

TEST_CASE("strong replay identity overrides fallback assignment generations") {
    std::vector<smp::ProductionVisit> sameReplay{
        classifiedVisit(smp::MacroProductType::Army, 1000, 1100),
        classifiedVisit(smp::MacroProductType::Army, 1600, 1700),
    };
    sameReplay[0].controlGroup = 5;
    sameReplay[1].controlGroup = 5;
    sameReplay[0].productionContext = smp::makeReplaySelectionProductionContext({1234});
    sameReplay[1].productionContext = smp::makeReplaySelectionProductionContext({1234});
    const auto repeated = smp::groupProductionVisits(sameReplay, smp::MacroProductType::Army,
                                                     testQpcFrequency);
    REQUIRE(repeated.cycles.size() == 2);
    REQUIRE(repeated.repeatedContextSplits == 1);

    sameReplay[1].productionContext = smp::makeReplaySelectionProductionContext({5678});
    const auto distinct = smp::groupProductionVisits(sameReplay, smp::MacroProductType::Army,
                                                     testQpcFrequency);
    REQUIRE(distinct.cycles.size() == 1);
    REQUIRE(distinct.repeatedContextSplits == 0);
}

TEST_CASE("unknown contexts retain timing-only grouping") {
    const std::vector<smp::ProductionVisit> visits{
        classifiedVisit(smp::MacroProductType::Worker, 1000, 1100,
                        smp::ProductionAccessMethod::ScreenClick),
        classifiedVisit(smp::MacroProductType::Worker, 1600, 1700,
                        smp::ProductionAccessMethod::ScreenClick),
    };
    const auto grouped = smp::groupProductionVisits(visits, smp::MacroProductType::Worker,
                                                    testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE(grouped.repeatedContextSplits == 0);
}

TEST_CASE("replay context identity overrides differing access methods") {
    auto controlGroup = classifiedVisit(smp::MacroProductType::Worker, 1000, 1100,
                                        smp::ProductionAccessMethod::ControlGroup);
    auto locationClick = classifiedVisit(smp::MacroProductType::Worker, 1600, 1700,
                                         smp::ProductionAccessMethod::LocationHotkeyClick);
    controlGroup.productionContext = smp::makeReplaySelectionProductionContext({1234});
    locationClick.productionContext = smp::makeReplaySelectionProductionContext({1234});
    const auto grouped = smp::groupProductionVisits({controlGroup, locationClick},
                                                    smp::MacroProductType::Worker,
                                                    testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 2);
    REQUIRE(grouped.repeatedContextSplits == 1);
}

TEST_CASE("worker grouping joins control group location and minimap visits into one pass") {
    std::vector<smp::ProductionVisit> visits{
        classifiedVisit(smp::MacroProductType::Worker, 1000, 1150,
                        smp::ProductionAccessMethod::ControlGroup),
        classifiedVisit(smp::MacroProductType::Worker, 1800, 2150,
                        smp::ProductionAccessMethod::LocationHotkeyClick),
        classifiedVisit(smp::MacroProductType::Worker, 2600, 2950,
                        smp::ProductionAccessMethod::LocationHotkeyClick),
        classifiedVisit(smp::MacroProductType::Worker, 3400, 3750,
                        smp::ProductionAccessMethod::MinimapClick),
        classifiedVisit(smp::MacroProductType::Worker, 4200, 4550,
                        smp::ProductionAccessMethod::MinimapClick),
    };
    const auto grouped = smp::groupProductionVisits(visits, smp::MacroProductType::Worker,
                                                    testQpcFrequency);
    const std::vector<smp::ProductionVisit> threeBaseVisits(visits.begin(), visits.begin() + 3);
    const auto threeBase = smp::groupProductionVisits(
        threeBaseVisits, smp::MacroProductType::Worker, testQpcFrequency);
    REQUIRE(threeBase.cycles.size() == 1);
    REQUIRE(threeBase.cycles[0].visitIndices.size() == 3);
    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE(grouped.cycles[0].visitIndices.size() == 5);
    REQUIRE(grouped.productionVisitCount == 5);
    REQUIRE_NEAR(grouped.cycles[0].startActiveMs, 1000.0, 0.001);
    REQUIRE_NEAR(grouped.cycles[0].endActiveMs, 4550.0, 0.001);
}

TEST_CASE("army grouping joins four production contexts without hard-breaking on access changes") {
    std::vector<smp::ProductionVisit> visits{
        classifiedVisit(smp::MacroProductType::Army, 1000, 1300),
        classifiedVisit(smp::MacroProductType::Army, 1800, 2050),
        classifiedVisit(smp::MacroProductType::Army, 2500, 2600,
                        smp::ProductionAccessMethod::LocationHotkeyClick),
        classifiedVisit(smp::MacroProductType::Army, 3200, 3300,
                        smp::ProductionAccessMethod::ScreenClick),
    };
    const auto grouped = smp::groupProductionVisits(visits, smp::MacroProductType::Army,
                                                    testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 1);
    REQUIRE(grouped.cycles[0].visitIndices.size() == 4);
    REQUIRE(grouped.productionVisitCount == 4);
}

TEST_CASE("opposite product types split cycles and never form a mixed cycle") {
    std::vector<smp::ProductionVisit> visits{
        classifiedVisit(smp::MacroProductType::Worker, 1000, 1100),
        classifiedVisit(smp::MacroProductType::Army, 1500, 1700),
        classifiedVisit(smp::MacroProductType::Army, 1900, 2100),
        classifiedVisit(smp::MacroProductType::Worker, 2300, 2400),
    };
    const auto workers = smp::groupProductionVisits(visits, smp::MacroProductType::Worker,
                                                    testQpcFrequency);
    const auto army = smp::groupProductionVisits(visits, smp::MacroProductType::Army,
                                                 testQpcFrequency);
    REQUIRE(workers.cycles.size() == 2);
    REQUIRE(army.cycles.size() == 1);
    REQUIRE(army.cycles[0].visitIndices.size() == 2);
}

TEST_CASE("real-time discontinuity breaks an otherwise close active-time macro pass") {
    auto first = classifiedVisit(smp::MacroProductType::Worker, 1000, 1100);
    auto second = classifiedVisit(smp::MacroProductType::Worker, 5000, 5100);
    second.startActiveMs = 1200.0;
    second.endActiveMs = 1300.0;
    const auto grouped = smp::groupProductionVisits({first, second}, smp::MacroProductType::Worker,
                                                    testQpcFrequency);
    REQUIRE(grouped.cycles.size() == 2);
}

TEST_CASE("replay failure preserves heuristic visits and marks worker and army unavailable") {
    smp::AnalysisResult live;
    visit(live.mechanicalEvents, 5, 'D', 3, 1000);
    auto analysis = smp::analyzeProductionVisits(live, profile(), testQpcFrequency);
    REQUIRE(analysis.visitsAvailable);
    // Establish the group explicitly to model already-learned heuristic evidence.
    analysis.productionVisits = smp::detectHeuristicProductionVisitsForLikelyGroups(
        live, profile(), testQpcFrequency, established({{5, 'D'}}));
    smp::ReplayData unrelated;
    unrelated.players = {{0, "different-game"}};
    unrelated.totalFrames = 100;
    analysis = smp::correlateProductionVisitsWithReplay(live, profile(), testQpcFrequency,
                                                        std::move(analysis), unrelated, "fixture");
    REQUIRE(!analysis.replayCorrelation.available);
    REQUIRE(analysis.productionVisits.size() == 1);
    REQUIRE(!analysis.workerMacroCycles.available);
    REQUIRE(!analysis.armyMacroCycles.available);
}

TEST_CASE("derived JSON stores visits separate worker and army cycles and compact diagnostics") {
    smp::AnalysisResult live;
    live.activeDurationSeconds = 60.0;
    live.navigationEvents.push_back(
        {1900, 1900.0, smp::CameraNavigationType::LocationHotkey, 2});
    smp::ProductionAnalysis production;
    production.visitsAvailable = true;
    production.productionVisits = {
        classifiedVisit(smp::MacroProductType::Worker, 1000, 1100),
        classifiedVisit(smp::MacroProductType::Army, 2000, 2200,
                        smp::ProductionAccessMethod::LocationHotkeyClick),
    };
    production.productionVisits[0].producedUnits = {"Probe"};
    production.productionVisits[0].productionContext =
        smp::makeControlGroupProductionContext(4, 2);
    production.productionVisits[1].producedUnits = {"Dragoon", "Dragoon"};
    production.productionVisits[1].productionContext =
        smp::makeReplaySelectionProductionContext({1234, 5678});
    production.productionVisits[1].physicalProductionPresses = 3;
    production.productionVisits[1].contextActiveMs = 2100.0;
    production.productionVisits[1].contextTimestampTicks = 2100;
    smp::refreshProductionVisitTiming(production.productionVisits[1], testQpcFrequency);
    production.productionVisits[1].physicalProductionKeys = {'D', 'D', 'D'};
    production.productionVisits[1].replayProductionCommands = 2;
    smp::annotateProductionAccessTelemetry(production.productionVisits, live);
    production.workerMacroCycles = smp::groupProductionVisits(
        production.productionVisits, smp::MacroProductType::Worker, testQpcFrequency);
    production.armyMacroCycles = smp::groupProductionVisits(
        production.productionVisits, smp::MacroProductType::Army, testQpcFrequency);
    production.workerMacroCycles.repeatedContextSplits = 2;
    production.armyMacroCycles.repeatedContextSplits = 1;
    production.workerMacroCycles.assignmentInterruptionSplits = 3;
    production.armyMacroCycles.assignmentInterruptionSplits = 4;
    production.replayCorrelation.available = true;
    production.replayCorrelation.playerId = 0;
    production.replayCorrelation.playerName = "fixture";
    production.replayCorrelation.sequenceScore = 0.95;
    production.replayCorrelation.matchedControlGroupEvents = 8;
    production.replayCorrelation.matchedProductionVisits = 2;
    production.replayCorrelation.replayCreatedControlGroupVisits = 1;
    production.replayCorrelation.matchedClickVisits = 1;
    production.replayCorrelation.matchedReplayProductionEvents = 3;
    production.replayCorrelation.unmatchedReplayProductionEvents = 1;
    production.replayCorrelation.extendedProductionVisits = 1;
    production.replayCorrelation.extendedPhysicalProductionPresses = 4;
    const auto encoded = smp::analysisToJson(live, "fixture", production, profile());
    REQUIRE(encoded["schema_version"].asInt() == 4);
    REQUIRE(encoded["analysis_version"].asString() ==
            "camera-nav-production-macro-3-army-control-group-management-5-army-command-1-ability-activity-1");
    REQUIRE(encoded["macro_cycles"].isNull());
    REQUIRE(encoded["production_visits"]["count"].asInt() == 2);
    const auto& encodedVisits = encoded["production_visits"]["visits"].asArray();
    REQUIRE(encodedVisits[1]["access_method"].asString() ==
            "location_hotkey_click");
    REQUIRE(encodedVisits[1]["selection_access"].asString() == "direct_click");
    REQUIRE(encodedVisits[1]["camera_access"].asString() == "location_hotkey");
    REQUIRE(encodedVisits[1]["camera_episode_id"].asInt() == 1);
    REQUIRE(encodedVisits[1]["camera_anchor_kind"].asString() ==
            "location_hotkey");
    REQUIRE(encodedVisits[1]["camera_anchor_id"].asInt() == 2);
    REQUIRE(encodedVisits[1]["camera_anchor_qpc"].asNumber() == 1900.0);
    REQUIRE(encodedVisits[1]["physical_production_presses"].asInt() == 3);
    REQUIRE(encodedVisits[1]["physical_production_keys"].asArray().size() == 3);
    REQUIRE_NEAR(encodedVisits[1]["context_active_ms"].asNumber(), 2100.0, 0.001);
    REQUIRE_NEAR(encodedVisits[1]["first_production_active_ms"].asNumber(), 2200.0,
                 0.001);
    REQUIRE_NEAR(encodedVisits[1]["access_latency_ms"].asNumber(), 100.0, 0.001);
    REQUIRE_NEAR(encodedVisits[1]["production_latency_ms"].asNumber(), 100.0, 0.001);
    REQUIRE_NEAR(encodedVisits[1]["execution_duration_ms"].asNumber(), 200.0, 0.001);
    REQUIRE_NEAR(encodedVisits[1]["production_burst_span_ms"].asNumber(), 0.0, 0.001);
    REQUIRE(encodedVisits[0]["production_context"]["kind"].asString() == "control_group");
    REQUIRE(encodedVisits[0]["production_context"]["control_group"].asInt() == 4);
    REQUIRE(encodedVisits[0]["production_context"]["generation"].asInt() == 2);
    REQUIRE(encodedVisits[1]["production_context"]["kind"].asString() ==
            "replay_selection");
    REQUIRE(encodedVisits[1]["production_context"]["unit_tags"].asArray().size() == 2);
    production.productionVisits[1].productionContext =
        smp::makeLocationHotkeyProductionContext(3, 4);
    const auto locationEncoded =
        smp::analysisToJson(live, "fixture", production, profile());
    const auto& locationContext =
        locationEncoded["production_visits"]["visits"].asArray()[1]["production_context"];
    REQUIRE(locationContext["kind"].asString() == "location_hotkey");
    REQUIRE(locationContext["location_hotkey"].asInt() == 3);
    REQUIRE(locationContext["generation"].asInt() == 4);
    const auto& workerCycles = encoded["worker_macro_cycles"]["cycles"].asArray();
    const auto& armyCycles = encoded["army_macro_cycles"]["cycles"].asArray();
    REQUIRE(workerCycles[0]["macro_access_style"].asString() ==
            "control_group_only");
    REQUIRE(armyCycles[0]["macro_access_style"].asString() ==
            "location_hotkey_click");
    REQUIRE(armyCycles[0]["direct_click_visit_count"].asInt() == 1);
    REQUIRE(armyCycles[0]["camera_episode_count"].asInt() == 1);
    const auto& armyStyle =
        encoded["army_macro_cycles"]["macro_access_styles"]
               ["location_hotkey_click"];
    REQUIRE(armyStyle["cycle_count"].asInt() == 1);
    REQUIRE_NEAR(armyStyle["percentage"].asNumber(), 100.0, 0.001);
    REQUIRE_NEAR(armyStyle["median_duration_ms"].asNumber(), 200.0, 0.001);
    REQUIRE(workerCycles[0]["visit_indices"].asArray()[0].asInt() == 0);
    REQUIRE_NEAR(workerCycles[0]["execution_end_active_ms"].asNumber(), 1100.0,
                 0.001);
    REQUIRE_NEAR(workerCycles[0]["duration_ms"].asNumber(), 100.0, 0.001);
    REQUIRE_NEAR(workerCycles[0]["full_span_ms"].asNumber(), 100.0, 0.001);
    REQUIRE(armyCycles[0]["visit_indices"].asArray()[0].asInt() == 1);
    REQUIRE(encoded["replay_correlation"]["matched_control_group_events"].asInt() == 8);
    REQUIRE(encoded["replay_correlation"]["replay_created_control_group_visits"].asInt() == 1);
    REQUIRE(encoded["replay_correlation"]["matched_click_visits"].asInt() == 1);
    REQUIRE(encoded["replay_correlation"]["unmatched_replay_production_events"].asInt() == 1);
    REQUIRE(encoded["replay_correlation"]["extended_production_visits"].asInt() == 1);
    REQUIRE(encoded["replay_correlation"]["extended_physical_production_presses"].asInt() == 4);
    REQUIRE(encoded["macro_cycle_diagnostics"]["worker_repeated_context_splits"].asInt() == 2);
    REQUIRE(encoded["macro_cycle_diagnostics"]["army_repeated_context_splits"].asInt() == 1);
    REQUIRE(encoded["macro_cycle_diagnostics"]
                   ["worker_assignment_interruption_splits"].asInt() == 3);
    REQUIRE(encoded["macro_cycle_diagnostics"]
                   ["army_assignment_interruption_splits"].asInt() == 4);
    REQUIRE(encoded["mechanical_events"].isNull());
    REQUIRE(encoded["replay_commands"].isNull());
}
