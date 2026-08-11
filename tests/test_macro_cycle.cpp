#include "test_framework.h"

#include "analysis/macro_cycle.h"
#include "storage/session.h"
#include "util/json.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t testQpcFrequency = 1000;

const std::string& realisticSettingsJson() {
    static const std::string fixture = R"json({
  "General settings": {
    "Starcraft-Game Custom Hotkeys": true
  },
  "Hotkeys": "STR_MAKE_P_PROBE=e\nSTR_MAKE_P_ZEALOT=z\nSTR_MAKE_P_DRAGOON=d\nSTR_MAKE_P_TEMPLAR=t\nSTR_MAKE_P_DTEMPLAR=q\nSTR_MAKE_P_OBSERVER=q\nSTR_MAKE_P_SHUTTLE=s\nSTR_MAKE_P_REAVER=v\nSTR_MAKE_P_CORSAIR=e\nSTR_MAKE_P_SCOUT=s\nSTR_MAKE_P_ARBITER=a\nSTR_MAKE_P_CARRIER=c\nSTR_MAKE_P_INTERCEPTOR=r\nSTR_MAKE_P_SCARAB=r\nSTR_MAKE_T_MARINE=m\nSTR_MAKE_T_VESSEL=i\nSTR_MAKE_T_BCRUISER=b\nSTR_MAKE_T_FRIGATE=f\nSTR_MAKE_Z_DRONE=w\nSTR_MAKE_Z_MUTALID=u\nSTR_MAKE_Z_AVENGER=x\nSTR_MAKE_Z_INFESTED=n\nSTR_MAKE_Z_LURKER=l\nSTR_ATTACK=a\nSTR_STOP=s\nSTR_MOVE=z\nSTR_PATROL=v\nMALFORMED LINE\n=missing-command\nmissing-key="
})json";
    return fixture;
}

smp::MechanicalInputEvent mechanical(smp::MechanicalInputType type, std::uint64_t ticks,
                                     std::uint16_t key = 0, int value = -1,
                                     std::uint16_t modifiers = smp::ModifierNone,
                                     double activeMs = -1.0) {
    smp::MechanicalInputEvent event;
    event.timestampTicks = ticks;
    event.activeMs = activeMs >= 0.0 ? activeMs : static_cast<double>(ticks);
    event.type = type;
    event.virtualKey = key;
    event.modifiers = modifiers;
    event.value = value;
    return event;
}

void select(std::vector<smp::MechanicalInputEvent>& events, int group, std::uint64_t ticks,
            double activeMs = -1.0) {
    events.push_back(mechanical(smp::MechanicalInputType::ControlGroupSelect, ticks,
                                static_cast<std::uint16_t>('0' + group), group,
                                smp::ModifierNone, activeMs));
}

void key(std::vector<smp::MechanicalInputEvent>& events, char value, std::uint64_t ticks,
         std::uint16_t modifiers = smp::ModifierNone, double activeMs = -1.0) {
    events.push_back(mechanical(smp::MechanicalInputType::KeyPress, ticks,
                                static_cast<std::uint16_t>(value), -1, modifiers, activeMs));
}

void visit(std::vector<smp::MechanicalInputEvent>& events, int group, char productionKey,
           int presses, std::uint64_t startTicks) {
    select(events, group, startTicks);
    for (int index = 0; index < presses; ++index)
        key(events, productionKey, startTicks + 100 + static_cast<std::uint64_t>(index) * 50);
}

smp::MacroHotkeyProfile profile() {
    return smp::parseStarCraftHotkeyProfile(realisticSettingsJson());
}

bool containsGroup(const std::vector<smp::LikelyProductionGroup>& groups, int number) {
    return std::any_of(groups.begin(), groups.end(), [number](const auto& group) {
        return group.group == number;
    });
}

std::vector<smp::LikelyProductionGroup> established(std::initializer_list<std::pair<int, char>> groups) {
    std::vector<smp::LikelyProductionGroup> result;
    for (const auto& [group, productionKey] : groups)
        result.push_back({group, {static_cast<std::uint16_t>(productionKey)}});
    return result;
}

} // namespace

