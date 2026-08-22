#include "test_framework.h"

#include "app/game_analysis_visualization_model.h"

#include <string>

namespace {

smp::NavSession navFixture() {
    smp::NavSession nav;
    nav.sessionId = "timeline-fixture";
    nav.analysis.activeDurationSeconds = 90.0;
    nav.analysis.navigationEvents = {
        {300, 3000.125, smp::CameraNavigationType::MinimapJump, -1, 20, 30, 0.0, smp::EdgeDirection::None, 20, 30},
        {100, 1000.250, smp::CameraNavigationType::EdgeScroll, -1, 1919, 500, 380.5, smp::EdgeDirection::Right, 1919,
         480},
        {200, 2000.750, smp::CameraNavigationType::ControlGroupJump, 3, 10, 10, 0.0, smp::EdgeDirection::None, 10, 10},
    };
    return nav;
}

smp::json::Value derivedFixture() {
    using Object = smp::json::Value::Object;
    using Array = smp::json::Value::Array;
    smp::json::Value root(Object{});
    root["session"] = Object{{"id", "timeline-fixture"}, {"active_duration_seconds", 90.0}};
    root["worker_macro_cycles"] = Object{
        {"available", true},
        {"cycles",
         Array{
             Object{{"start_active_ms", 10000.0},
                    {"execution_end_active_ms", 11200.0},
                    {"end_active_ms", 11600.0},
                    {"duration_ms", 1200.0},
                    {"full_span_ms", 1600.0},
                    {"visit_count", 3},
                    {"macro_access_style", "location_hotkey_click"}},
             Object{{"start_active_ms", 5000.0},
                    {"execution_end_active_ms", 5700.0},
                    {"end_active_ms", 6100.0},
                    {"duration_ms", 700.0},
                    {"full_span_ms", 1100.0},
                    {"visit_count", 1},
                    {"macro_access_style", "control_group_only"}},
         }},
    };
    root["army_macro_cycles"] = Object{
        {"available", true},
        {"cycles", Array{Object{{"start_active_ms", 20000.0},
                                {"execution_end_active_ms", 21800.0},
                                {"end_active_ms", 22500.0},
                                {"duration_ms", 1800.0},
                                {"full_span_ms", 2500.0},
                                {"visit_count", 4},
                                {"macro_access_style", "mixed"}}}},
    };
    root["production_visits"] = Object{
        {"available", true},
        {"visits", Array{Object{
                       {"product_type", "worker"},
                       {"start_active_ms", 10000.0},
                       {"context_active_ms", 10100.0},
                       {"first_production_active_ms", 10400.0},
                       {"end_active_ms", 10800.0},
                       {"selection_access", "direct_click"},
                       {"camera_access", "location_hotkey"},
                       {"control_group", nullptr},
                       {"location_hotkey", 2},
                       {"production_context", Object{{"kind", "location_hotkey"}, {"location_hotkey", 2}}},
                       {"produced_units", Array{"Probe"}},
                       {"physical_production_presses", 2},
                       {"replay_confirmed", true},
                       {"access_latency_ms", 100.0},
                       {"production_latency_ms", 300.0},
                       {"execution_duration_ms", 400.0},
                   }}},
    };
    root["army_control_group_management"] = Object{
        {"available", true},
        {"total_group_edits_per_minute", 1.3},
        {"edits",
         Array{
             Object{{"operation_active_ms", 31000.5},
                    {"group", 2},
                    {"operation", "add"},
                    {"selection_method", "box_select"},
                    {"selection_to_operation_ms", nullptr},
                    {"selection_duration_ms", 82.0},
                    {"total_execution_ms", 170.0},
                    {"scope", "army"},
                    {"replay_confirmed", true},
                    {"binding_confidence", "replay_confirmed"}},
             Object{{"operation_active_ms", 30000.0},
                    {"group", 1},
                    {"operation", "assign"},
                    {"selection_method", "direct_click"},
                    {"selection_to_operation_ms", 118.0},
                    {"selection_duration_ms", 42.0},
                    {"total_execution_ms", 160.0},
                    {"scope", "army"},
                    {"replay_confirmed", true},
                    {"binding_confidence", "replay_confirmed"}},
             Object{{"operation_active_ms", 30500.0},
                    {"group", 4},
                    {"operation", "assign"},
                    {"selection_method", "existing_selection"},
                    {"selection_to_operation_ms", 25.0},
                    {"scope", "production_building"},
                    {"replay_confirmed", true},
                    {"binding_confidence", "replay_confirmed"}},
         }},
        {"scouting_unit_activity",
         Array{
             Object{{"group", 1},
                    {"assignment_generation", 2},
                    {"assigned_active_ms", 42000.0},
                    {"last_command_active_ms", nullptr},
                    {"activity_duration_ms", nullptr},
                    {"selection_count", 1},
                    {"command_count", 0}},
             Object{{"group", 1},
                    {"assignment_generation", 1},
                    {"assigned_active_ms", 40000.0},
                    {"last_command_active_ms", 47000.0},
                    {"activity_duration_ms", 7000.0},
                    {"selection_count", 3},
                    {"command_count", 4}},
         }},
    };
    return root;
}

} // namespace

