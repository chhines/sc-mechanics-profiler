#include "test_framework.h"

#include "platform/screen_regions.h"
#include "platform/starcraft_display_mode.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path temporarySettingsRoot(const char* label) {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("starcraft-display-mode-") + label + '-' +
                       std::to_string(nonce));
    std::filesystem::create_directories(root);
    return root;
}

void writeSettings(const std::filesystem::path& path,
                   const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

} // namespace

TEST_CASE("StarCraft settings true selects original-aspect mode") {
    const auto root = temporarySettingsRoot("true");
    const auto path = root / "CSettings.json";
    writeSettings(path, R"({"OriginalAspectRatio":true})");
    REQUIRE(smp::readStarcraftDisplayMode(path) ==
            smp::StarcraftDisplayMode::OriginalAspect);
    std::filesystem::remove_all(root);
}

TEST_CASE("StarCraft settings false selects widescreen mode") {
    const auto root = temporarySettingsRoot("false");
    const auto path = root / "CSettings.json";
    writeSettings(path, R"({"OriginalAspectRatio":false})");
    REQUIRE(smp::readStarcraftDisplayMode(path) ==
            smp::StarcraftDisplayMode::Widescreen);
    std::filesystem::remove_all(root);
}

TEST_CASE("missing malformed and wrong-type display settings are unknown") {
    const auto root = temporarySettingsRoot("invalid");
    const auto missing = root / "missing.json";
    REQUIRE(smp::readStarcraftDisplayMode(missing) ==
            smp::StarcraftDisplayMode::Unknown);

    const auto malformed = root / "malformed.json";
    writeSettings(malformed, "{not-json");
    REQUIRE(smp::readStarcraftDisplayMode(malformed) ==
            smp::StarcraftDisplayMode::Unknown);

    const auto missingField = root / "missing-field.json";
    writeSettings(missingField, R"({"OtherSetting":true})");
    REQUIRE(smp::readStarcraftDisplayMode(missingField) ==
            smp::StarcraftDisplayMode::Unknown);

    const auto wrongType = root / "wrong-type.json";
    writeSettings(wrongType, R"({"OriginalAspectRatio":"true"})");
    REQUIRE(smp::readStarcraftDisplayMode(wrongType) ==
            smp::StarcraftDisplayMode::Unknown);
    std::filesystem::remove_all(root);
}

TEST_CASE("display-mode reader detects a settings change without polling every tick") {
    using namespace std::chrono_literals;
    const auto root = temporarySettingsRoot("refresh");
    const auto path = root / "CSettings.json";
    writeSettings(path, R"({"OriginalAspectRatio":true})");

    smp::StarcraftDisplayModeReader reader(path, 500ms);
    const auto start = std::chrono::steady_clock::now();
    const auto original = reader.refreshIfDue(start);
    REQUIRE(original.checked);
    REQUIRE(original.changed);
    REQUIRE(original.mode == smp::StarcraftDisplayMode::OriginalAspect);

    writeSettings(path, R"({"OriginalAspectRatio":false})");
    const auto cached = reader.refreshIfDue(start + 100ms);
    REQUIRE(!cached.checked);
    REQUIRE(!cached.changed);
    REQUIRE(cached.mode == smp::StarcraftDisplayMode::OriginalAspect);

    const auto changed = reader.refreshIfDue(start + 500ms);
    REQUIRE(changed.checked);
    REQUIRE(changed.changed);
    REQUIRE(changed.mode == smp::StarcraftDisplayMode::Widescreen);

    const smp::ScreenRect client{0, 0, 1919, 1079};
    const auto originalRegions = smp::calculateStarcraftScreenRegions(
        client, original.mode);
    const auto widescreenRegions = smp::calculateStarcraftScreenRegions(
        client, changed.mode);
    REQUIRE(originalRegions.displayMode != widescreenRegions.displayMode);
    REQUIRE(originalRegions.gameArea != widescreenRegions.gameArea);
    std::filesystem::remove_all(root);
}
