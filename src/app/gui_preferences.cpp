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

void ReportGroupVisibility::selectAll() noexcept {
    *this = {};
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
            preferences.reports.cameraNavigation =
                readBool(reports, "camera_navigation", true);
            preferences.reports.workerMacroCycles =
                readBool(reports, "worker_macro_cycles", true);
            preferences.reports.armyMacroCycles =
                readBool(reports, "army_macro_cycles", true);
            preferences.reports.macroAccessStyles =
                readBool(reports, "macro_access_styles", true);
            preferences.reports.armyControlGroupManagement =
                readBool(reports, "army_control_group_management", true);
            preferences.reports.scoutingUnitActivity =
                readBool(reports, "scouting_unit_activity", true);
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
    root["schema_version"] = 1;
    root["reports"] = json::Value::Object{
        {"camera_navigation", reports.cameraNavigation},
        {"worker_macro_cycles", reports.workerMacroCycles},
        {"army_macro_cycles", reports.armyMacroCycles},
        {"macro_access_styles", reports.macroAccessStyles},
        {"army_control_group_management", reports.armyControlGroupManagement},
        {"scouting_unit_activity", reports.scoutingUnitActivity},
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