TEST_CASE("real CSettings Hotkeys string preserves production command identities and shared keys") {
    const auto hotkeys = profile();
    REQUIRE(hotkeys.available);
    REQUIRE(hotkeys.customHotkeysEnabled == true);
    REQUIRE(hotkeys.productionCommands.size() >= 16);
    const auto bindingFor = [&](const std::string& command) {
        const auto found = std::find_if(hotkeys.productionCommands.begin(), hotkeys.productionCommands.end(),
                                        [&](const auto& hotkey) { return hotkey.command == command; });
        return found == hotkeys.productionCommands.end() ? std::string{} : found->boundKey;
    };
    REQUIRE(bindingFor("STR_MAKE_P_PROBE") == "e");
    REQUIRE(bindingFor("STR_MAKE_P_DRAGOON") == "d");
    REQUIRE(bindingFor("STR_MAKE_P_DTEMPLAR") == "q");
    REQUIRE(bindingFor("STR_MAKE_P_CORSAIR") == "e");
    const auto eCommands = hotkeys.compatibleProductionCommands('E');
    REQUIRE(eCommands.size() == 2);
    REQUIRE(std::find(eCommands.begin(), eCommands.end(), "STR_MAKE_P_PROBE") != eCommands.end());
    REQUIRE(std::find(eCommands.begin(), eCommands.end(), "STR_MAKE_P_CORSAIR") != eCommands.end());
    REQUIRE(hotkeys.compatibleProductionCommands('D').front() == "STR_MAKE_P_DRAGOON");
    const auto qCommands = hotkeys.compatibleProductionCommands('Q');
    REQUIRE(std::find(qCommands.begin(), qCommands.end(), "STR_MAKE_P_DTEMPLAR") != qCommands.end());
    REQUIRE(std::find(qCommands.begin(), qCommands.end(), "STR_MAKE_P_OBSERVER") != qCommands.end());
    REQUIRE(hotkeys.compatibleProductionCommands('A').front() == "STR_MAKE_P_ARBITER");
    REQUIRE(std::none_of(hotkeys.productionCommands.begin(), hotkeys.productionCommands.end(),
                         [](const auto& hotkey) { return hotkey.command == "STR_ATTACK"; }));
    REQUIRE(std::any_of(hotkeys.parsedBindings.begin(), hotkeys.parsedBindings.end(),
                        [](const auto& hotkey) { return hotkey.command == "STR_ATTACK"; }));
    REQUIRE(std::none_of(hotkeys.parsedBindings.begin(), hotkeys.parsedBindings.end(),
                         [](const auto& hotkey) { return hotkey.command == "MALFORMED LINE"; }));
}

TEST_CASE("production command filtering is explicit for all races and excludes non-production actions") {
    REQUIRE(smp::isOrdinaryProductionCommandIdentifier("STR_MAKE_P_PROBE"));
    REQUIRE(smp::isOrdinaryProductionCommandIdentifier("STR_MAKE_T_MARINE"));
    REQUIRE(smp::isOrdinaryProductionCommandIdentifier("STR_MAKE_T_BCRUISER"));
    REQUIRE(smp::isOrdinaryProductionCommandIdentifier("STR_MAKE_T_FRIGATE"));
    REQUIRE(smp::isOrdinaryProductionCommandIdentifier("STR_MAKE_Z_DRONE"));
    REQUIRE(smp::isOrdinaryProductionCommandIdentifier("STR_MAKE_Z_MUTALID"));
    REQUIRE(smp::isOrdinaryProductionCommandIdentifier("STR_MAKE_Z_AVENGER"));
    REQUIRE(!smp::isOrdinaryProductionCommandIdentifier("STR_ATTACK"));
    REQUIRE(!smp::isOrdinaryProductionCommandIdentifier("STR_RSRCH_STIM"));
    REQUIRE(!smp::isOrdinaryProductionCommandIdentifier("STR_MAKE_P_ARCHON"));
    REQUIRE(!smp::isOrdinaryProductionCommandIdentifier("STR_BUILD_P_NEXUS"));
}

