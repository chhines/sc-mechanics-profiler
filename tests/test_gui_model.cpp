#include "test_framework.h"

#include "app/application_paths.h"
#include "app/gui_preferences.h"
#include "app/results_view_model.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

std::filesystem::path temporaryRoot(const char* label) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           (std::string("starcraft-mechanics-profiler-") + label + "-" +
            std::to_string(nonce));
}

smp::json::Value summaryFixture() {
    smp::json::Value root(smp::json::Value::Object{});
    root["session"] = smp::json::Value::Object{{"id", "fixture"},
                                                {"active_duration_seconds", 60.0}};
    root["camera_navigation"] = smp::json::Value::Object{
        {"total_transitions", 10},
        {"transitions_per_minute", 10.0},
        {"control_group", smp::json::Value::Object{{"transitions", 4}}},
        {"location_hotkey", smp::json::Value::Object{{"transitions", 3}}},
        {"minimap", smp::json::Value::Object{{"transitions", 2}}},
        {"edge_scroll", smp::json::Value::Object{{"episodes", 1}}},
    };
    const auto macro = smp::json::Value::Object{
        {"available", true},
        {"count", 2},
        {"average_duration_ms", 1200.0},
        {"best_duration_ms", 900.0},
        {"slowest_duration_ms", 1500.0},
        {"production_visit_count", 3},
        {"macro_access_styles",
         smp::json::Value::Object{
             {"control_group_only",
              smp::json::Value::Object{{"cycle_count", 2}, {"percentage", 100.0},
                                       {"median_duration_ms", 1200.0}}}}},
    };
    root["worker_macro_cycles"] = macro;
    root["army_macro_cycles"] = macro;
    root["army_control_group_management"] = smp::json::Value::Object{
        {"available", true},
        {"assignments", 2},
        {"additions", 1},
        {"total_group_edits_per_minute", 3.0},
        {"excluded_scouting_unit_edits", 1},
        {"excluded_production_building_edits", 0},
        {"assignment_methods",
         smp::json::Value::Object{
             {"box_select", smp::json::Value::Object{{"edit_count", 2},
                                                       {"percentage", 100.0},
                                                       {"average_selection_to_operation_ms", 125.0}}}}},
        {"addition_methods", smp::json::Value::Object{}},
        {"scouting_unit_activity",
         smp::json::Value::Array{smp::json::Value::Object{
             {"group", 1}, {"assignment_generation", 1},
             {"activity_duration_ms", 87000.0}, {"selection_count", 3},
             {"command_count", 3}, {"last_command_active_ms", 127000.0}}}},
    };
    return root;
}

} // namespace

TEST_CASE("GUI application paths share the executable directory as their data root") {
    const std::filesystem::path executable =
        LR"(C:\Portable\Starcraft Profiler\Starcraft Mechanics Profiler.exe)";
    const auto paths = smp::guiApplicationPaths(executable);
    const std::filesystem::path root = LR"(C:\Portable\Starcraft Profiler)";
    REQUIRE(paths.dataRoot == root);
    REQUIRE(paths.config == root / "config.json");
    REQUIRE(paths.preferences == root / "gui-config.json");
    REQUIRE(paths.sessions == root / "sessions");
    REQUIRE(paths.exports == root / "exports");
}

TEST_CASE("current GUI application root is independent of the process current directory") {
    const auto originalCurrentDirectory = std::filesystem::current_path();
    const auto expected = smp::currentGuiApplicationPaths();
    const auto otherDirectory = temporaryRoot("different-current-directory");
    std::filesystem::create_directories(otherDirectory);
    std::filesystem::current_path(otherDirectory);
    const auto actual = smp::currentGuiApplicationPaths();
    std::filesystem::current_path(originalCurrentDirectory);
    std::filesystem::remove_all(otherDirectory);
    REQUIRE(actual == expected);
    REQUIRE(actual.dataRoot != otherDirectory);
}

TEST_CASE("tray preference selects consistent minimize and close actions") {
    REQUIRE(smp::minimizeAction(true) == smp::MainWindowAction::HideToTray);
    REQUIRE(smp::closeAction(true) == smp::MainWindowAction::HideToTray);
    REQUIRE(smp::minimizeAction(false) == smp::MainWindowAction::MinimizeNormally);
    REQUIRE(smp::closeAction(false) == smp::MainWindowAction::Exit);
}

