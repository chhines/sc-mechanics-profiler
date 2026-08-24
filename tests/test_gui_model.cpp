#include "test_framework.h"

#include "app/application_paths.h"
#include "app/analysis_multitasking.h"
#include "app/gui_preferences.h"
#include "app/gui_single_instance.h"
#include "app/results_view_model.h"
#include "app/session_trend_data.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>

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
        {"count", 5},
        {"average_duration_ms", 1200.0},
        {"best_duration_ms", 500.0},
        {"slowest_duration_ms", 4500.0},
        {"production_visit_count", 7},
        {"cycles",
         smp::json::Value::Array{
             smp::json::Value::Object{{"start_active_ms", 1000.0},
                                      {"execution_end_active_ms", 1500.0},
                                      {"end_active_ms", 1600.0},
                                      {"duration_ms", 500.0}},
             smp::json::Value::Object{{"start_active_ms", 5000.0},
                                      {"execution_end_active_ms", 6500.0},
                                      {"end_active_ms", 6600.0},
                                      {"duration_ms", 1500.0}},
             smp::json::Value::Object{{"start_active_ms", 15000.0},
                                      {"execution_end_active_ms", 17500.0},
                                      {"end_active_ms", 17600.0},
                                      {"duration_ms", 2500.0}},
             smp::json::Value::Object{{"start_active_ms", 30000.0},
                                      {"execution_end_active_ms", 33500.0},
                                      {"end_active_ms", 33600.0},
                                      {"duration_ms", 3500.0}},
             smp::json::Value::Object{{"start_active_ms", 55000.0},
                                      {"execution_end_active_ms", 59500.0},
                                      {"end_active_ms", 59600.0},
                                      {"duration_ms", 4500.0}},
         }},
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
        {"scouting_outcome_data_available", true},
        {"edits",
         smp::json::Value::Array{smp::json::Value::Object{
             {"operation_active_ms", 20000.0}, {"scope", "army"},
             {"operation", "assign"}, {"selection_method", "box_select"}}}},
        {"assignment_methods",
         smp::json::Value::Object{
             {"box_select", smp::json::Value::Object{{"edit_count", 2},
                                                       {"percentage", 100.0},
                                                       {"average_selection_to_operation_ms", 125.0}}}}},
        {"addition_methods", smp::json::Value::Object{}},
        {"scouting_unit_activity",
         smp::json::Value::Array{smp::json::Value::Object{
             {"group", 1}, {"assignment_generation", 1},
             {"assigned_active_ms", 15000.0},
             {"activity_duration_ms", 42000.0}, {"selection_count", 3},
             {"command_count", 3}, {"last_command_active_ms", 57000.0},
             {"longest_command_gap_ms", 21000.0},
             {"command_active_ms", smp::json::Value::Array{40000.0, 59000.0}},
             {"outcome_available", true}, {"returned_home", true},
             {"resumed_after_temporary_return", true}}}},
    };
    root["army_command_activity"] = smp::json::Value::Object{
        {"available", true}, {"command_count", 4},
        {"commands_per_minute", 4.0}, {"median_gap_ms", 2000.0},
        {"p90_gap_ms", 5000.0}, {"longest_gap_ms", 6000.0},
        {"commands",
         smp::json::Value::Array{
             smp::json::Value::Object{{"active_ms", 10000.0}},
             smp::json::Value::Object{{"active_ms", 12000.0}},
             smp::json::Value::Object{{"active_ms", 17000.0}},
             smp::json::Value::Object{{"active_ms", 23000.0}},
         }},
    };
    root["ability_activity"] = smp::json::Value::Object{
        {"available", true}, {"total_uses", 3},
        {"abilities_per_minute", 3.0},
        {"by_ability",
         smp::json::Value::Object{
             {"Psionic Storm",
              smp::json::Value::Object{{"uses", 2},
                                       {"uses_per_minute", 2.0}}},
             {"Recall", smp::json::Value::Object{{"uses", 1},
                                                  {"uses_per_minute", 1.0}}},
         }},
    };
    return root;
}

