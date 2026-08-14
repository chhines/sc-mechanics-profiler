#include "test_framework.h"

#include "app/application_controller.h"
#include "cli/automatic_session_files.h"
#include "cli/automatic_session_stats.h"
#include "cli/report.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::filesystem::path temporaryRoot(const char* label) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("starcraft-mechanics-profiler-") + label + "-" +
                       std::to_string(nonce));
    std::filesystem::create_directories(root);
    return root;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::size_t occurrences(const std::string& text, const std::string& value) {
    std::size_t count = 0;
    for (std::size_t position = 0;
         (position = text.find(value, position)) != std::string::npos;
         position += value.size()) {
        ++count;
    }
    return count;
}

smp::ProductionAnalysis availableProduction() {
    smp::ProductionAnalysis production;
    production.visitsAvailable = true;
    production.workerMacroCycles.available = true;
    production.workerMacroCycles.productType = smp::MacroProductType::Worker;
    production.workerMacroCycles.productionVisitCount = 2;
    production.workerMacroCycles.cycles.push_back(
        {smp::MacroProductType::Worker, 0.0, 700.0, 700.0, 0, 700, {0, 1}});
    production.workerMacroCycles.averageDurationMs = 700.0;
    production.workerMacroCycles.bestDurationMs = 700.0;
    production.workerMacroCycles.slowestDurationMs = 700.0;
    production.armyMacroCycles.available = true;
    production.armyMacroCycles.productType = smp::MacroProductType::Army;
    production.armyMacroCycles.productionVisitCount = 1;
    production.armyMacroCycles.cycles.push_back(
        {smp::MacroProductType::Army, 0.0, 900.0, 900.0, 0, 900, {0}});
    production.armyMacroCycles.averageDurationMs = 900.0;
    production.armyMacroCycles.bestDurationMs = 900.0;
    production.armyMacroCycles.slowestDurationMs = 900.0;
    production.armyControlGroupManagement.available = true;
    production.armyControlGroupManagement.assignments = 2;
    production.armyControlGroupManagement.additions = 1;
    production.armyControlGroupManagement.activeDurationSeconds = 60.0;
    smp::ScoutingUnitActivity scouting;
    scouting.group = 1;
    scouting.assignmentGeneration = 1;
    scouting.lastCommandActiveMs = 127'000.0;
    scouting.selectionCount = 3;
    scouting.commandCount = 3;
    scouting.scoutingActivityDurationMs = 87'000.0;
    production.armyControlGroupManagement.scoutingUnitActivities.push_back(scouting);
    return production;
}

} // namespace

TEST_CASE("automatic summary path is sortable and is not created for an empty invocation") {
    const auto root = temporaryRoot("automatic-summary-path");
    const auto sessions = root / "sessions";
    const auto start = std::chrono::system_clock::time_point(std::chrono::seconds(1'786'435'200));
    const auto first = smp::makeAutomaticSessionSummaryPath(sessions, start);
    REQUIRE(first.parent_path() == sessions);
    REQUIRE(first.filename().string().ends_with("_session.txt"));
    REQUIRE(!std::filesystem::exists(first));
    writeText(first, "completed");
    const auto second = smp::makeAutomaticSessionSummaryPath(sessions, start);
    REQUIRE(second != first);
    REQUIRE(second.filename().string().ends_with("_1_session.txt"));
    std::filesystem::remove_all(root);
}

TEST_CASE("automatic session summary is atomically replaced with the latest aggregate") {
    const auto root = temporaryRoot("automatic-summary-write");
    const auto summaryPath = root / "sessions" / "2026-08-11_120000_session.txt";
    smp::AnalysisResult first;
    first.activeDurationSeconds = 60.0;
    first.navigationEvents.push_back(
        {1000, 1000.0, smp::CameraNavigationType::ControlGroupJump, 1});
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, first, availableProduction()));
    smp::writeAutomaticSessionSummary(summaryPath,
                                      smp::formatAutomaticSessionReport(session));
    REQUIRE(std::filesystem::is_regular_file(summaryPath));
    REQUIRE(readText(summaryPath).find("Games") != std::string::npos);
    REQUIRE(readText(summaryPath).find("TOTAL NAVIGATION TRANSITIONS") != std::string::npos);
    REQUIRE(readText(summaryPath).find("WORKER MACRO") != std::string::npos);
    REQUIRE(readText(summaryPath).find("ARMY MACRO") != std::string::npos);

    smp::AnalysisResult second = first;
    REQUIRE(session.addFinalizedGame(2, second, availableProduction()));
    const auto updated = smp::formatAutomaticSessionReport(session);
    smp::writeAutomaticSessionSummary(summaryPath, updated);
    const auto persisted = readText(summaryPath);
    REQUIRE(persisted == updated);
    REQUIRE(occurrences(persisted, "SESSION SUMMARY") == 1);
    REQUIRE(persisted.find("LAST GAME") == std::string::npos);
    REQUIRE(!std::filesystem::exists(summaryPath.string() + ".tmp"));
    std::filesystem::remove_all(root);
}

