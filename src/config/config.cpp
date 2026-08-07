#include "config/config.h"

#include "util/json.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <windows.h>

namespace scm {
namespace {

std::wstring widen(const std::string& text) {
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string narrow(const std::wstring& text) {
    if (text.empty())
        return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr,
                        nullptr);
    return result;
}

Rect readRect(const json::Value& value) {
    return {value["left"].asInt(), value["top"].asInt(), value["right"].asInt(), value["bottom"].asInt()};
}

json::Value rectJson(const Rect& rect) {
    return json::Value::Object{{"left", rect.left}, {"top", rect.top}, {"right", rect.right}, {"bottom", rect.bottom}};
}

std::vector<ProductionRule> readRules(const json::Value& value) {
    std::vector<ProductionRule> rules;
    for (const auto& item : value.asArray()) {
        ProductionRule rule;
        rule.group = item["group"].asInt(-1);
        for (const auto& key : item["train_keys"].asArray()) {
            const auto vk = keyNameToVirtualKey(key.asString());
            if (vk != 0)
                rule.trainKeys.push_back(vk);
        }
        if (rule.group >= 0 && rule.group <= 9 && !rule.trainKeys.empty())
            rules.push_back(std::move(rule));
    }
    return rules;
}

json::Value rulesJson(const std::vector<ProductionRule>& rules) {
    json::Value::Array result;
    for (const auto& rule : rules) {
        json::Value::Array keys;
        for (const auto key : rule.trainKeys)
            keys.emplace_back(virtualKeyToName(key));
        result.emplace_back(json::Value::Object{{"group", rule.group}, {"train_keys", std::move(keys)}});
    }
    return result;
}

int positiveOr(int value, int fallback) {
    return value > 0 ? value : fallback;
}

} // namespace

std::uint16_t keyNameToVirtualKey(const std::string& value) {
    if (value.empty())
        return 0;
    std::string upper = value;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (upper.size() == 1 && ((upper[0] >= 'A' && upper[0] <= 'Z') || (upper[0] >= '0' && upper[0] <= '9'))) {
        return static_cast<std::uint16_t>(upper[0]);
    }
    if (upper[0] == 'F' && upper.size() <= 3) {
        try {
            const int number = std::stoi(upper.substr(1));
            if (number >= 1 && number <= 24)
                return static_cast<std::uint16_t>(VK_F1 + number - 1);
        } catch (...) {
        }
    }
    if (upper == "CTRL" || upper == "CONTROL")
        return VK_CONTROL;
    if (upper == "SHIFT")
        return VK_SHIFT;
    if (upper == "ALT")
        return VK_MENU;
    if (upper == "SPACE")
        return VK_SPACE;
    return 0;
}

std::string virtualKeyToName(std::uint16_t key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
        return std::string(1, static_cast<char>(key));
    if (key >= VK_F1 && key <= VK_F24)
        return "F" + std::to_string(key - VK_F1 + 1);
    if (key == VK_CONTROL)
        return "CTRL";
    if (key == VK_SHIFT)
        return "SHIFT";
    if (key == VK_MENU)
        return "ALT";
    if (key == VK_SPACE)
        return "SPACE";
    return "VK_" + std::to_string(key);
}

Config Config::loadOrCreate(const std::filesystem::path& path) {
    Config config;
    if (!std::filesystem::exists(path)) {
        config.save(path);
        return config;
    }

    const auto root = json::parseFile(path);
    const auto process = root["starcraft_process"].asString();
    if (!process.empty())
        config.starcraftProcess = widen(process);
    config.controlGroupDoubleTapMs = positiveOr(
        root["control_groups"]["double_tap_ms"].asInt(config.controlGroupDoubleTapMs), config.controlGroupDoubleTapMs);

    if (!root["location_hotkeys"]["recall"].asArray().empty()) {
        config.locationHotkeys.clear();
        for (const auto& key : root["location_hotkeys"]["recall"].asArray()) {
            if (const auto vk = keyNameToVirtualKey(key.asString()); vk != 0)
                config.locationHotkeys.push_back(vk);
        }
    }

    const auto setKey = [&](const char* name, std::uint16_t& target) {
        if (const auto key = keyNameToVirtualKey(root["commands"][name].asString()); key != 0)
            target = key;
    };
    setKey("attack", config.attackKey);
    setKey("move", config.moveKey);
    setKey("patrol", config.patrolKey);
    setKey("stop", config.stopKey);
    setKey("hold", config.holdKey);

    config.viewport = readRect(root["screen"]["viewport"]);
    config.minimap = readRect(root["screen"]["minimap"]);
    config.commandCard = readRect(root["screen"]["command_card"]);
    config.edgeThicknessPx =
        positiveOr(root["screen"]["edge_thickness_px"].asInt(config.edgeThicknessPx), config.edgeThicknessPx);
    config.edgeDwellMs = positiveOr(root["screen"]["edge_dwell_ms"].asInt(config.edgeDwellMs), config.edgeDwellMs);
    config.dragThresholdPx =
        positiveOr(root["box_selection"]["drag_threshold_px"].asInt(config.dragThresholdPx), config.dragThresholdPx);
    config.reselectionIntervalMs =
        positiveOr(root["box_selection"]["reselection_interval_ms"].asInt(config.reselectionIntervalMs),
                   config.reselectionIntervalMs);
    config.reselectionIou = root["box_selection"]["reselection_iou"].asNumber(config.reselectionIou);
    if (config.reselectionIou < 0.0 || config.reselectionIou > 1.0)
        config.reselectionIou = 0.5;

    config.macroRecognitionIntervalMs =
        positiveOr(root["macro"]["recognition_interval_ms"].asInt(config.macroRecognitionIntervalMs),
                   config.macroRecognitionIntervalMs);
    config.macroEpisodeGapMs =
        positiveOr(root["macro"]["episode_gap_ms"].asInt(config.macroEpisodeGapMs), config.macroEpisodeGapMs);
    config.workerRules = readRules(root["macro"]["worker"]);
    config.armyRules = readRules(root["macro"]["army"]);
    config.microWindowMs =
        positiveOr(root["micro_burst"]["window_ms"].asInt(config.microWindowMs), config.microWindowMs);
    config.microMinimumEvents =
        positiveOr(root["micro_burst"]["minimum_events"].asInt(config.microMinimumEvents), config.microMinimumEvents);
    config.microEndQuietMs =
        positiveOr(root["micro_burst"]["end_quiet_ms"].asInt(config.microEndQuietMs), config.microEndQuietMs);
    config.loadWindowSeconds =
        positiveOr(root["load"]["window_seconds"].asInt(config.loadWindowSeconds), config.loadWindowSeconds);
    config.loadMinimumObservations = positiveOr(
        root["load"]["minimum_observations"].asInt(config.loadMinimumObservations), config.loadMinimumObservations);
    config.writeLogicalEvents = root["storage"]["write_logical_events"].asBool(config.writeLogicalEvents);
    config.flushIntervalMs =
        positiveOr(root["storage"]["flush_interval_ms"].asInt(config.flushIntervalMs), config.flushIntervalMs);
    return config;
}

void Config::save(const std::filesystem::path& path) const {
    json::Value::Array locations;
    for (const auto key : locationHotkeys)
        locations.emplace_back(virtualKeyToName(key));
    json::Value::Array groupKeys;
    for (int group = 0; group <= 9; ++group)
        groupKeys.emplace_back(std::to_string(group));

    json::Value root(json::Value::Object{});
    root["starcraft_process"] = narrow(starcraftProcess);
    root["control_groups"] = json::Value::Object{
        {"keys", std::move(groupKeys)}, {"assign_modifier", "CTRL"}, {"double_tap_ms", controlGroupDoubleTapMs}};
    root["location_hotkeys"] = json::Value::Object{{"recall", std::move(locations)}, {"assign_modifier", "SHIFT"}};
    root["commands"] = json::Value::Object{{"attack", virtualKeyToName(attackKey)},
                                           {"move", virtualKeyToName(moveKey)},
                                           {"patrol", virtualKeyToName(patrolKey)},
                                           {"stop", virtualKeyToName(stopKey)},
                                           {"hold", virtualKeyToName(holdKey)}};
    root["screen"] = json::Value::Object{{"viewport", rectJson(viewport)},
                                         {"minimap", rectJson(minimap)},
                                         {"command_card", rectJson(commandCard)},
                                         {"edge_thickness_px", edgeThicknessPx},
                                         {"edge_dwell_ms", edgeDwellMs}};
    root["box_selection"] = json::Value::Object{{"drag_threshold_px", dragThresholdPx},
                                                {"reselection_interval_ms", reselectionIntervalMs},
                                                {"reselection_iou", reselectionIou}};
    root["macro"] = json::Value::Object{{"recognition_interval_ms", macroRecognitionIntervalMs},
                                        {"episode_gap_ms", macroEpisodeGapMs},
                                        {"worker", rulesJson(workerRules)},
                                        {"army", rulesJson(armyRules)}};
    root["micro_burst"] = json::Value::Object{
        {"window_ms", microWindowMs}, {"minimum_events", microMinimumEvents}, {"end_quiet_ms", microEndQuietMs}};
    root["load"] =
        json::Value::Object{{"window_seconds", loadWindowSeconds}, {"minimum_observations", loadMinimumObservations}};
    root["storage"] =
        json::Value::Object{{"write_logical_events", writeLogicalEvents}, {"flush_interval_ms", flushIntervalMs}};
    json::writeFile(path, root);
}

} // namespace scm