smp::GameAnalysisVisualizationModel visualizationFixture(
    const smp::json::Value& summary) {
    auto model =
        smp::buildGameAnalysisVisualizationModel(nullptr, &summary);
    model.navLoaded = true;
    model.navigationStatus = {true, {}};
    model.navigationEvents = {
        smp::TimelineNavigationEvent{1000.0},
        smp::TimelineNavigationEvent{31000.0},
    };
    return model;
}

const smp::ResultsSection* findSection(const smp::ResultsViewModel& model,
                                       const std::string& id) {
    for (const auto& section : model.sections) {
        if (section.id == id)
            return &section;
    }
    return nullptr;
}

bool hasMetric(const smp::ResultsSection& section, const std::string& label) {
    for (const auto& metric : section.metrics) {
        if (metric.label == label)
            return true;
    }
    return false;
}

const smp::ResultsMetric* findMetric(const smp::ResultsSection& section,
                                     const std::string& label) {
    for (const auto& metric : section.metrics) {
        if (metric.label == label)
            return &metric;
    }
    return nullptr;
}

const smp::ResultsMetric* findMetric(const smp::ResultsViewModel& model,
                                     const std::string& label) {
    for (const auto& section : model.sections) {
        if (const auto* metric = findMetric(section, label))
            return metric;
    }
    return nullptr;
}