TEST_CASE("automatic session report visibility filters presentation without changing data") {
    smp::AnalysisResult analysis;
    analysis.activeDurationSeconds = 60.0;
    analysis.navigationEvents.push_back(
        {1000, 1000.0, smp::CameraNavigationType::ControlGroupJump, 1});
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, analysis, availableProduction()));
    const auto navigationBefore = session.stats().navigationTransitions();
    const auto workerCyclesBefore = session.stats().workerMacro.cycles;
    const auto armyAssignmentsBefore = session.stats().armyControlGroups.assignments;
    const auto scoutingActivitiesBefore =
        session.stats().armyControlGroups.scoutingUnitActivities.size();

    const auto complete = smp::formatAutomaticSessionReport(session);
    REQUIRE(complete == smp::formatAutomaticSessionReport(
                            session, smp::ReportGroupVisibility{}));
    REQUIRE(complete.find("TOTAL NAVIGATION TRANSITIONS") != std::string::npos);
    REQUIRE(complete.find("MACRO ACCESS STYLES") != std::string::npos);
    REQUIRE(complete.find("ARMY CONTROL-GROUP MANAGEMENT") != std::string::npos);
    REQUIRE(complete.find("SCOUTING UNIT ACTIVITY") != std::string::npos);

    auto visibility = smp::ReportGroupVisibility{};
    visibility.cameraNavigation = false;
    const auto withoutNavigation =
        smp::formatAutomaticSessionReport(session, visibility);
    REQUIRE(withoutNavigation.find("TOTAL NAVIGATION TRANSITIONS") == std::string::npos);
    REQUIRE(withoutNavigation.find("SESSION RATE") == std::string::npos);

    visibility = {};
    visibility.macroAccessStyles = false;
    const auto withoutStyles = smp::formatAutomaticSessionReport(session, visibility);
    REQUIRE(withoutStyles.find("WORKER MACRO") != std::string::npos);
    REQUIRE(withoutStyles.find("ARMY MACRO") != std::string::npos);
    REQUIRE(withoutStyles.find("MACRO ACCESS STYLES") == std::string::npos);

    visibility = {};
    visibility.workerMacroCycles = false;
    visibility.armyMacroCycles = false;
    const auto stylesWithoutMacroTotals =
        smp::formatAutomaticSessionReport(session, visibility);
    REQUIRE(stylesWithoutMacroTotals.find("\nWORKER MACRO\n\n") == std::string::npos);
    REQUIRE(stylesWithoutMacroTotals.find("\nARMY MACRO\n\n") == std::string::npos);
    REQUIRE(stylesWithoutMacroTotals.find("WORKER MACRO ACCESS STYLES") !=
            std::string::npos);
    REQUIRE(stylesWithoutMacroTotals.find("ARMY MACRO ACCESS STYLES") !=
            std::string::npos);

    visibility = {};
    visibility.scoutingUnitActivity = false;
    const auto withoutScouting = smp::formatAutomaticSessionReport(session, visibility);
    REQUIRE(withoutScouting.find("ARMY CONTROL-GROUP MANAGEMENT") != std::string::npos);
    REQUIRE(withoutScouting.find("SCOUTING UNIT ACTIVITY") == std::string::npos);

    visibility = {};
    visibility.armyControlGroupManagement = false;
    const auto scoutingWithoutArmyManagement =
        smp::formatAutomaticSessionReport(session, visibility);
    REQUIRE(scoutingWithoutArmyManagement.find("ARMY CONTROL-GROUP MANAGEMENT") ==
            std::string::npos);
    REQUIRE(scoutingWithoutArmyManagement.find("SCOUTING UNIT ACTIVITY") !=
            std::string::npos);

    REQUIRE(session.stats().navigationTransitions() == navigationBefore);
    REQUIRE(session.stats().workerMacro.cycles == workerCyclesBefore);
    REQUIRE(session.stats().armyControlGroups.assignments == armyAssignmentsBefore);
    REQUIRE(session.stats().armyControlGroups.scoutingUnitActivities.size() ==
            scoutingActivitiesBefore);
}