TEST_CASE("visualization navigation retains exact active times") {
    const auto nav = navFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(&nav, nullptr);
    REQUIRE_NEAR(model.navigationEvents[0].activeMs, 1000.250, 0.000001);
    REQUIRE_NEAR(model.navigationEvents[1].activeMs, 2000.750, 0.000001);
    REQUIRE_NEAR(model.navigationEvents[2].activeMs, 3000.125, 0.000001);
}

TEST_CASE("visualization edge interval retains its duration") {
    const auto nav = navFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(&nav, nullptr);
    REQUIRE(model.navigationEvents[0].type == smp::CameraNavigationType::EdgeScroll);
    REQUIRE_NEAR(model.navigationEvents[0].durationMs, 380.5, 0.000001);
}

TEST_CASE("visualization Worker cycle uses execution completion") {
    const auto derived = derivedFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE_NEAR(model.workerMacroCycles[0].startActiveMs, 5000.0, 0.001);
    REQUIRE_NEAR(model.workerMacroCycles[0].executionEndActiveMs, 5700.0, 0.001);
    REQUIRE_NEAR(model.workerMacroCycles[0].durationMs, 700.0, 0.001);
}

TEST_CASE("visualization Army cycle uses execution completion") {
    const auto derived = derivedFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE_NEAR(model.armyMacroCycles[0].startActiveMs, 20000.0, 0.001);
    REQUIRE_NEAR(model.armyMacroCycles[0].executionEndActiveMs, 21800.0, 0.001);
}

TEST_CASE("visualization macro full-span tail remains separate") {
    const auto derived = derivedFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE_NEAR(model.armyMacroCycles[0].executionEndActiveMs, 21800.0, 0.001);
    REQUIRE_NEAR(model.armyMacroCycles[0].endActiveMs, 22500.0, 0.001);
    REQUIRE_NEAR(model.armyMacroCycles[0].fullSpanMs, 2500.0, 0.001);
}

TEST_CASE("visualization production stages are preserved") {
    const auto derived = derivedFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    const auto& visit = model.productionVisits[0];
    REQUIRE_NEAR(visit.startActiveMs, 10000.0, 0.001);
    REQUIRE_NEAR(visit.contextActiveMs, 10100.0, 0.001);
    REQUIRE_NEAR(visit.firstProductionActiveMs, 10400.0, 0.001);
    REQUIRE_NEAR(visit.endActiveMs, 10800.0, 0.001);
}

TEST_CASE("visualization production timing meanings remain independent") {
    const auto derived = derivedFixture();
    const auto model =
        smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    const auto& visit = model.productionVisits[0];
    REQUIRE_NEAR(visit.accessLatencyMs, 100.0, 0.001);
    REQUIRE_NEAR(visit.productionLatencyMs, 300.0, 0.001);
    REQUIRE_NEAR(visit.executionDurationMs, 400.0, 0.001);
}