void requireNavigationVisibility(const smp::ResultsViewModel& model,
                                 bool showRate,
                                 bool showMethods) {
    const auto* navigation = findSection(model, "camera_navigation");
    REQUIRE((navigation != nullptr) == (showRate || showMethods));
    if (!navigation)
        return;

    REQUIRE(hasMetric(*navigation, "Active time") == showRate);
    REQUIRE(hasMetric(*navigation, "Transitions / minute") == showRate);
    REQUIRE(hasMetric(*navigation, "Control-group jumps") == showMethods);
    REQUIRE(hasMetric(*navigation, "Location-hotkey jumps") == showMethods);
    REQUIRE(hasMetric(*navigation, "Minimap jumps") == showMethods);
    REQUIRE(hasMetric(*navigation, "Edge pans") == showMethods);
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

TEST_CASE("GUI instance claim distinguishes first and existing launches") {
    const std::wstring mutexName =
        L"Local\\StarcraftMechanicsProfiler.Gui.SingleInstance.Test." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());

    {
        auto first = smp::GuiInstanceClaim::acquire(mutexName);
        REQUIRE(first.ownsInstance());
        {
            auto second = smp::GuiInstanceClaim::acquire(mutexName);
            REQUIRE(!second.ownsInstance());
        }
    }

    auto afterExit = smp::GuiInstanceClaim::acquire(mutexName);
    REQUIRE(afterExit.ownsInstance());
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

TEST_CASE("GUI preferences default to every analysis display group and minimize to tray") {
    const auto defaults = smp::GuiPreferences::defaults();
    REQUIRE(defaults.reports.gameTimeline);
    REQUIRE(defaults.reports.cameraNavigation);
    REQUIRE(defaults.reports.workerMacroCycles);
    REQUIRE(defaults.reports.armyMacroCycles);
    REQUIRE(defaults.reports.macroGaps);
    REQUIRE(defaults.reports.macroDurationDistribution);
    REQUIRE(defaults.reports.macroAccessStyles);
    REQUIRE(defaults.reports.armyControlGroupManagement);
    REQUIRE(defaults.reports.armyCommandActivity);
    REQUIRE(defaults.reports.abilityActivity);
    REQUIRE(defaults.reports.navigationTransitionRate);
    REQUIRE(defaults.reports.multitaskingDensity);
    REQUIRE(defaults.reports.scoutingUnitActivity);
    REQUIRE(defaults.sessionReports.workerMacroDuration);
    REQUIRE(defaults.sessionReports.armyMacroDuration);
    REQUIRE(defaults.sessionReports.macroCadenceGaps);
    REQUIRE(defaults.sessionReports.armyControlGroupManagement);
    REQUIRE(defaults.sessionReports.armyCommandActivity);
    REQUIRE(defaults.sessionReports.abilityActivity);
    REQUIRE(defaults.sessionReports.navigationTransitionRate);
    REQUIRE(defaults.sessionReports.multitasking);
    REQUIRE(defaults.minimizeToTray);
    REQUIRE(!defaults.window.has_value());
}

TEST_CASE("GUI preferences round trip analysis display visibility and window placement") {
    const auto root = temporaryRoot("gui-preferences");
    std::filesystem::create_directories(root);
    const auto path = root / "gui-config.json";
    auto expected = smp::GuiPreferences::defaults();
    expected.reports.gameTimeline = false;
    expected.reports.cameraNavigation = false;
    expected.reports.macroGaps = false;
    expected.reports.armyCommandActivity = false;
    expected.reports.abilityActivity = false;
    expected.reports.multitaskingDensity = false;
    expected.reports.scoutingUnitActivity = false;
    expected.sessionReports.workerMacroDuration = false;
    expected.sessionReports.armyCommandActivity = false;
    expected.sessionReports.navigationTransitionRate = false;
    expected.minimizeToTray = false;
    expected.window = smp::GuiWindowPlacement{40, 50, 900, 700};
    expected.save(path);
    const auto encoded = smp::json::parseFile(path);
    REQUIRE(encoded["reports"]["worker_macro_cycles"].asBool(false));
    REQUIRE(!encoded["session_reports"]["worker_macro_duration"].asBool(true));
    REQUIRE(!encoded["reports"]["army_command_activity"].asBool(true));
    REQUIRE(!encoded["session_reports"]["army_command_activity"].asBool(true));
    REQUIRE(!encoded["reports"]["ability_activity"].asBool(true));
    REQUIRE(encoded["session_reports"]["ability_activity"].asBool(false));
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

TEST_CASE("schema one report visibility loads with newer display sections visible") {
    const auto root = temporaryRoot("gui-schema-one");
    std::filesystem::create_directories(root);
    const auto path = root / "gui-config.json";
    std::ofstream(path, std::ios::binary)
        << R"({"schema_version":1,"reports":{"worker_macro_cycles":false,"macro_access_styles":false}})";

    const auto loaded = smp::GuiPreferences::load(path);
    REQUIRE(!loaded.reports.workerMacroCycles);
    REQUIRE(!loaded.reports.macroAccessStyles);
    REQUIRE(loaded.reports.armyMacroCycles);
    REQUIRE(loaded.reports.gameTimeline);
    REQUIRE(loaded.reports.macroGaps);
    REQUIRE(loaded.reports.macroDurationDistribution);
    REQUIRE(loaded.reports.armyCommandActivity);
    REQUIRE(loaded.reports.abilityActivity);
    REQUIRE(loaded.reports.navigationTransitionRate);
    REQUIRE(loaded.reports.multitaskingDensity);
    REQUIRE(loaded.sessionReports == smp::SessionReportVisibility{});

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
    REQUIRE(loaded.reports.gameTimeline);
    REQUIRE(loaded.reports.macroGaps);
    REQUIRE(loaded.reports.macroDurationDistribution);
    REQUIRE(loaded.reports.armyCommandActivity);
    REQUIRE(loaded.reports.abilityActivity);
    REQUIRE(loaded.reports.navigationTransitionRate);
    REQUIRE(loaded.reports.multitaskingDensity);
    REQUIRE(!loaded.window.has_value());
    std::filesystem::remove_all(root);
}

TEST_CASE("analysis display category visibility suppresses empty headings") {
    smp::ReportGroupVisibility visibility;
    visibility.clearAll();

    REQUIRE(!visibility.hasMacroAnalysisSections());
    REQUIRE(!visibility.hasArmyManagementAnalysisSections());
    REQUIRE(!visibility.hasMultitaskingAnalysisSections());
    visibility.macroGaps = true;
    REQUIRE(visibility.hasMacroAnalysisSections());
    visibility.macroGaps = false;

    visibility.macroDurationDistribution = true;
    REQUIRE(visibility.hasMacroAnalysisSections());
    visibility.macroDurationDistribution = false;

    visibility.abilityActivity = true;
    REQUIRE(visibility.hasArmyManagementAnalysisSections());
    visibility.abilityActivity = false;

    visibility.multitaskingDensity = true;
    REQUIRE(visibility.hasMultitaskingAnalysisSections());
    visibility.multitaskingDensity = false;

    visibility.cameraNavigation = true;
    REQUIRE(visibility.hasMultitaskingAnalysisSections());

    visibility.selectAll();
    REQUIRE(visibility == smp::ReportGroupVisibility{});
}

TEST_CASE("session display visibility is independent from latest game visibility") {
    smp::ReportGroupVisibility latest;
    smp::SessionReportVisibility session;
    latest.clearAll();

    REQUIRE(session.hasMacroSections());
    REQUIRE(session.hasArmyManagementSections());
    REQUIRE(session.hasMultitaskingSections());
    REQUIRE(!latest.hasMacroAnalysisSections());
    REQUIRE(!latest.hasArmyManagementAnalysisSections());
    REQUIRE(!latest.hasMultitaskingAnalysisSections());

    session.clearAll();
    REQUIRE(!session.hasMacroSections());
    REQUIRE(!session.hasArmyManagementSections());
    REQUIRE(!session.hasMultitaskingSections());
    latest.selectAll();
    REQUIRE(latest == smp::ReportGroupVisibility{});
    REQUIRE(session != smp::SessionReportVisibility{});
}

TEST_CASE("session display visibility maps directly to shared trend metrics") {
    smp::SessionReportVisibility visibility;
    visibility.clearAll();
    visibility.workerMacroDuration = true;
    visibility.armyCommandActivity = true;

    REQUIRE(smp::trendMetricVisible(
        visibility, smp::TrendMetric::WorkerMacroDuration));
    REQUIRE(!smp::trendMetricVisible(
        visibility, smp::TrendMetric::ArmyMacroDuration));
    REQUIRE(smp::trendMetricVisible(
        visibility, smp::TrendMetric::ArmyCommandsRate));
    REQUIRE(smp::trendMetricVisible(
        visibility, smp::TrendMetric::MedianArmyCommandGap));
    REQUIRE(!smp::trendMetricVisible(
        visibility, smp::TrendMetric::AbilitiesRate));
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

TEST_CASE("latest game Results covers Analysis numerical helpers") {
    const auto summary = summaryFixture();
    const auto visualization = visualizationFixture(summary);
    const auto model = smp::deriveGameResults(
        summary, smp::ReportGroupVisibility{}, &visualization);

    const auto* workerGaps = findSection(model, "worker_macro_gaps");
    REQUIRE(workerGaps != nullptr);
    REQUIRE(findMetric(*workerGaps, "Cycles / min") != nullptr);
    REQUIRE(findMetric(*workerGaps, "Median gap") != nullptr);
    REQUIRE(findMetric(*workerGaps, "P90 gap") != nullptr);
    REQUIRE(findMetric(*workerGaps, "Longest gap") != nullptr);
    REQUIRE(findMetric(*workerGaps, "Gaps >10 s")->value == "2");
    REQUIRE(findMetric(*workerGaps, "Gaps >20 s")->value == "1");
    REQUIRE(model.hasSection("army_macro_gaps"));

    const auto* distribution =
        findSection(model, "worker_macro_duration_distribution");
    REQUIRE(distribution != nullptr);
    REQUIRE(distribution->metrics.size() == 5);
    for (const auto& metric : distribution->metrics)
        REQUIRE(metric.value == "1");

    const auto* armyCommands =
        findSection(model, "army_command_activity");
    REQUIRE(armyCommands != nullptr);
    REQUIRE(findMetric(*armyCommands, "Commands / min")->value == "4.0");
    REQUIRE(findMetric(*armyCommands, "Median command gap")->value ==
            "2.00 s");
    REQUIRE(findMetric(*armyCommands, "P90 command gap")->value ==
            "5.00 s");
    REQUIRE(findMetric(*armyCommands, "Longest command gap")->value ==
            "6.00 s");

    const auto* abilities = findSection(model, "ability_activity");
    REQUIRE(abilities != nullptr);
    REQUIRE(findMetric(*abilities, "Abilities / min")->value == "3.0");
    REQUIRE(findMetric(*abilities, "Total abilities")->value == "3");
    const auto* abilityBreakdown =
        findSection(model, "ability_activity_breakdown");
    REQUIRE(abilityBreakdown != nullptr);
    REQUIRE(findMetric(*abilityBreakdown, "Psionic Storm") != nullptr);

    const auto* multitasking = findSection(model, "multitasking");
    REQUIRE(multitasking != nullptr);
    const auto windows =
        smp::analysis_insights::multitaskingWindows(visualization);
    const auto* average = findMetric(
        *multitasking,
        "Average mechanic types / active 5-second window");
    const auto* peak = findMetric(
        *multitasking, "Peak mechanic types in one 5-second window");
    REQUIRE(average != nullptr);
    REQUIRE(peak != nullptr);
    REQUIRE(windows.averageActiveDiversity().has_value());
    REQUIRE_NEAR(std::stod(average->value),
                 *windows.averageActiveDiversity(), 0.01);
    REQUIRE(peak->value == std::to_string(windows.peakDiversity));
    REQUIRE(model.hasSection("command_heatmap_windows"));
    REQUIRE(model.hasSection("navigation_transition_buckets"));

    const auto* scouting = findSection(model, "scouting_activity");
    REQUIRE(scouting != nullptr);
    REQUIRE(findMetric(*scouting, "Confirmed scouts")->value == "1");
    REQUIRE(findMetric(*scouting, "Total scouting time") != nullptr);
    REQUIRE(findMetric(*scouting, "Longest scout command gap") != nullptr);
    REQUIRE(findMetric(*scouting, "Returned home")->value == "1");
    REQUIRE(findMetric(*scouting, "No observed return")->value == "0");
    REQUIRE(findMetric(*scouting, "Resumed after temporary return")->value ==
            "1");
}

TEST_CASE("latest game Results preserve zero ability and legacy scouting semantics") {
    auto summary = summaryFixture();
    summary["ability_activity"]["total_uses"] = 0;
    summary["ability_activity"]["abilities_per_minute"] = 0.0;
    summary["ability_activity"]["by_ability"] =
        smp::json::Value::Object{};
    summary["army_control_group_management"]
           ["scouting_outcome_data_available"] = false;
    auto visualization = visualizationFixture(summary);
    const auto model = smp::deriveGameResults(
        summary, smp::ReportGroupVisibility{}, &visualization);

    const auto* abilities = findSection(model, "ability_activity");
    REQUIRE(abilities != nullptr);
    REQUIRE(findMetric(*abilities, "Abilities / min")->value == "N/A");
    REQUIRE(findMetric(*abilities, "Total abilities")->value == "N/A");

    const auto* scouting = findSection(model, "scouting_activity");
    REQUIRE(scouting != nullptr);
    REQUIRE(findMetric(*scouting, "Returned home") == nullptr);
    REQUIRE(findMetric(*scouting, "No observed return") == nullptr);
    REQUIRE(findMetric(*scouting, "Resumed after temporary return") ==
            nullptr);
    REQUIRE(findMetric(*scouting, "Observed scouting outcomes")->value ==
            "N/A");

    summary["ability_activity"] = smp::json::Value::Object{
        {"available", false}, {"reason", "Replay analysis unavailable"}};
    visualization = visualizationFixture(summary);
    const auto unavailable = smp::deriveGameResults(
        summary, smp::ReportGroupVisibility{}, &visualization);
    const auto* unavailableAbilities =
        findSection(unavailable, "ability_activity");
    REQUIRE(unavailableAbilities != nullptr);
    REQUIRE(findMetric(*unavailableAbilities, "Status")->value.find(
                "Unavailable:") == 0);
}

TEST_CASE("clearing latest game visibility removes all numerical counterparts") {
    const auto summary = summaryFixture();
    const auto visualization = visualizationFixture(summary);
    smp::ReportGroupVisibility visibility;
    visibility.clearAll();
    const auto model =
        smp::deriveGameResults(summary, visibility, &visualization);
    REQUIRE(model.sections.empty());
}

TEST_CASE("game Results keeps navigation rate and methods independently visible") {
    const auto summary = summaryFixture();
    const auto verify = [&](bool showRate, bool showMethods) {
        auto visibility = smp::ReportGroupVisibility{};
        visibility.navigationTransitionRate = showRate;
        visibility.cameraNavigation = showMethods;
        requireNavigationVisibility(
            smp::deriveGameResults(summary, visibility), showRate,
            showMethods);
    };

    verify(true, true);
    verify(true, false);
    verify(false, true);
    verify(false, false);
    REQUIRE(summary["camera_navigation"]["total_transitions"].asInt() == 10);
}

TEST_CASE("current session Results uses only Session Trends visibility") {
    smp::AutomaticSessionStats stats;
    stats.games = 1;
    stats.activeSeconds = 60.0;
    stats.controlGroupJumps = 4;
    stats.locationHotkeyJumps = 3;
    stats.minimapJumps = 2;
    stats.edgePans = 1;

    smp::SessionReportVisibility visibility;
    visibility.clearAll();
    visibility.navigationTransitionRate = true;
    const auto model = smp::deriveSessionResults(stats, visibility);
    REQUIRE(model.sections.size() == 1);
    REQUIRE(model.hasSection("session_multitasking"));
    REQUIRE(!model.hasSection("camera_navigation"));
    const auto* section = findSection(model, "session_multitasking");
    REQUIRE(section != nullptr);
    REQUIRE(section->metrics.size() == 1);
    REQUIRE(section->metrics[0].label ==
            smp::metricTitle(smp::TrendMetric::NavigationRate));
}

TEST_CASE("results omit ambiguous method buckets and explain reported techniques") {
    auto summary = summaryFixture();
    summary["worker_macro_cycles"]["macro_access_styles"]["control_group_only"] =
        smp::json::Value::Object{{"cycle_count", 2}, {"percentage", 50.0},
                                 {"median_duration_ms", 1200.0}};
    summary["worker_macro_cycles"]["macro_access_styles"]["other"] =
        smp::json::Value::Object{{"cycle_count", 2}, {"percentage", 50.0},
                                 {"median_duration_ms", 1400.0}};
    summary["army_control_group_management"]["assignment_methods"]["box_select"] =
        smp::json::Value::Object{{"edit_count", 2}, {"percentage", 50.0},
                                 {"average_selection_to_operation_ms", 125.0}};
    summary["army_control_group_management"]["assignment_methods"]["existing_selection"] =
        smp::json::Value::Object{{"edit_count", 1}, {"percentage", 25.0}};
    summary["army_control_group_management"]["assignment_methods"]["other"] =
        smp::json::Value::Object{{"edit_count", 1}, {"percentage", 25.0}};

    const auto model = smp::deriveGameResults(summary, smp::ReportGroupVisibility{});
    const auto* access = findSection(model, "worker_access_styles");
    REQUIRE(access != nullptr);
    REQUIRE(access->metrics.size() == 1);
    REQUIRE(access->metrics[0].label == "Control Group Only");
    REQUIRE(access->metrics[0].value.find("100.0%") != std::string::npos);
    REQUIRE(!access->metrics[0].tooltip.empty());
    REQUIRE(access->metrics[0].label.find('_') == std::string::npos);

    const auto* assignments = findSection(model, "army_assignment_methods");
    REQUIRE(assignments != nullptr);
    REQUIRE(assignments->metrics.size() == 1);
    REQUIRE(assignments->metrics[0].label == "Box Select");
    REQUIRE(assignments->metrics[0].value.find("100.0%") != std::string::npos);
    REQUIRE(!assignments->metrics[0].tooltip.empty());
}

TEST_CASE("session results view model uses pooled existing statistics") {
    smp::AutomaticSessionStats stats;
    stats.games = 2;
    stats.activeSeconds = 120.0;
    stats.controlGroupJumps = 8;
    stats.locationHotkeyJumps = 4;
    stats.armyControlGroups.available = true;
    stats.armyControlGroups.activeDurationSeconds = 120.0;
    stats.armyControlGroupGamesAnalyzed = 2;
    stats.armyControlGroups.assignments = 4;
    stats.armyControlGroups.additions = 2;
    const auto model =
        smp::deriveSessionResults(stats, smp::SessionReportVisibility{});
    REQUIRE(model.hasSection("session_macro"));
    REQUIRE(model.hasSection("session_army_management"));
    REQUIRE(model.hasSection("session_multitasking"));
    REQUIRE(!model.hasSection("worker_access_styles"));
    REQUIRE(!model.hasSection("scouting_activity"));
    REQUIRE(model.subtitle.find("2 completed") != std::string::npos);
}

TEST_CASE("current session Results exposes the shared Session Trends metric set") {
    smp::AutomaticSessionStats stats;
    stats.games = 2;
    stats.activeSeconds = 120.0;
    stats.controlGroupJumps = 8;
    stats.locationHotkeyJumps = 4;
    stats.workerMacro.gamesAnalyzed = 2;
    stats.workerMacro.analyzedActiveSeconds = 120.0;
    stats.workerMacro.cycles = 6;
    stats.workerMacro.totalDurationMs = 6000.0;
    stats.workerMacro.gapDurationsMs = {5000.0, 15000.0, 25000.0};
    stats.armyMacro.gamesAnalyzed = 2;
    stats.armyMacro.analyzedActiveSeconds = 120.0;
    stats.armyMacro.cycles = 4;
    stats.armyMacro.totalDurationMs = 8000.0;
    stats.armyMacro.gapDurationsMs = {10000.0, 20000.0};
    stats.armyControlGroupGamesAnalyzed = 2;
    stats.armyControlGroups.available = true;
    stats.armyControlGroups.activeDurationSeconds = 120.0;
    stats.armyControlGroups.assignments = 4;
    stats.armyControlGroups.additions = 2;
    stats.armyCommands.gamesAnalyzed = 2;
    stats.armyCommands.analyzedActiveSeconds = 120.0;
    stats.armyCommands.commandCount = 20;
    stats.armyCommands.gapDurationsMs = {1000.0, 3000.0, 5000.0};
    stats.abilityActivity.gamesAnalyzed = 2;
    stats.abilityActivity.analyzedActiveSeconds = 120.0;
    stats.abilityActivity.totalUses = 8;
    stats.multitasking.gamesAnalyzed = 2;
    stats.multitasking.totalDiversityAcrossActiveWindows = 12;
    stats.multitasking.activeWindowCount = 6;
    stats.multitasking.peakDiversity = 4;

    const auto shared = smp::sessionTrendStats(stats);
    const auto model =
        smp::deriveSessionResults(stats, smp::SessionReportVisibility{});
    std::size_t metricCount = 0;
    for (const auto& section : model.sections)
        metricCount += section.metrics.size();
    REQUIRE(metricCount == 23);
    for (const auto metric : smp::trendMetrics) {
        REQUIRE(smp::metricValue(shared, metric).has_value());
        REQUIRE(findMetric(model, smp::metricTitle(metric)) != nullptr);
    }
    for (const auto metric : smp::workerArmyTrendMetrics) {
        REQUIRE(smp::workerArmyTrendValue(shared, true, metric).has_value());
        REQUIRE(smp::workerArmyTrendValue(shared, false, metric).has_value());
        REQUIRE(findMetric(
                    model,
                    smp::workerArmyTrendMetricTitle(true, metric)) != nullptr);
        REQUIRE(findMetric(
                    model,
                    smp::workerArmyTrendMetricTitle(false, metric)) != nullptr);
    }
    REQUIRE_NEAR(
        *smp::metricValue(shared, smp::TrendMetric::ArmyCommandsRate),
        10.0, 0.001);
    REQUIRE_NEAR(*smp::workerArmyTrendValue(
                     shared, true,
                     smp::WorkerArmyTrendMetric::GapsOver10SecondsPerGame),
                 1.0, 0.001);
}

TEST_CASE("session metric visibility filters Current Session independently") {
    smp::AutomaticSessionStats stats;
    stats.games = 1;
    stats.activeSeconds = 60.0;
    stats.workerMacro.gamesAnalyzed = 1;
    stats.workerMacro.analyzedActiveSeconds = 60.0;
    stats.workerMacro.cycles = 1;
    stats.workerMacro.totalDurationMs = 1000.0;

    smp::SessionReportVisibility visibility;
    visibility.clearAll();
    visibility.workerMacroDuration = true;
    const auto model = smp::deriveSessionResults(stats, visibility);
    REQUIRE(model.sections.size() == 1);
    REQUIRE(model.hasSection("session_macro"));
    REQUIRE(findMetric(model, "Average worker macro duration") != nullptr);
    REQUIRE(findMetric(model, "Average army macro duration") == nullptr);
    REQUIRE(findMetric(model, "Worker macro cycles / minute") == nullptr);
}