TEST_CASE("running GUI automatic reports resolve current visibility for every write") {
    const auto root = temporaryRoot("automatic-live-report-visibility");
    const auto summaryPath = root / "sessions" / "2026-08-13_120000_session.txt";
    smp::AnalysisResult analysis;
    analysis.activeDurationSeconds = 60.0;
    analysis.navigationEvents.push_back(
        {1000, 1000.0, smp::CameraNavigationType::ControlGroupJump, 1});
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, analysis, availableProduction()));
    const auto navigationBefore = session.stats().navigationTransitions();
    const auto workerCyclesBefore = session.stats().workerMacro.cycles;
    const auto scoutingBefore =
        session.stats().armyControlGroups.scoutingUnitActivities.size();

    smp::ApplicationController controller(root);
    auto initial = smp::ReportGroupVisibility{};
    controller.setReportVisibility(initial);
    std::atomic<int> providerCalls{};
    const smp::ReportVisibilityProvider provider = [&]() {
        providerCalls.fetch_add(1, std::memory_order_relaxed);
        return controller.reportVisibility();
    };

    smp::writeAutomaticSessionSummary(summaryPath, session, provider);
    auto persisted = readText(summaryPath);
    REQUIRE(persisted.find("TOTAL NAVIGATION TRANSITIONS") != std::string::npos);
    REQUIRE(persisted.find("SCOUTING UNIT ACTIVITY") != std::string::npos);
    REQUIRE(persisted.find("WORKER MACRO") != std::string::npos);

    auto updated = initial;
    updated.cameraNavigation = false;
    updated.scoutingUnitActivity = false;
    controller.setReportVisibility(updated);
    smp::writeAutomaticSessionSummary(summaryPath, session, provider);
    persisted = readText(summaryPath);
    REQUIRE(persisted.find("TOTAL NAVIGATION TRANSITIONS") == std::string::npos);
    REQUIRE(persisted.find("SCOUTING UNIT ACTIVITY") == std::string::npos);
    REQUIRE(persisted.find("WORKER MACRO") != std::string::npos);

    controller.setReportVisibility(initial);
    smp::writeAutomaticSessionSummary(summaryPath, session, provider);
    persisted = readText(summaryPath);
    REQUIRE(persisted.find("TOTAL NAVIGATION TRANSITIONS") != std::string::npos);
    REQUIRE(persisted.find("SCOUTING UNIT ACTIVITY") != std::string::npos);
    REQUIRE(providerCalls.load(std::memory_order_relaxed) == 3);

    REQUIRE(session.stats().navigationTransitions() == navigationBefore);
    REQUIRE(session.stats().workerMacro.cycles == workerCyclesBefore);
    REQUIRE(session.stats().armyControlGroups.scoutingUnitActivities.size() ==
            scoutingBefore);
    std::filesystem::remove_all(root);
}