TEST_CASE("missing malformed or disabled custom hotkeys make macro analysis unavailable") {
    const auto missing = smp::parseStarCraftHotkeyProfile(
        R"json({"General settings":{"Starcraft-Game Custom Hotkeys":true}})json");
    REQUIRE(!missing.available);
    REQUIRE(!missing.unavailableReason.empty());
    const auto malformed = smp::parseStarCraftHotkeyProfile("{ definitely not JSON");
    REQUIRE(!malformed.available);
    REQUIRE(!malformed.unavailableReason.empty());
    const auto disabled = smp::parseStarCraftHotkeyProfile(
        R"json({"General settings":{"Starcraft-Game Custom Hotkeys":false},"Hotkeys":"STR_MAKE_P_PROBE=e"})json");
    REQUIRE(!disabled.available);
    REQUIRE(disabled.customHotkeysEnabled == false);

    smp::AnalysisResult cameraStillAvailable;
    cameraStillAvailable.navigationEvents.push_back(
        {100, 100.0, smp::CameraNavigationType::MinimapJump, -1, 10, 10});
    const auto macro = smp::analyzeMacroCycles(cameraStillAvailable, malformed, testQpcFrequency);
    REQUIRE(!macro.available);
    REQUIRE(cameraStillAvailable.navigationEvents.size() == 1);
}

TEST_CASE("production-group inference requires repeated clean visits and learns repeated keys") {
    std::vector<smp::MechanicalInputEvent> strong;
    visit(strong, 5, 'D', 3, 0);
    visit(strong, 5, 'D', 2, 2000);
    visit(strong, 5, 'D', 3, 4000);
    const auto inferred = smp::inferLikelyProductionGroups(strong, profile(), testQpcFrequency);
    REQUIRE(containsGroup(inferred, 5));
    const auto group = std::find_if(inferred.begin(), inferred.end(), [](const auto& value) {
        return value.group == 5;
    });
    REQUIRE(group != inferred.end());
    REQUIRE(group->observedProductionKeys == std::vector<std::uint16_t>{'D'});

    std::vector<smp::MechanicalInputEvent> weak;
    visit(weak, 5, 'D', 1, 0);
    REQUIRE(!containsGroup(smp::inferLikelyProductionGroups(weak, profile(), testQpcFrequency), 5));
}

TEST_CASE("overlapping tactical bindings with mouse targeting do not infer an army group") {
    std::vector<smp::MechanicalInputEvent> events;
    select(events, 1, 0);
    key(events, 'A', 100);
    events.push_back(mechanical(smp::MechanicalInputType::MouseLeftDown, 150));
    select(events, 1, 1000);
    key(events, 'A', 1100);
    events.push_back(mechanical(smp::MechanicalInputType::MouseRightDown, 1150));
    select(events, 1, 2000);
    key(events, 'Z', 2100);
    const auto inferred = smp::inferLikelyProductionGroups(events, profile(), testQpcFrequency);
    REQUIRE(!containsGroup(inferred, 1));
    REQUIRE(smp::analyzeMacroCycles(smp::AnalysisResult{.mechanicalEvents = events}, profile(),
                                    testQpcFrequency).cycles.empty());
}

TEST_CASE("repeated single worker-production visits can establish a production group") {
    std::vector<smp::MechanicalInputEvent> events;
    visit(events, 4, 'E', 1, 0);
    visit(events, 4, 'E', 1, 2000);
    visit(events, 4, 'E', 1, 4000);
    REQUIRE(containsGroup(smp::inferLikelyProductionGroups(events, profile(), testQpcFrequency), 4));

    smp::AnalysisResult withUnrelatedCameraEvent;
    withUnrelatedCameraEvent.mechanicalEvents = events;
    withUnrelatedCameraEvent.navigationEvents.push_back(
        {500, 500.0, smp::CameraNavigationType::MinimapJump, -1, 10, 10});
    const auto analyzed = smp::analyzeMacroCycles(withUnrelatedCameraEvent, profile(), testQpcFrequency);
    REQUIRE(containsGroup(analyzed.likelyProductionGroups, 4));
}

