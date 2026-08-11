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
  "Hotkeys": "STR_MAKE_P_PROBE=e\nSTR_MAKE_P_DRAGOON=d\nSTR_MAKE_P_DTEMPLAR=q\nSTR_MAKE_P_OBSERVER=q\nSTR_MAKE_P_CORSAIR=e\nSTR_MAKE_P_SHUTTLE=s\nSTR_MAKE_P_REAVER=v\nSTR_MAKE_P_ARBITER=a\nSTR_MAKE_P_INTERCEPTOR=i\nSTR_MAKE_P_SCARAB=r\nSTR_MAKE_T_SCV=s\nSTR_MAKE_T_MARINE=m\nSTR_MAKE_Z_DRONE=w\nSTR_MAKE_Z_HYDRALISK=h\nSTR_ATTACK=a\nSTR_STOP=s"
})json";
    return fixture;
}

smp::MacroHotkeyProfile profile() {
    return smp::parseStarCraftHotkeyProfile(realisticSettingsJson());
}

smp::MechanicalInputEvent mechanical(smp::MechanicalInputType type, std::uint64_t ticks,
                                     std::uint16_t key = 0, int value = -1,
                                     std::uint16_t modifiers = smp::ModifierNone) {
    smp::MechanicalInputEvent event;
    event.timestampTicks = ticks;
    event.activeMs = static_cast<double>(ticks);
    event.type = type;
    event.virtualKey = key;
    event.modifiers = modifiers;
    event.value = value;
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
    visit.startTimestampTicks = start;
    visit.endTimestampTicks = end;
    visit.startActiveMs = static_cast<double>(start);
    visit.endActiveMs = static_cast<double>(end);
    visit.durationMs = static_cast<double>(end - start);
    visit.replayConfirmed = true;
    visit.physicalProductionPresses = 1;
    visit.replayProductionCommands = 1;
    return visit;
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
    REQUIRE(visits[0].physicalProductionPresses == 4);
    REQUIRE_NEAR(visits[0].startActiveMs, 1000.0, 0.001);
    REQUIRE_NEAR(visits[0].endActiveMs, 1250.0, 0.001);
    REQUIRE(visits[0].productType == smp::MacroProductType::Unknown);
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

TEST_CASE("realistic screp JSON extracts only select and ordinary production semantics") {
    const std::string fixture = R"json({
      "Header":{"Frames":400,"Players":[{"ID":0,"Name":"P"},{"ID":1,"Name":"Z"}]},
      "Commands":{"Cmds":[
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Hotkey"},"HotkeyType":{"Name":"Select"},"Group":4},
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Select"},"UnitTags":[1001]},
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Select Add"},"UnitTags":[1002]},
        {"Frame":10,"PlayerID":0,"Type":{"Name":"Select Remove"},"UnitTags":[1001]},
        {"Frame":11,"PlayerID":0,"Type":{"Name":"Train"},"Unit":{"Name":"Probe","ID":64}},
        {"Frame":12,"PlayerID":0,"Type":{"Name":"Unit Morph"},"Unit":{"Name":"Hydralisk","ID":38}},
        {"Frame":13,"PlayerID":0,"Type":{"Name":"Build"},"Unit":{"Name":"Nexus","ID":154}},
        {"Frame":14,"PlayerID":0,"Type":{"Name":"Upgrade"}},
        {"Frame":15,"PlayerID":0,"Type":{"Name":"Train Fighter"}}
      ]}}
    )json";
    const auto replay = smp::parseScrepReplayJson(fixture);
    REQUIRE(replay.players.size() == 2);
    REQUIRE(replay.controlGroupSelections.size() == 1);
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

TEST_CASE("replay evidence recovers a one-off control-group visit missed by the heuristic") {
    smp::AnalysisResult live;
    smp::ReplayData replay = replayWithPlayers();
    addAnchor(live, replay, 1, 0, 0);
    addAnchor(live, replay, 2, 1000, 24);
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
        REQUIRE_NEAR(analyzed.productionVisits[0].startActiveMs, 1900.0, 0.001);
    };
    run("Probe", 0x40, smp::MacroProductType::Worker);
    run("Corsair", 0x3c, smp::MacroProductType::Army);
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
    const auto screen = run(false);
    REQUIRE(screen.productionVisits[0].accessMethod == smp::ProductionAccessMethod::ScreenClick);
    REQUIRE_NEAR(screen.productionVisits[0].startActiveMs, 2050.0, 0.001);
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
    smp::ProductionAnalysis production;
    production.visitsAvailable = true;
    production.productionVisits = {
        classifiedVisit(smp::MacroProductType::Worker, 1000, 1100),
        classifiedVisit(smp::MacroProductType::Army, 2000, 2200,
                        smp::ProductionAccessMethod::LocationHotkeyClick),
    };
    production.productionVisits[0].producedUnits = {"Probe"};
    production.productionVisits[1].producedUnits = {"Dragoon", "Dragoon"};
    production.productionVisits[1].physicalProductionPresses = 3;
    production.productionVisits[1].physicalProductionKeys = {'D', 'D', 'D'};
    production.productionVisits[1].replayProductionCommands = 2;
    production.workerMacroCycles = smp::groupProductionVisits(
        production.productionVisits, smp::MacroProductType::Worker, testQpcFrequency);
    production.armyMacroCycles = smp::groupProductionVisits(
        production.productionVisits, smp::MacroProductType::Army, testQpcFrequency);
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
    const auto encoded = smp::analysisToJson(live, "fixture", production, profile());
    REQUIRE(encoded["schema_version"].asInt() == 3);
    REQUIRE(encoded["macro_cycles"].isNull());
    REQUIRE(encoded["production_visits"]["count"].asInt() == 2);
    const auto& encodedVisits = encoded["production_visits"]["visits"].asArray();
    REQUIRE(encodedVisits[1]["access_method"].asString() ==
            "location_hotkey_click");
    REQUIRE(encodedVisits[1]["physical_production_presses"].asInt() == 3);
    REQUIRE(encodedVisits[1]["physical_production_keys"].asArray().size() == 3);
    const auto& workerCycles = encoded["worker_macro_cycles"]["cycles"].asArray();
    const auto& armyCycles = encoded["army_macro_cycles"]["cycles"].asArray();
    REQUIRE(workerCycles[0]["visit_indices"].asArray()[0].asInt() == 0);
    REQUIRE(armyCycles[0]["visit_indices"].asArray()[0].asInt() == 1);
    REQUIRE(encoded["replay_correlation"]["matched_control_group_events"].asInt() == 8);
    REQUIRE(encoded["replay_correlation"]["replay_created_control_group_visits"].asInt() == 1);
    REQUIRE(encoded["replay_correlation"]["matched_click_visits"].asInt() == 1);
    REQUIRE(encoded["replay_correlation"]["unmatched_replay_production_events"].asInt() == 1);
    REQUIRE(encoded["mechanical_events"].isNull());
    REQUIRE(encoded["replay_commands"].isNull());
}
