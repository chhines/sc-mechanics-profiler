#include "app/gui_preferences.h"

#include "util/json.h"

#include <filesystem>

namespace smp {
namespace {

bool readBool(const json::Value& object, const char* key, bool fallback) {
    const auto& value = object[key];
    return value.isBool() ? value.asBool() : fallback;
}

} // namespace

MainWindowAction minimizeAction(bool minimizeToTray) noexcept {
    return minimizeToTray ? MainWindowAction::HideToTray
                          : MainWindowAction::MinimizeNormally;
}

MainWindowAction closeAction(bool minimizeToTray) noexcept {
    return minimizeToTray ? MainWindowAction::HideToTray : MainWindowAction::Exit;
}

void ReportGroupVisibility::selectAll() noexcept {
    *this = {};
}

void ReportGroupVisibility::clearAll() noexcept {
    gameTimeline = false;
    cameraNavigation = false;
    workerMacroCycles = false;
    armyMacroCycles = false;
    macroGaps = false;
    macroDurationDistribution = false;
    macroAccessStyles = false;
    armyControlGroupManagement = false;
    armyCommandActivity = false;
    abilityActivity = false;
    navigationTransitionRate = false;
    multitaskingDensity = false;
    scoutingUnitActivity = false;
}

bool ReportGroupVisibility::hasMacroAnalysisSections() const noexcept {
    return macroGaps || macroDurationDistribution || macroAccessStyles;
}

bool ReportGroupVisibility::hasArmyManagementAnalysisSections() const noexcept {
    return armyControlGroupManagement || armyCommandActivity || abilityActivity;
}

bool ReportGroupVisibility::hasMultitaskingAnalysisSections() const noexcept {
    return navigationTransitionRate || multitaskingDensity ||
           scoutingUnitActivity || cameraNavigation;
}

void SessionReportVisibility::selectAll() noexcept {
    *this = {};
}

void SessionReportVisibility::clearAll() noexcept {
    workerMacroDuration = false;
    armyMacroDuration = false;
    macroCadenceGaps = false;
    armyControlGroupManagement = false;
    armyCommandActivity = false;
    abilityActivity = false;
    navigationTransitionRate = false;
    multitasking = false;
}

bool SessionReportVisibility::hasMacroSections() const noexcept {
    return workerMacroDuration || armyMacroDuration || macroCadenceGaps;
}

bool SessionReportVisibility::hasArmyManagementSections() const noexcept {
    return armyControlGroupManagement || armyCommandActivity ||
           abilityActivity;
}

bool SessionReportVisibility::hasMultitaskingSections() const noexcept {
    return navigationTransitionRate || multitasking;
}

bool GuiWindowPlacement::valid() const noexcept {
    return width >= 520 && width <= 2400 && height >= 420 && height <= 1800 &&
           x >= -10000 && x <= 10000 && y >= -10000 && y <= 10000;
}

GuiPreferences GuiPreferences::defaults() noexcept {
    return {};
}

GuiPreferences GuiPreferences::load(const std::filesystem::path& path) noexcept {
    GuiPreferences preferences = defaults();
    try {
        if (!std::filesystem::is_regular_file(path))
            return preferences;
        const auto root = json::parseFile(path);
        if (!root.isObject())
            return preferences;

        const auto& reports = root["reports"];
        if (reports.isObject()) {
            preferences.reports.gameTimeline =
                readBool(reports, "game_timeline", true);
            preferences.reports.cameraNavigation =
                readBool(reports, "camera_navigation", true);
            preferences.reports.workerMacroCycles =
                readBool(reports, "worker_macro_cycles", true);
            preferences.reports.armyMacroCycles =
                readBool(reports, "army_macro_cycles", true);
            preferences.reports.macroGaps =
                readBool(reports, "macro_gaps", true);
            preferences.reports.macroDurationDistribution =
                readBool(reports, "macro_duration_distribution", true);
            preferences.reports.macroAccessStyles =
                readBool(reports, "macro_access_styles", true);
            preferences.reports.armyControlGroupManagement =
                readBool(reports, "army_control_group_management", true);
            preferences.reports.armyCommandActivity =
                readBool(reports, "army_command_activity", true);
            preferences.reports.abilityActivity =
                readBool(reports, "ability_activity", true);
            preferences.reports.navigationTransitionRate =
                readBool(reports, "navigation_transition_rate", true);
            preferences.reports.multitaskingDensity =
                readBool(reports, "multitasking_density", true);
            preferences.reports.scoutingUnitActivity =
                readBool(reports, "scouting_unit_activity", true);
        }
        const auto& sessionReports = root["session_reports"];
        if (sessionReports.isObject()) {
            preferences.sessionReports.workerMacroDuration =
                readBool(sessionReports, "worker_macro_duration", true);
            preferences.sessionReports.armyMacroDuration =
                readBool(sessionReports, "army_macro_duration", true);
            preferences.sessionReports.macroCadenceGaps =
                readBool(sessionReports, "macro_cadence_gaps", true);
            preferences.sessionReports.armyControlGroupManagement = readBool(
                sessionReports, "army_control_group_management", true);
            preferences.sessionReports.armyCommandActivity =
                readBool(sessionReports, "army_command_activity", true);
            preferences.sessionReports.abilityActivity =
                readBool(sessionReports, "ability_activity", true);
            preferences.sessionReports.navigationTransitionRate = readBool(
                sessionReports, "navigation_transition_rate", true);
            preferences.sessionReports.multitasking =
                readBool(sessionReports, "multitasking", true);
        }
        preferences.minimizeToTray =
            readBool(root, "minimize_to_tray", preferences.minimizeToTray);

        const auto& window = root["window"];
        if (window.isObject()) {
            GuiWindowPlacement placement{window["x"].asInt(), window["y"].asInt(),
                                         window["width"].asInt(),
                                         window["height"].asInt()};
            if (placement.valid())
                preferences.window = placement;
        }
    } catch (...) {
        return defaults();
    }
    return preferences;
}

void GuiPreferences::save(const std::filesystem::path& path) const {
    json::Value root(json::Value::Object{});
    root["schema_version"] = 3;
    root["reports"] = json::Value::Object{
        {"game_timeline", reports.gameTimeline},
        {"camera_navigation", reports.cameraNavigation},
        {"worker_macro_cycles", reports.workerMacroCycles},
        {"army_macro_cycles", reports.armyMacroCycles},
        {"macro_gaps", reports.macroGaps},
        {"macro_duration_distribution", reports.macroDurationDistribution},
        {"macro_access_styles", reports.macroAccessStyles},
        {"army_control_group_management", reports.armyControlGroupManagement},
        {"army_command_activity", reports.armyCommandActivity},
        {"ability_activity", reports.abilityActivity},
        {"navigation_transition_rate", reports.navigationTransitionRate},
        {"multitasking_density", reports.multitaskingDensity},
        {"scouting_unit_activity", reports.scoutingUnitActivity},
    };
    root["session_reports"] = json::Value::Object{
        {"worker_macro_duration", sessionReports.workerMacroDuration},
        {"army_macro_duration", sessionReports.armyMacroDuration},
        {"macro_cadence_gaps", sessionReports.macroCadenceGaps},
        {"army_control_group_management",
         sessionReports.armyControlGroupManagement},
        {"army_command_activity", sessionReports.armyCommandActivity},
        {"ability_activity", sessionReports.abilityActivity},
        {"navigation_transition_rate",
         sessionReports.navigationTransitionRate},
        {"multitasking", sessionReports.multitasking},
    };
    root["minimize_to_tray"] = minimizeToTray;
    root["window"] = window && window->valid()
                         ? json::Value(json::Value::Object{{"x", window->x},
                                                          {"y", window->y},
                                                          {"width", window->width},
                                                          {"height", window->height}})
                         : json::Value(nullptr);
    json::writeFile(path, root);
}

} // namespace smp