TEST_CASE("an established production visit starts at selection and ends at its final repeated press") {
    std::vector<smp::MechanicalInputEvent> events;
    visit(events, 5, 'D', 3, 1000);
    const auto analysis = smp::detectMacroCyclesForLikelyProductionGroups(
        events, profile(), testQpcFrequency, established({{5, 'D'}}));
    REQUIRE(analysis.cycles.size() == 1);
    const auto& cycle = analysis.cycles.front();
    REQUIRE_NEAR(cycle.startActiveMs, 1000.0, 0.001);
    REQUIRE_NEAR(cycle.endActiveMs, 1200.0, 0.001);
    REQUIRE(cycle.startTimestampTicks == 1000);
    REQUIRE(cycle.endTimestampTicks == 1200);
    REQUIRE_NEAR(cycle.durationMs, 200.0, 0.001);
    REQUIRE(cycle.productionPresses == 3);
    REQUIRE(cycle.productionVisits == 1);
}

TEST_CASE("production visit window includes 750 ms and excludes later keys and modifiers") {
    std::vector<smp::MechanicalInputEvent> boundary;
    select(boundary, 5, 0);
    key(boundary, 'D', 750);
    REQUIRE(smp::detectMacroCyclesForLikelyProductionGroups(
                boundary, profile(), testQpcFrequency, established({{5, 'D'}})).cycles.size() == 1);

    std::vector<smp::MechanicalInputEvent> late;
    select(late, 5, 0);
    key(late, 'D', 751);
    REQUIRE(smp::detectMacroCyclesForLikelyProductionGroups(
                late, profile(), testQpcFrequency, established({{5, 'D'}})).cycles.empty());

    for (const auto modifier : {smp::ModifierCtrl, smp::ModifierShift, smp::ModifierAlt}) {
        std::vector<smp::MechanicalInputEvent> modified;
        select(modified, 5, 0);
        key(modified, 'D', 100, modifier);
        REQUIRE(smp::detectMacroCyclesForLikelyProductionGroups(
                    modified, profile(), testQpcFrequency, established({{5, 'D'}})).cycles.empty());
    }

    std::vector<smp::MechanicalInputEvent> paused;
    select(paused, 5, 0, 0.0);
    key(paused, 'D', 600, smp::ModifierNone, 100.0);
    REQUIRE(smp::detectMacroCyclesForLikelyProductionGroups(
                paused, profile(), testQpcFrequency, established({{5, 'D'}})).cycles.empty());
}

TEST_CASE("control-group assignment without a select cannot create a production visit") {
    std::vector<smp::MechanicalInputEvent> events;
    events.push_back(mechanical(smp::MechanicalInputType::ControlGroupAssign, 0, '5', 5,
                                smp::ModifierCtrl));
    key(events, 'D', 100);
    REQUIRE(smp::detectMacroCyclesForLikelyProductionGroups(
                events, profile(), testQpcFrequency, established({{5, 'D'}})).cycles.empty());
}

TEST_CASE("production visits inside the merge gap form one ordered multi-group macro cycle") {
    std::vector<smp::MechanicalInputEvent> events;
    visit(events, 5, 'D', 3, 0);
    visit(events, 4, 'E', 1, 500);
    visit(events, 6, 'Q', 2, 900);
    const auto analysis = smp::detectMacroCyclesForLikelyProductionGroups(
        events, profile(), testQpcFrequency, established({{5, 'D'}, {4, 'E'}, {6, 'Q'}}));
    REQUIRE(analysis.cycles.size() == 1);
    const auto& cycle = analysis.cycles.front();
    REQUIRE(cycle.startTimestampTicks == 0);
    REQUIRE(cycle.endTimestampTicks == 1050);
    REQUIRE_NEAR(cycle.durationMs, 1050.0, 0.001);
    REQUIRE(cycle.productionPresses == 6);
    REQUIRE(cycle.productionVisits == 3);
    REQUIRE(cycle.controlGroups == std::vector<int>({5, 4, 6}));
}

TEST_CASE("merge-gap boundary joins at 1500 ms and separates beyond it") {
    const auto groups = established({{5, 'D'}, {4, 'E'}});
    std::vector<smp::MechanicalInputEvent> joined;
    visit(joined, 5, 'D', 1, 0);       // final press 100
    visit(joined, 4, 'E', 1, 1600);    // selection gap 1500
    REQUIRE(smp::detectMacroCyclesForLikelyProductionGroups(joined, profile(), testQpcFrequency,
                                                             groups).cycles.size() == 1);

    std::vector<smp::MechanicalInputEvent> separate;
    visit(separate, 5, 'D', 1, 0);
    visit(separate, 4, 'E', 1, 1601);
    REQUIRE(smp::detectMacroCyclesForLikelyProductionGroups(separate, profile(), testQpcFrequency,
                                                             groups).cycles.size() == 2);
}