TEST_CASE("visualization access-style timing is separated by macro product") {
    using Object = smp::json::Value::Object;
    using Array = smp::json::Value::Array;
    auto derived = derivedFixture();
    derived["worker_macro_cycles"]["cycles"] = Array{
        Object{{"start_active_ms", 1000.0},
               {"execution_end_active_ms", 1700.0},
               {"end_active_ms", 1700.0},
               {"duration_ms", 700.0},
               {"full_span_ms", 700.0},
               {"visit_count", 1},
               {"macro_access_style", "control_group_only"}},
        Object{{"start_active_ms", 2000.0},
               {"execution_end_active_ms", 2900.0},
               {"end_active_ms", 2900.0},
               {"duration_ms", 900.0},
               {"full_span_ms", 900.0},
               {"visit_count", 1},
               {"macro_access_style", "control_group_only"}},
        Object{{"start_active_ms", 3000.0},
               {"execution_end_active_ms", 4100.0},
               {"end_active_ms", 4100.0},
               {"duration_ms", 1100.0},
               {"full_span_ms", 1100.0},
               {"visit_count", 1},
               {"macro_access_style", "location_hotkey_click"}},
    };
    derived["army_macro_cycles"]["cycles"] = Array{
        Object{{"start_active_ms", 5000.0},
               {"execution_end_active_ms", 6800.0},
               {"end_active_ms", 6800.0},
               {"duration_ms", 1800.0},
               {"full_span_ms", 1800.0},
               {"visit_count", 2},
               {"macro_access_style", "control_group_only"}},
        Object{{"start_active_ms", 7000.0},
               {"execution_end_active_ms", 9200.0},
               {"end_active_ms", 9200.0},
               {"duration_ms", 2200.0},
               {"full_span_ms", 2200.0},
               {"visit_count", 2},
               {"macro_access_style", "control_group_only"}},
        Object{{"start_active_ms", 10000.0},
               {"execution_end_active_ms", 12500.0},
               {"end_active_ms", 12500.0},
               {"duration_ms", 2500.0},
               {"full_span_ms", 2500.0},
               {"visit_count", 2},
               {"macro_access_style", "mixed"}},
    };

    const auto model =
        smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE(model.workerAccessStyleDurations.size() == 2);
    REQUIRE(model.armyAccessStyleDurations.size() == 2);

    const auto& workerControlGroup = model.workerAccessStyleDurations[0];
    REQUIRE(workerControlGroup.accessStyle == "control_group_only");
    REQUIRE(workerControlGroup.durationMs.size() == 2);
    REQUIRE_NEAR(workerControlGroup.durationMs[0], 700.0, 0.001);
    REQUIRE_NEAR(workerControlGroup.durationMs[1], 900.0, 0.001);
    REQUIRE_NEAR(*workerControlGroup.medianMs, 800.0, 0.001);

    const auto& armyControlGroup = model.armyAccessStyleDurations[0];
    REQUIRE(armyControlGroup.accessStyle == "control_group_only");
    REQUIRE(armyControlGroup.durationMs.size() == 2);
    REQUIRE_NEAR(armyControlGroup.durationMs[0], 1800.0, 0.001);
    REQUIRE_NEAR(armyControlGroup.durationMs[1], 2200.0, 0.001);
    REQUIRE_NEAR(*armyControlGroup.medianMs, 2000.0, 0.001);

    REQUIRE(model.workerAccessStyleDurations[1].accessStyle ==
            "location_hotkey_click");
    REQUIRE(model.workerAccessStyleDurations[1].durationMs.size() == 1);
    REQUIRE(model.armyAccessStyleDurations[1].accessStyle == "mixed");
    REQUIRE(model.armyAccessStyleDurations[1].durationMs.size() == 1);
}

TEST_CASE("visualization access-style timing handles an empty product cleanly") {
    using Array = smp::json::Value::Array;
    auto derived = derivedFixture();
    derived["army_macro_cycles"]["cycles"] = Array{};
    const auto model =
        smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE(!model.workerAccessStyleDurations.empty());
    REQUIRE(model.armyAccessStyleDurations.empty());
}

TEST_CASE("analysis model reload decision distinguishes latest results") {
    smp::GameAnalysisVisualizationModel current;
    current.sessionId = "game-a";
    current.jsonPath = L"sessions/game-a.json";
    current.navPath = L"sessions/game-a.nav";

    auto same = current;
    REQUIRE(!smp::shouldReloadAnalysisModel(current, same));

    auto newer = current;
    newer.sessionId = "game-b";
    newer.jsonPath = L"sessions/game-b.json";
    newer.navPath = L"sessions/game-b.nav";
    REQUIRE(smp::shouldReloadAnalysisModel(current, newer));
}

TEST_CASE("visualization control-group edits use operation active time") {
    const auto derived = derivedFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE(model.armyControlGroupEdits.size() == 2);
    REQUIRE_NEAR(model.armyControlGroupEdits[0].operationActiveMs, 30000.0, 0.001);
}

TEST_CASE("visualization model carries the derived Army control-group edit rate") {
    const auto derived = derivedFixture();
    const auto model =
        smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE(model.armyControlGroupEditsPerMinute.has_value());
    REQUIRE_NEAR(*model.armyControlGroupEditsPerMinute, 1.3, 0.001);
}