TEST_CASE("GUI preferences default to every report group and minimize to tray") {
    const auto defaults = smp::GuiPreferences::defaults();
    REQUIRE(defaults.reports.cameraNavigation);
    REQUIRE(defaults.reports.workerMacroCycles);
    REQUIRE(defaults.reports.armyMacroCycles);
    REQUIRE(defaults.reports.macroAccessStyles);
    REQUIRE(defaults.reports.armyControlGroupManagement);
    REQUIRE(defaults.reports.scoutingUnitActivity);
    REQUIRE(defaults.minimizeToTray);
    REQUIRE(!defaults.window.has_value());
}

TEST_CASE("GUI preferences round trip report visibility and window placement") {
    const auto root = temporaryRoot("gui-preferences");
    std::filesystem::create_directories(root);
    const auto path = root / "gui-config.json";
    auto expected = smp::GuiPreferences::defaults();
    expected.reports.cameraNavigation = false;
    expected.reports.scoutingUnitActivity = false;
    expected.minimizeToTray = false;
    expected.window = smp::GuiWindowPlacement{40, 50, 900, 700};
    expected.save(path);
    REQUIRE(smp::GuiPreferences::load(path) == expected);
    std::filesystem::remove_all(root);
}

TEST_CASE("missing or corrupt GUI preferences safely return defaults") {
    const auto root = temporaryRoot("gui-corrupt");
    std::filesystem::create_directories(root);
    const auto path = root / "gui-config.json";
    REQUIRE(smp::GuiPreferences::load(path) == smp::GuiPreferences::defaults());
    std::ofstream(path, std::ios::binary) << "{ definitely not JSON";
    REQUIRE(smp::GuiPreferences::load(path) == smp::GuiPreferences::defaults());
    std::filesystem::remove_all(root);
}

TEST_CASE("invalid GUI window geometry is discarded without losing valid report choices") {
    const auto root = temporaryRoot("gui-invalid-window");
    std::filesystem::create_directories(root);
    const auto path = root / "gui-config.json";
    std::ofstream(path, std::ios::binary)
        << R"({"reports":{"camera_navigation":false},"window":{"x":0,"y":0,"width":10,"height":10}})";
    const auto loaded = smp::GuiPreferences::load(path);
    REQUIRE(!loaded.reports.cameraNavigation);
    REQUIRE(loaded.reports.workerMacroCycles);
    REQUIRE(!loaded.window.has_value());
    std::filesystem::remove_all(root);
}

TEST_CASE("game results view model filters statistic groups without changing source data") {
    const auto summary = summaryFixture();
    smp::ReportGroupVisibility visibility;
    visibility.workerMacroCycles = false;
    visibility.macroAccessStyles = false;
    visibility.scoutingUnitActivity = false;
    const auto filtered = smp::deriveGameResults(summary, visibility);
    REQUIRE(filtered.hasSection("camera_navigation"));
    REQUIRE(!filtered.hasSection("worker_macro"));
    REQUIRE(filtered.hasSection("army_macro"));
    REQUIRE(!filtered.hasSection("worker_access_styles"));
    REQUIRE(filtered.hasSection("army_control_groups"));
    REQUIRE(!filtered.hasSection("scouting_activity"));

    const auto complete = smp::deriveGameResults(summary, smp::ReportGroupVisibility{});
    REQUIRE(complete.hasSection("worker_macro"));
    REQUIRE(complete.hasSection("worker_access_styles"));
    REQUIRE(complete.hasSection("scouting_activity"));
    REQUIRE(summary["camera_navigation"]["total_transitions"].asInt() == 10);
}

TEST_CASE("session results view model uses pooled existing statistics") {
    smp::AutomaticSessionStats stats;
    stats.games = 2;
    stats.activeSeconds = 120.0;
    stats.controlGroupJumps = 8;
    stats.locationHotkeyJumps = 4;
    stats.armyControlGroups.available = true;
    stats.armyControlGroups.activeDurationSeconds = 120.0;
    const auto model = smp::deriveSessionResults(stats, smp::ReportGroupVisibility{});
    REQUIRE(model.hasSection("camera_navigation"));
    REQUIRE(model.hasSection("army_control_groups"));
    REQUIRE(model.subtitle.find("2 completed") != std::string::npos);
}