TEST_CASE("gameplay context changes hard-break macro cycles") {
    const auto groups = established({{5, 'D'}, {4, 'E'}});
    const auto verifyBreak = [&](smp::MechanicalInputEvent breakEvent) {
        std::vector<smp::MechanicalInputEvent> events;
        visit(events, 5, 'D', 1, 0);
        events.push_back(std::move(breakEvent));
        visit(events, 4, 'E', 1, 300);
        REQUIRE(smp::detectMacroCyclesForLikelyProductionGroups(events, profile(), testQpcFrequency,
                                                                 groups).cycles.size() == 2);
    };

    verifyBreak(mechanical(smp::MechanicalInputType::ControlGroupSelect, 200, '1', 1));
    verifyBreak(mechanical(smp::MechanicalInputType::ControlGroupAssign, 200, '5', 5,
                           smp::ModifierCtrl));
    verifyBreak(mechanical(smp::MechanicalInputType::LocationRecall, 200, VK_F2, 2));
    verifyBreak(mechanical(smp::MechanicalInputType::LocationAssign, 200, VK_F2, 2,
                           smp::ModifierShift));
    verifyBreak(mechanical(smp::MechanicalInputType::MouseLeftDown, 200));
    verifyBreak(mechanical(smp::MechanicalInputType::MouseWheel, 200));
    verifyBreak(mechanical(smp::MechanicalInputType::KeyPress, 200, 'X'));
}

TEST_CASE("standalone Ctrl Shift and Alt do not hard-break a macro cycle") {
    std::vector<smp::MechanicalInputEvent> events;
    visit(events, 5, 'D', 1, 0);
    events.push_back(mechanical(smp::MechanicalInputType::KeyPress, 150, VK_CONTROL,
                                -1, smp::ModifierCtrl));
    events.push_back(mechanical(smp::MechanicalInputType::KeyPress, 175, VK_SHIFT,
                                -1, smp::ModifierShift));
    events.push_back(mechanical(smp::MechanicalInputType::KeyPress, 200, VK_MENU,
                                -1, smp::ModifierAlt));
    visit(events, 4, 'E', 1, 300);
    const auto analysis = smp::detectMacroCyclesForLikelyProductionGroups(
        events, profile(), testQpcFrequency, established({{5, 'D'}, {4, 'E'}}));
    REQUIRE(analysis.cycles.size() == 1);
    REQUIRE(analysis.cycles.front().productionVisits == 2);
}

TEST_CASE("real QPC time prevents an alt-tab merge despite a close active timeline") {
    std::vector<smp::MechanicalInputEvent> events;
    select(events, 5, 0, 0.0);
    key(events, 'D', 100, smp::ModifierNone, 100.0);
    select(events, 4, 1000, 150.0);
    key(events, 'E', 1100, smp::ModifierNone, 250.0);
    const auto analysis = smp::detectMacroCyclesForLikelyProductionGroups(
        events, profile(), testQpcFrequency, established({{5, 'D'}, {4, 'E'}}));
    REQUIRE(analysis.cycles.size() == 2);
    REQUIRE_NEAR(analysis.cycles[1].startActiveMs, 150.0, 0.001);
}

TEST_CASE("macro statistics use pooled individual durations and zero cycles remain N A") {
    const auto stats = smp::summarizeMacroCycles({
        smp::MacroCycle{.durationMs = 700.0},
        smp::MacroCycle{.durationMs = 1000.0},
        smp::MacroCycle{.durationMs = 1500.0},
    });
    REQUIRE(stats.cycles.size() == 3);
    REQUIRE_NEAR(*stats.averageDurationMs, 1066.666666, 0.001);
    REQUIRE_NEAR(*stats.bestDurationMs, 700.0, 0.001);
    REQUIRE_NEAR(*stats.slowestDurationMs, 1500.0, 0.001);

    const auto empty = smp::summarizeMacroCycles({});
    REQUIRE(empty.cycles.empty());
    REQUIRE(!empty.averageDurationMs);
    REQUIRE(!empty.bestDurationMs);
    REQUIRE(!empty.slowestDurationMs);
}

