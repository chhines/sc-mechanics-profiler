#include "config/config.h"

#include "util/json.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <windows.h>

namespace smp {
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

ScreenRect readRect(const json::Value& value) {
    if (!value.isObject())
        return {};
    return {value["left"].asInt(), value["top"].asInt(), value["right"].asInt(-1),
            value["bottom"].asInt(-1)};
}

json::Value rectJson(const ScreenRect& rect) {
    return json::Value::Object{{"left", rect.left}, {"top", rect.top}, {"right", rect.right},
                               {"bottom", rect.bottom}};
}

std::optional<NormalizedScreenRect> readNormalizedRect(const json::Value& value) {
    if (!value.isObject())
        return std::nullopt;
    NormalizedScreenRect rect{value["left_norm"].asNumber(), value["top_norm"].asNumber(),
                              value["right_norm"].asNumber(), value["bottom_norm"].asNumber()};
    return rect.valid() ? std::optional<NormalizedScreenRect>(rect) : std::nullopt;
}

json::Value normalizedRectJson(const NormalizedScreenRect& rect) {
    return json::Value::Object{{"left_norm", rect.left},
                               {"top_norm", rect.top},
                               {"right_norm", rect.right},
                               {"bottom_norm", rect.bottom}};
}

int positiveOr(int value, int fallback) {
    return value > 0 ? value : fallback;
}

MinimapMode readMinimapMode(const json::Value& value) noexcept {
    return value.asString() == "calibrated_override"
               ? MinimapMode::CalibratedOverride
               : MinimapMode::Automatic;
}

} // namespace

const char* minimapModeName(MinimapMode mode) noexcept {
    return mode == MinimapMode::CalibratedOverride ? "calibrated_override"
                                                    : "automatic";
}

std::uint16_t keyNameToVirtualKey(const std::string& value) {
    if (value.empty())
        return 0;
    std::string upper = value;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (upper.size() == 1 && ((upper[0] >= 'A' && upper[0] <= 'Z') || (upper[0] >= '0' && upper[0] <= '9')))
        return static_cast<std::uint16_t>(upper[0]);
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
            if (const auto virtualKey = keyNameToVirtualKey(key.asString()); virtualKey != 0)
                config.locationHotkeys.push_back(virtualKey);
        }
    }

    config.autoScreenRegions = root["screen"]["auto_detect"].asBool(config.autoScreenRegions);
    config.gameArea = readRect(root["screen"]["game_area"]);
    config.viewport = readRect(root["screen"]["viewport"]);
    config.commandCard = readRect(root["screen"]["command_card"]);
    config.minimapMode = readMinimapMode(root["screen_regions"]["minimap_mode"]);
    config.calibratedMinimap = readNormalizedRect(root["screen_regions"]["minimap"]);
    config.widescreenCalibratedMinimap =
        readNormalizedRect(root["screen_regions"]["widescreen_minimap"]);
    if (const auto key = keyNameToVirtualKey(root["calibration"]["capture_key"].asString()); key != 0)
        config.calibrationCaptureKey = key;

    const int legacyMargin = root["screen"]["edge_thickness_px"].asInt(config.edgeMarginPx);
    config.edgeMarginPx =
        positiveOr(root["edge_scroll"]["margin_px"].asInt(legacyMargin), config.edgeMarginPx);
    config.edgeMinimumDwellMs = positiveOr(root["edge_scroll"]["minimum_dwell_ms"].asInt(config.edgeMinimumDwellMs),
                                            config.edgeMinimumDwellMs);
    config.flushIntervalMs = positiveOr(root["storage"]["flush_interval_ms"].asInt(config.flushIntervalMs),
                                         config.flushIntervalMs);
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
    root["location_hotkeys"] =
        json::Value::Object{{"recall", std::move(locations)}, {"assign_modifier", "SHIFT"}};
    root["screen"] = json::Value::Object{{"auto_detect", autoScreenRegions},
                                         {"game_area", rectJson(gameArea)},
                                         {"viewport", rectJson(viewport)},
                                         {"command_card", rectJson(commandCard)}};
    root["screen_regions"] = json::Value::Object{
        {"minimap_mode", minimapModeName(minimapMode)},
        {"minimap", calibratedMinimap ? normalizedRectJson(*calibratedMinimap) : json::Value(nullptr)},
        {"widescreen_minimap", widescreenCalibratedMinimap
                                   ? normalizedRectJson(*widescreenCalibratedMinimap)
                                   : json::Value(nullptr)}};
    root["calibration"] =
        json::Value::Object{{"capture_key", virtualKeyToName(calibrationCaptureKey)}};
    root["edge_scroll"] =
        json::Value::Object{{"margin_px", edgeMarginPx}, {"minimum_dwell_ms", edgeMinimumDwellMs}};
    root["storage"] = json::Value::Object{{"flush_interval_ms", flushIntervalMs}};
    json::writeFile(path, root);
}

void Config::useAutomaticMinimap() noexcept {
    minimapMode = MinimapMode::Automatic;
}

void Config::useCalibratedMinimapOverride(NormalizedScreenRect calibration) {
    if (!calibration.valid())
        throw std::invalid_argument("Calibrated minimap override is invalid");
    calibratedMinimap = calibration;
    minimapMode = MinimapMode::CalibratedOverride;
}

void Config::useWidescreenCalibratedMinimapOverride(
    NormalizedScreenRect calibration) {
    if (!calibration.valid())
        throw std::invalid_argument("Widescreen calibrated minimap override is invalid");
    widescreenCalibratedMinimap = calibration;
}

} // namespace smp