TEST_CASE("controller report visibility is copied and safe across GUI and worker threads") {
    const auto root = temporaryRoot("automatic-report-visibility-threading");
    smp::ApplicationController controller(root);
    const auto enabled = smp::ReportGroupVisibility{};
    auto disabled = enabled;
    disabled.cameraNavigation = false;
    disabled.workerMacroCycles = false;
    disabled.armyMacroCycles = false;
    disabled.macroAccessStyles = false;
    disabled.armyControlGroupManagement = false;
    disabled.scoutingUnitActivity = false;
    controller.setReportVisibility(enabled);

    std::atomic<bool> invalidCopy{};
    std::thread reader([&]() {
        for (int iteration = 0; iteration < 20'000; ++iteration) {
            const auto copy = controller.reportVisibility();
            if (copy != enabled && copy != disabled)
                invalidCopy.store(true, std::memory_order_relaxed);
        }
    });
    for (int iteration = 0; iteration < 20'000; ++iteration)
        controller.setReportVisibility((iteration % 2) == 0 ? disabled : enabled);
    reader.join();
    REQUIRE(!invalidCopy.load(std::memory_order_relaxed));

    controller.setReportVisibility(enabled);
    auto independentCopy = controller.reportVisibility();
    independentCopy.cameraNavigation = false;
    REQUIRE(controller.reportVisibility().cameraNavigation);
    std::filesystem::remove_all(root);
}

TEST_CASE("CLI automatic report writing defaults to every report group") {
    const auto root = temporaryRoot("automatic-cli-report-defaults");
    const auto summaryPath = root / "sessions" / "2026-08-13_130000_session.txt";
    smp::AnalysisResult analysis;
    analysis.activeDurationSeconds = 60.0;
    analysis.navigationEvents.push_back(
        {1000, 1000.0, smp::CameraNavigationType::ControlGroupJump, 1});
    smp::AutomaticSessionState session;
    REQUIRE(session.addFinalizedGame(1, analysis, availableProduction()));

    smp::writeAutomaticSessionSummary(summaryPath, session);
    const auto persisted = readText(summaryPath);
    REQUIRE(persisted.find("TOTAL NAVIGATION TRANSITIONS") != std::string::npos);
    REQUIRE(persisted.find("WORKER MACRO") != std::string::npos);
    REQUIRE(persisted.find("SCOUTING UNIT ACTIVITY") != std::string::npos);
    std::filesystem::remove_all(root);
}

TEST_CASE("latest automatic summary ignores nav files and searches date directories globally") {
    const auto root = temporaryRoot("automatic-summary-latest");
    const auto sessions = root / "sessions";
    const auto older = sessions / "2026-08-11" / "2026-08-11_120000_session.txt";
    const auto newer = sessions / "2026-08-11" / "2026-08-11_123000_session.txt";
    const auto newest = sessions / "2026-08-12" / "2026-08-12_010000_session.txt";
    writeText(older, "older");
    writeText(newer, "newer");
    writeText(sessions / "2026-08-11" / "2026-08-11_124500.nav", "not a summary");
    auto resolved = smp::findLatestAutomaticSessionSummary(sessions);
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == newer);
    writeText(newest, "newest");
    resolved = smp::findLatestAutomaticSessionSummary(sessions);
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == newest);
    std::filesystem::remove_all(root);
}

TEST_CASE("an aborted newer automatic invocation does not hide an older real summary") {
    const auto root = temporaryRoot("automatic-summary-abort");
    const auto sessions = root / "sessions";
    const auto completed = sessions / "2026-08-11_120000_session.txt";
    writeText(completed, "completed session");
    const auto aborted = sessions / "2026-08-11_124500_session.txt";
    REQUIRE(!std::filesystem::exists(aborted));
    const auto resolved = smp::findLatestAutomaticSessionSummary(sessions);
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == completed);
    std::filesystem::remove_all(root);
}

TEST_CASE("aborted automatic artifacts are removed without touching completed game files") {
    const auto root = temporaryRoot("automatic-abort-cleanup");
    const auto partialNav = root / "partial.nav";
    const auto partialJson = root / "partial.json";
    const auto partialRaw = root / "partial.events.bin";
    const auto partialTemporary = root / "partial.nav.tmp";
    const auto completedNav = root / "completed.nav";
    const auto completedJson = root / "completed.json";
    writeText(partialNav, "partial nav");
    writeText(partialJson, "partial json");
    writeText(partialRaw, "partial raw");
    writeText(partialTemporary, "partial temporary");
    writeText(completedNav, "completed nav");
    writeText(completedJson, "completed json");

    const auto discarded = smp::discardAbortedAutomaticRecordingFiles(
        {partialNav, partialJson, partialRaw});
    REQUIRE(discarded.failedPaths.empty());
    REQUIRE(discarded.removedFiles == 4);
    REQUIRE(!std::filesystem::exists(partialNav));
    REQUIRE(!std::filesystem::exists(partialJson));
    REQUIRE(!std::filesystem::exists(partialRaw));
    REQUIRE(!std::filesystem::exists(partialTemporary));
    REQUIRE(std::filesystem::exists(completedNav));
    REQUIRE(std::filesystem::exists(completedJson));
    std::filesystem::remove_all(root);
}

TEST_CASE("aborted automatic cleanup failure is reported without throwing") {
    const auto root = temporaryRoot("automatic-abort-cleanup-failure");
    const auto directoryInsteadOfFile = root / "partial.nav";
    std::filesystem::create_directories(directoryInsteadOfFile / "child");
    const auto discarded = smp::discardAbortedAutomaticRecordingFiles(
        {directoryInsteadOfFile, {}, {}});
    REQUIRE(discarded.failedPaths.size() == 1);
    REQUIRE(std::filesystem::is_directory(directoryInsteadOfFile));
    std::filesystem::remove_all(root);
}