TEST_CASE("analysis JSON contains compact macro timeline and hotkey snapshot without mechanical events") {
    smp::AnalysisResult result;
    result.activeDurationSeconds = 600.0;
    auto macro = smp::summarizeMacroCycles({smp::MacroCycle{
        .startActiveMs = 312400.0,
        .endActiveMs = 313480.0,
        .durationMs = 1080.0,
        .startTimestampTicks = 5000,
        .endTimestampTicks = 6080,
        .productionPresses = 7,
        .productionVisits = 3,
        .controlGroups = {5, 4, 6},
    }});
    const auto encoded = smp::analysisToJson(result, "fixture", macro, profile());
    const auto& cycles = encoded["macro_cycles"];
    REQUIRE(cycles["available"].asBool());
    REQUIRE(cycles["count"].asInt() == 1);
    REQUIRE_NEAR(cycles["average_duration_ms"].asNumber(), 1080.0, 0.001);
    REQUIRE_NEAR(cycles["best_duration_ms"].asNumber(), 1080.0, 0.001);
    REQUIRE_NEAR(cycles["slowest_duration_ms"].asNumber(), 1080.0, 0.001);
    const auto& cycle = cycles["cycles"].asArray().front();
    REQUIRE_NEAR(cycle["start_active_ms"].asNumber(), 312400.0, 0.001);
    REQUIRE_NEAR(cycle["end_active_ms"].asNumber(), 313480.0, 0.001);
    REQUIRE_NEAR(cycle["duration_ms"].asNumber(), 1080.0, 0.001);
    REQUIRE(cycle["production_presses"].asInt() == 7);
    REQUIRE(cycle["production_visits"].asInt() == 3);
    REQUIRE(cycle["control_groups"].asArray().size() == 3);
    REQUIRE(encoded["macro_hotkeys"]["production_bindings"]["STR_MAKE_P_DRAGOON"].asString() == "d");
    const auto text = smp::json::stringify(encoded);
    REQUIRE(text.find("mechanical_events") == std::string::npos);
    REQUIRE(text.find("mechanicalEvents") == std::string::npos);

    smp::MacroCycleAnalysis unavailable;
    unavailable.available = false;
    unavailable.unavailableReason = "fixture unavailable";
    const auto unavailableJson = smp::analysisToJson(result, "unavailable", unavailable, {});
    REQUIRE(!unavailableJson["macro_cycles"]["available"].asBool(true));
    REQUIRE(unavailableJson["macro_cycles"]["reason"].asString() == "fixture unavailable");
}

TEST_CASE("nav and atomically finalized analysis JSON coexist and JSON failure preserves nav") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("starcraft-mechanics-profiler-macro-persistence-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    const auto navPath = root / "fixture.nav";
    smp::writeNavSession(navPath, {}, "fixture", testQpcFrequency, 1000);
    const auto jsonPath = smp::writeAnalysisJson(navPath, smp::analysisToJson({}, "fixture"));
    REQUIRE(std::filesystem::is_regular_file(navPath));
    REQUIRE(std::filesystem::is_regular_file(jsonPath));
    REQUIRE(!std::filesystem::exists(root / "fixture.json.tmp"));
    REQUIRE(smp::json::parseFile(jsonPath)["session"]["id"].asString() == "fixture");

    const auto failedNav = root / "failed.nav";
    smp::writeNavSession(failedNav, {}, "failed", testQpcFrequency, 2000);
    std::filesystem::create_directories(root / "failed.json");
    bool threw = false;
    try {
        (void)smp::writeAnalysisJson(failedNav, smp::analysisToJson({}, "failed"));
    } catch (...) {
        threw = true;
    }
    REQUIRE(threw);
    REQUIRE(std::filesystem::is_regular_file(failedNav));
    REQUIRE(!std::filesystem::exists(root / "failed.json.tmp"));
    std::filesystem::remove_all(root);
}