TEST_CASE("visualization Army control-group edit rate distinguishes unavailable from zero") {
    auto unavailable = derivedFixture();
    unavailable["army_control_group_management"]["available"] = false;
    unavailable["army_control_group_management"]["reason"] =
        "Replay correlation unavailable";
    const auto unavailableModel =
        smp::buildGameAnalysisVisualizationModel(nullptr, &unavailable);
    REQUIRE(!unavailableModel.controlGroupEditStatus.available);
    REQUIRE(!unavailableModel.armyControlGroupEditsPerMinute.has_value());

    auto zero = derivedFixture();
    zero["army_control_group_management"]
        ["total_group_edits_per_minute"] = 0.0;
    const auto zeroModel =
        smp::buildGameAnalysisVisualizationModel(nullptr, &zero);
    REQUIRE(zeroModel.controlGroupEditStatus.available);
    REQUIRE(zeroModel.armyControlGroupEditsPerMinute.has_value());
    REQUIRE_NEAR(*zeroModel.armyControlGroupEditsPerMinute, 0.0, 0.001);
}

TEST_CASE("visualization missing control-group latency remains missing") {
    const auto derived = derivedFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE(!model.armyControlGroupEdits[1].selectionToOperationMs.has_value());
}

TEST_CASE("visualization scouting interval uses assignment through last command") {
    const auto derived = derivedFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE_NEAR(model.scoutingActivities[0].assignedActiveMs, 40000.0, 0.001);
    REQUIRE_NEAR(*model.scoutingActivities[0].lastCommandActiveMs, 47000.0, 0.001);
    REQUIRE_NEAR(*model.scoutingActivities[0].activityDurationMs, 7000.0, 0.001);
}

TEST_CASE("visualization scout without commands has no fabricated duration") {
    const auto derived = derivedFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE(!model.scoutingActivities[1].lastCommandActiveMs.has_value());
    REQUIRE(!model.scoutingActivities[1].activityDurationMs.has_value());
}

TEST_CASE("visualization scouting assignment generations remain separate") {
    const auto derived = derivedFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE(model.scoutingActivities.size() == 2);
    REQUIRE(model.scoutingActivities[0].assignmentGeneration == 1);
    REQUIRE(model.scoutingActivities[1].assignmentGeneration == 2);
}

TEST_CASE("visualization model sorting is deterministic") {
    const auto nav = navFixture();
    const auto derived = derivedFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(&nav, &derived);
    REQUIRE(model.navigationEvents[0].activeMs < model.navigationEvents[1].activeMs);
    REQUIRE(model.workerMacroCycles[0].startActiveMs < model.workerMacroCycles[1].startActiveMs);
    REQUIRE(model.armyControlGroupEdits[0].operationActiveMs < model.armyControlGroupEdits[1].operationActiveMs);
}

TEST_CASE("visualization model degrades independently when nav or JSON is missing") {
    const auto nav = navFixture();
    const auto derived = derivedFixture();
    const auto navOnly = smp::buildGameAnalysisVisualizationModel(&nav, nullptr);
    REQUIRE(navOnly.navigationStatus.available);
    REQUIRE(!navOnly.workerMacroStatus.available);
    const auto jsonOnly = smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE(!jsonOnly.navigationStatus.available);
    REQUIRE(jsonOnly.workerMacroStatus.available);
    REQUIRE(jsonOnly.controlGroupEditStatus.available);
}

TEST_CASE("visualization model does not mutate source analysis data") {
    const auto nav = navFixture();
    const auto originalEvents = nav.analysis.navigationEvents;
    auto derived = derivedFixture();
    const std::string originalJson = smp::json::stringify(derived);
    (void)smp::buildGameAnalysisVisualizationModel(&nav, &derived);
    REQUIRE(nav.analysis.navigationEvents.size() == originalEvents.size());
    REQUIRE_NEAR(nav.analysis.navigationEvents[0].activeMs, originalEvents[0].activeMs, 0.000001);
    REQUIRE(smp::json::stringify(derived) == originalJson);
}

TEST_CASE("visualization excludes non-Army edits from the headline track") {
    const auto derived = derivedFixture();
    const auto model = smp::buildGameAnalysisVisualizationModel(nullptr, &derived);
    REQUIRE(model.armyControlGroupEdits.size() == 2);
    for (const auto& edit : model.armyControlGroupEdits)
        REQUIRE(edit.scope == "army");
}
