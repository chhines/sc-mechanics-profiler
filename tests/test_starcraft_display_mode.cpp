#include "test_framework.h"

#include "platform/screen_regions.h"
#include "platform/starcraft_display_mode.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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

bool waitForMode(smp::StarcraftDisplayModeWatcher& watcher,
                 smp::StarcraftDisplayMode expected,
                 std::chrono::milliseconds timeout =
                     std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (watcher.mode() == expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return watcher.mode() == expected;
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

TEST_CASE("display-mode watcher reads either valid initial mode") {
    using namespace std::chrono_literals;
    const auto root = temporarySettingsRoot("initial");
    const auto path = root / "CSettings.json";
    writeSettings(path, R"({"OriginalAspectRatio":true})");

    smp::StarcraftDisplayModeWatcher original(path, 50ms);
    REQUIRE(original.start());
    REQUIRE(original.mode() == smp::StarcraftDisplayMode::OriginalAspect);
    original.stop();

    writeSettings(path, R"({"OriginalAspectRatio":false})");
    smp::StarcraftDisplayModeWatcher widescreen(path, 50ms);
    REQUIRE(widescreen.start());
    REQUIRE(widescreen.mode() == smp::StarcraftDisplayMode::Widescreen);
    widescreen.stop();
    std::filesystem::remove_all(root);
}

TEST_CASE("display-mode watcher continuously follows both settings transitions") {
    using namespace std::chrono_literals;
    const auto root = temporarySettingsRoot("continuous");
    const auto path = root / "CSettings.json";
    writeSettings(path, R"({"OriginalAspectRatio":true})");

    smp::StarcraftDisplayModeWatcher watcher(path, 50ms);
    REQUIRE(watcher.start());
    REQUIRE(watcher.mode() == smp::StarcraftDisplayMode::OriginalAspect);
    writeSettings(path, R"({"OriginalAspectRatio":false})");
    REQUIRE(waitForMode(watcher, smp::StarcraftDisplayMode::Widescreen));
    writeSettings(path, R"({"OriginalAspectRatio":true})");
    REQUIRE(waitForMode(watcher, smp::StarcraftDisplayMode::OriginalAspect));

    const smp::ScreenRect client{0, 0, 1919, 1079};
    const auto originalRegions = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::OriginalAspect);
    const auto widescreenRegions = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::Widescreen);
    REQUIRE(originalRegions.displayMode != widescreenRegions.displayMode);
    REQUIRE(originalRegions.gameArea != widescreenRegions.gameArea);
    watcher.stop();
    std::filesystem::remove_all(root);
}

TEST_CASE("display-mode watcher notices target replacement by rename") {
    using namespace std::chrono_literals;
    const auto root = temporarySettingsRoot("replace");
    const auto path = root / "CSettings.json";
    const auto replacement = root / "CSettings.tmp";
    writeSettings(path, R"({"OriginalAspectRatio":true})");
    smp::StarcraftDisplayModeWatcher watcher(path, 50ms);
    REQUIRE(watcher.start());

    writeSettings(replacement, R"({"OriginalAspectRatio":false})");
    std::filesystem::remove(path);
    std::filesystem::rename(replacement, path);
    REQUIRE(waitForMode(watcher, smp::StarcraftDisplayMode::Widescreen));
    watcher.stop();
    std::filesystem::remove_all(root);
}

TEST_CASE("display-mode watcher ignores unrelated files") {
    using namespace std::chrono_literals;
    const auto root = temporarySettingsRoot("unrelated");
    const auto path = root / "CSettings.json";
    writeSettings(path, R"({"OriginalAspectRatio":true})");
    smp::StarcraftDisplayModeWatcher watcher(path, 50ms);
    REQUIRE(watcher.start());

    writeSettings(root / "Other.json", R"({"OriginalAspectRatio":false})");
    std::this_thread::sleep_for(200ms);
    REQUIRE(watcher.mode() == smp::StarcraftDisplayMode::OriginalAspect);
    watcher.stop();
    std::filesystem::remove_all(root);
}

TEST_CASE("display-mode watcher retains a known mode during malformed rewrites") {
    using namespace std::chrono_literals;
    const auto root = temporarySettingsRoot("malformed-rewrite");
    const auto path = root / "CSettings.json";
    writeSettings(path, R"({"OriginalAspectRatio":false})");
    smp::StarcraftDisplayModeWatcher watcher(path, 50ms);
    REQUIRE(watcher.start());

    writeSettings(path, "{not-json");
    std::this_thread::sleep_for(250ms);
    REQUIRE(watcher.mode() == smp::StarcraftDisplayMode::Widescreen);
    writeSettings(path, R"({"OriginalAspectRatio":true})");
    REQUIRE(waitForMode(watcher, smp::StarcraftDisplayMode::OriginalAspect));
    watcher.stop();
    std::filesystem::remove_all(root);
}

TEST_CASE("display-mode watcher starts unknown on malformed input and later recovers") {
    using namespace std::chrono_literals;
    const auto root = temporarySettingsRoot("malformed-startup");
    const auto path = root / "CSettings.json";
    writeSettings(path, "{not-json");
    smp::StarcraftDisplayModeWatcher watcher(path, 50ms);
    REQUIRE(watcher.start());
    REQUIRE(watcher.mode() == smp::StarcraftDisplayMode::Unknown);

    writeSettings(path, R"({"OriginalAspectRatio":false})");
    REQUIRE(waitForMode(watcher, smp::StarcraftDisplayMode::Widescreen));
    watcher.stop();
    std::filesystem::remove_all(root);
}

TEST_CASE("display-mode watcher keeps watching when the target is initially missing") {
    using namespace std::chrono_literals;
    const auto root = temporarySettingsRoot("missing-startup");
    const auto path = root / "CSettings.json";
    smp::StarcraftDisplayModeWatcher watcher(path, 50ms);
    REQUIRE(watcher.start());
    REQUIRE(watcher.mode() == smp::StarcraftDisplayMode::Unknown);

    writeSettings(path, R"({"OriginalAspectRatio":true})");
    REQUIRE(waitForMode(watcher, smp::StarcraftDisplayMode::OriginalAspect));
    watcher.stop();
    std::filesystem::remove_all(root);
}

TEST_CASE("display-mode watcher stops promptly while its directory is idle") {
    using namespace std::chrono_literals;
    const auto root = temporarySettingsRoot("stop");
    const auto path = root / "CSettings.json";
    smp::StarcraftDisplayModeWatcher watcher(path, 50ms);
    REQUIRE(watcher.start());
    const auto started = std::chrono::steady_clock::now();
    watcher.stop();
    REQUIRE(std::chrono::steady_clock::now() - started < 1s);
    std::filesystem::remove_all(root);
}
