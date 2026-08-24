#include "test_framework.h"

#include "analysis/analyzer.h"
#include "storage/session.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

#pragma pack(push, 1)
struct LegacyNavFileHeader {
    char magic[4]{};
    std::uint16_t schemaVersion{};
    std::uint16_t headerSize{};
    std::uint16_t recordSize{};
    std::uint16_t flags{};
    std::uint64_t qpcFrequency{};
    std::int64_t sessionStartUnixMs{};
    std::uint64_t activeDurationUs{};
    std::uint64_t pausedDurationUs{};
    std::uint64_t droppedEventCount{};
    std::uint32_t recordCount{};
    std::uint32_t reserved{};
};

struct LegacyNavRecord {
    std::uint64_t activeUs{};
    std::uint64_t durationUs{};
    std::int32_t cursorX{};
    std::int32_t cursorY{};
    std::uint8_t type{};
    std::int8_t id{-1};
    std::int8_t direction{};
    std::uint8_t reserved{};
};

struct LegacyNavRecordV2 {
    std::uint64_t activeUs{};
    std::uint64_t durationUs{};
    std::int32_t cursorX{};
    std::int32_t cursorY{};
    std::uint8_t type{};
    std::int8_t id{-1};
    std::int8_t direction{};
    std::uint8_t reserved{};
    std::int32_t startCursorX{};
    std::int32_t startCursorY{};
};

struct LegacyTimelineAnchorV4 {
    std::uint64_t activeTimelineStartQpcTicks{};
    std::int64_t activeTimelineStartUnixNs{};
};

struct LegacyNavRecordV4 {
    std::uint64_t activeUs{};
    std::uint64_t durationUs{};
    std::int32_t cursorX{};
    std::int32_t cursorY{};
    std::uint8_t type{};
    std::int8_t id{-1};
    std::int8_t direction{};
    std::uint8_t reserved{};
    std::int32_t startCursorX{};
    std::int32_t startCursorY{};
    std::uint64_t qpcOffsetTicks{};
};
#pragma pack(pop)

static_assert(sizeof(LegacyNavFileHeader) == 60);
static_assert(sizeof(LegacyNavRecord) == 28);
static_assert(sizeof(LegacyNavRecordV2) == 36);
static_assert(sizeof(LegacyTimelineAnchorV4) == 16);
static_assert(sizeof(LegacyNavRecordV4) == 44);

std::filesystem::path temporaryRoot(const char* label) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("starcraft-mechanics-profiler-") + label + '-' + std::to_string(nonce));
    std::filesystem::create_directories(root);
    return root;
}

smp::AnalysisResult sampleAnalysis() {
    smp::AnalysisResult result;
    result.activeDurationSeconds = 61.234567;
    result.pausedDurationSeconds = 2.345678;
    result.droppedEventCount = 3;
    result.locationRecallCount = 2;
    result.navigationEvents.push_back(
        {1000, 1000.123, smp::CameraNavigationType::ControlGroupJump, 1, 900, 500, 0.0,
         smp::EdgeDirection::None, 900, 500});
    result.recenters.push_back({1200, 1200.234, smp::CameraRecenterType::ControlGroup, 1, 901, 501});
    result.navigationEvents.push_back(
        {1400, 1400.345, smp::CameraNavigationType::LocationHotkey, 2, 902, 502, 0.0,
         smp::EdgeDirection::None, 902, 502});
    result.recenters.push_back({1600, 1600.456, smp::CameraRecenterType::LocationHotkey, 2, 903, 503});
    result.navigationEvents.push_back(
        {1800, 1800.567, smp::CameraNavigationType::MinimapJump, -1, 373, 871, 0.0,
         smp::EdgeDirection::None, 373, 871});
    result.navigationEvents.push_back(
        {2000, 2000.678, smp::CameraNavigationType::EdgeScroll, -1, 500, 500, 200.456,
         smp::EdgeDirection::Left, 242, 500});
    result.mechanicalEvents.push_back(
        {1100, 1100.111, smp::MechanicalInputType::ControlGroupSelect, '5', 0x06,
         smp::ModifierNone, 5, 900, 500});
    result.mechanicalEvents.push_back(
        {1500, 1500.222, smp::MechanicalInputType::KeyPress, 'D', 0x20,
         static_cast<std::uint16_t>(smp::ModifierCtrl | smp::ModifierAlt), -1, 901, 501});
    result.mechanicalEvents.push_back(
        {1900, 1900.333, smp::MechanicalInputType::MouseWheel, 0, 0,
         smp::ModifierShift, -120, 333, 444});
    result.mechanicalEvents.push_back(
        {1950, 1950.444, smp::MechanicalInputType::ControlGroupAdd, '3', 0x04,
         smp::ModifierShift, 3, 334, 445});
    return result;
}

bool readFailsWith(const std::filesystem::path& path, const std::string& expected) {
    try {
        (void)smp::readNavSession(path);
        return false;
    } catch (const std::exception& error) {
        return std::string(error.what()).find(expected) != std::string::npos;
    }
}

} // namespace

TEST_CASE("both normalized minimap calibrations and capture key round trip") {
    const auto root = temporaryRoot("config-test");
    const auto path = root / "config.json";

    smp::Config config;
    config.useCalibratedMinimapOverride(
        smp::NormalizedScreenRect{0.025694, 0.739815, 0.193750, 0.962963});
    config.useWidescreenCalibratedMinimapOverride(
        smp::NormalizedScreenRect{0.010417, 0.722222, 0.161458, 0.986111});
    config.calibrationCaptureKey = 0x79; // F10, proving the key is configurable
    config.save(path);

    const auto loaded = smp::Config::loadOrCreate(path);
    REQUIRE(loaded.calibratedMinimap.has_value());
    REQUIRE_NEAR(loaded.calibratedMinimap->left, 0.025694, 0.000001);
    REQUIRE_NEAR(loaded.calibratedMinimap->bottom, 0.962963, 0.000001);
    REQUIRE(loaded.widescreenCalibratedMinimap.has_value());
    REQUIRE_NEAR(loaded.widescreenCalibratedMinimap->left, 0.010417, 0.000001);
    REQUIRE_NEAR(loaded.widescreenCalibratedMinimap->bottom, 0.986111, 0.000001);
    REQUIRE(loaded.originalAspectMinimapMode ==
            smp::MinimapMode::CalibratedOverride);
    REQUIRE(loaded.widescreenMinimapMode ==
            smp::MinimapMode::CalibratedOverride);
    REQUIRE(loaded.calibrationCaptureKey == 0x79);
    const auto json = smp::json::parseFile(path);
    REQUIRE(json["screen_regions"]["minimap_mode"].asString() ==
            "calibrated_override");
    REQUIRE(json["screen_regions"]["widescreen_minimap_mode"].asString() ==
            "calibrated_override");
    REQUIRE(json["screen_regions"]["minimap"]["left_norm"].isNumber());
    REQUIRE(json["screen_regions"]["widescreen_minimap"]["left_norm"].isNumber());
    REQUIRE(json["screen"]["minimap"].isNull());
    std::filesystem::remove_all(root);
}

TEST_CASE("legacy original minimap override leaves widescreen mode automatic") {
    const auto root = temporaryRoot("legacy-minimap-mode");
    const auto path = root / "config.json";
    smp::json::Value legacy(smp::json::Value::Object{});
    legacy["screen_regions"]["minimap_mode"] = "calibrated_override";
    legacy["screen_regions"]["minimap"] = smp::json::Value::Object{
        {"left_norm", 37.0 / 1440.0}, {"top_norm", 799.0 / 1080.0},
        {"right_norm", 279.0 / 1440.0}, {"bottom_norm", 1040.0 / 1080.0}};
    legacy["screen_regions"]["widescreen_minimap"] =
        smp::json::Value::Object{
            {"left_norm", 13.0 / 1920.0}, {"top_norm", 783.0 / 1080.0},
            {"right_norm", 300.0 / 1920.0}, {"bottom_norm", 1070.0 / 1080.0}};
    smp::json::writeFile(path, legacy);

    const auto loaded = smp::Config::loadOrCreate(path);
    REQUIRE(loaded.calibratedMinimap.has_value());
    REQUIRE(loaded.widescreenCalibratedMinimap.has_value());
    REQUIRE(loaded.originalAspectMinimapMode ==
            smp::MinimapMode::CalibratedOverride);
    REQUIRE(loaded.widescreenMinimapMode == smp::MinimapMode::Automatic);
    std::filesystem::remove_all(root);
}

TEST_CASE("fresh config writes automatic minimap mode without calibration") {
    const auto root = temporaryRoot("fresh-minimap-mode");
    const auto path = root / "config.json";
    const auto loaded = smp::Config::loadOrCreate(path);
    REQUIRE(loaded.originalAspectMinimapMode == smp::MinimapMode::Automatic);
    REQUIRE(loaded.widescreenMinimapMode == smp::MinimapMode::Automatic);
    REQUIRE(!loaded.calibratedMinimap.has_value());
    REQUIRE(!loaded.widescreenCalibratedMinimap.has_value());
    const auto json = smp::json::parseFile(path);
    REQUIRE(json["screen_regions"]["minimap_mode"].asString() == "automatic");
    REQUIRE(json["screen_regions"]["widescreen_minimap_mode"].asString() ==
            "automatic");
    REQUIRE(json["screen_regions"]["minimap"].isNull());
    REQUIRE(json["screen_regions"]["widescreen_minimap"].isNull());
    std::filesystem::remove_all(root);
}

TEST_CASE("navigation retention defaults to keep all and round trips keep last") {
    const auto root = temporaryRoot("nav-retention-config");
    const auto path = root / "config.json";
    const auto defaults = smp::Config::loadOrCreate(path);
    REQUIRE(defaults.navRetention.mode == smp::NavRetentionMode::KeepAll);
    REQUIRE(defaults.navRetention.gamesToKeep >= 1);

    const auto existingPath = root / "existing-config.json";
    smp::json::Value existing(smp::json::Value::Object{});
    existing["storage"]["flush_interval_ms"] = 1000;
    smp::json::writeFile(existingPath, existing);
    const auto existingUser = smp::Config::loadOrCreate(existingPath);
    REQUIRE(existingUser.navRetention.mode ==
            smp::NavRetentionMode::KeepAll);

    auto configured = defaults;
    configured.navRetention = {smp::NavRetentionMode::KeepLastGames, 7};
    configured.save(path);
    const auto loaded = smp::Config::loadOrCreate(path);
    REQUIRE(loaded.navRetention.mode ==
            smp::NavRetentionMode::KeepLastGames);
    REQUIRE(loaded.navRetention.gamesToKeep == 7);
    const auto json = smp::json::parseFile(path);
    REQUIRE(json["storage"]["nav_retention"]["policy"].asString() ==
            "keep_last_games");
    REQUIRE(json["storage"]["nav_retention"]["games_to_keep"].asInt() ==
            7);
    std::filesystem::remove_all(root);
}

TEST_CASE("invalid navigation retention counts are clamped to one") {
    const auto root = temporaryRoot("nav-retention-config-validation");
    const auto path = root / "config.json";
    smp::json::Value config(smp::json::Value::Object{});
    config["storage"]["nav_retention"] = smp::json::Value::Object{
        {"policy", "keep_last_games"}, {"games_to_keep", 0}};
    smp::json::writeFile(path, config);

    const auto loaded = smp::Config::loadOrCreate(path);
    REQUIRE(loaded.navRetention.mode ==
            smp::NavRetentionMode::KeepLastGames);
    REQUIRE(loaded.navRetention.gamesToKeep == 1);
    std::filesystem::remove_all(root);
}

TEST_CASE("calibration and automatic actions change only their display mode") {
    smp::Config config;
    const smp::NormalizedScreenRect calibration{
        37.0 / 1440.0, 799.0 / 1080.0,
        279.0 / 1440.0, 1040.0 / 1080.0};
    const smp::NormalizedScreenRect widescreenCalibration{
        20.0 / 1920.0, 780.0 / 1080.0,
        310.0 / 1920.0, 1065.0 / 1080.0};
    config.useCalibratedMinimapOverride(calibration);
    config.useWidescreenCalibratedMinimapOverride(widescreenCalibration);
    REQUIRE(config.originalAspectMinimapMode ==
            smp::MinimapMode::CalibratedOverride);
    REQUIRE(config.widescreenMinimapMode ==
            smp::MinimapMode::CalibratedOverride);
    REQUIRE(config.calibratedMinimap == calibration);
    REQUIRE(config.widescreenCalibratedMinimap == widescreenCalibration);

    config.useOriginalAspectAutomaticMinimap();
    REQUIRE(config.originalAspectMinimapMode == smp::MinimapMode::Automatic);
    REQUIRE(config.widescreenMinimapMode ==
            smp::MinimapMode::CalibratedOverride);

    config.useCalibratedMinimapOverride(calibration);
    config.useWidescreenAutomaticMinimap();
    REQUIRE(config.originalAspectMinimapMode ==
            smp::MinimapMode::CalibratedOverride);
    REQUIRE(config.widescreenMinimapMode == smp::MinimapMode::Automatic);
    REQUIRE(config.calibratedMinimap == calibration);
    REQUIRE(config.widescreenCalibratedMinimap == widescreenCalibration);
}

TEST_CASE("compact navigation binary round trips transitions recenters and metadata") {
    const auto root = temporaryRoot("nav-roundtrip");
    const auto path = root / "sample.nav";
    const auto expected = sampleAnalysis();
    const smp::QpcWallClockAnchor anchor{500, 1'786'281'445'500'000'000};
    smp::writeNavSession(path, expected, "sample", 10'000'000, 1'786'281'445'123, anchor);

    REQUIRE(std::filesystem::exists(path));
    REQUIRE(!std::filesystem::exists(path.string() + ".tmp"));
    REQUIRE(std::filesystem::file_size(path) == 84 + 6 * 44 + 4 * 34);
    const auto loaded = smp::readNavSession(path);
    REQUIRE(loaded.sessionId == "sample");
    REQUIRE(loaded.qpcFrequency == 10'000'000);
    REQUIRE(loaded.sessionStartUnixMs == 1'786'281'445'123);
    REQUIRE(loaded.activeTimelineAnchor.has_value());
    REQUIRE(loaded.activeTimelineAnchor->qpcTicks == anchor.qpcTicks);
    REQUIRE(loaded.activeTimelineAnchor->unixNanoseconds == anchor.unixNanoseconds);
    REQUIRE_NEAR(loaded.analysis.activeDurationSeconds, expected.activeDurationSeconds, 0.000001);
    REQUIRE_NEAR(loaded.analysis.pausedDurationSeconds, expected.pausedDurationSeconds, 0.000001);
    REQUIRE(loaded.analysis.droppedEventCount == 3);
    REQUIRE(loaded.analysis.locationRecallCount == 2);
    REQUIRE(loaded.analysis.navigationEvents.size() == 4);
    REQUIRE(loaded.analysis.recenters.size() == 2);
    REQUIRE(loaded.analysis.mechanicalEvents.size() == 4);
    REQUIRE(loaded.analysis.navigationEvents[0].type == smp::CameraNavigationType::ControlGroupJump);
    REQUIRE(loaded.analysis.navigationEvents[0].id == 1);
    REQUIRE(loaded.analysis.navigationEvents[0].timestampTicks == 1000);
    REQUIRE_NEAR(loaded.analysis.navigationEvents[0].activeMs, 1000.123, 0.001);
    REQUIRE(loaded.analysis.navigationEvents[1].type == smp::CameraNavigationType::LocationHotkey);
    REQUIRE(loaded.analysis.navigationEvents[2].type == smp::CameraNavigationType::MinimapJump);
    REQUIRE(loaded.analysis.navigationEvents[2].cursorX == 373);
    REQUIRE(loaded.analysis.navigationEvents[2].cursorY == 871);
    REQUIRE(loaded.analysis.navigationEvents[3].type == smp::CameraNavigationType::EdgeScroll);
    REQUIRE(loaded.analysis.navigationEvents[3].edgeDirection == smp::EdgeDirection::Left);
    REQUIRE_NEAR(loaded.analysis.navigationEvents[3].activeMs, 2000.678, 0.001);
    REQUIRE(loaded.analysis.navigationEvents[3].cursorX == 500);
    REQUIRE(loaded.analysis.navigationEvents[3].cursorY == 500);
    REQUIRE(loaded.analysis.navigationEvents[3].startCursorX == 242);
    REQUIRE(loaded.analysis.navigationEvents[3].startCursorY == 500);
    REQUIRE_NEAR(loaded.analysis.navigationEvents[3].durationMs, 200.456, 0.001);
    REQUIRE(loaded.analysis.recenters[0].type == smp::CameraRecenterType::ControlGroup);
    REQUIRE(loaded.analysis.recenters[1].type == smp::CameraRecenterType::LocationHotkey);
    const auto& groupSelect = loaded.analysis.mechanicalEvents[0];
    REQUIRE(groupSelect.type == smp::MechanicalInputType::ControlGroupSelect);
    REQUIRE(groupSelect.timestampTicks == 1100);
    REQUIRE_NEAR(groupSelect.activeMs, 1100.111, 0.001);
    REQUIRE(groupSelect.virtualKey == '5');
    REQUIRE(groupSelect.scanCode == 0x06);
    REQUIRE(groupSelect.modifiers == smp::ModifierNone);
    REQUIRE(groupSelect.value == 5);
    const auto& keyPress = loaded.analysis.mechanicalEvents[1];
    REQUIRE(keyPress.type == smp::MechanicalInputType::KeyPress);
    REQUIRE(keyPress.timestampTicks == 1500);
    REQUIRE(keyPress.virtualKey == 'D');
    REQUIRE(keyPress.scanCode == 0x20);
    REQUIRE((keyPress.modifiers & smp::ModifierCtrl) != 0);
    REQUIRE((keyPress.modifiers & smp::ModifierAlt) != 0);
    REQUIRE(keyPress.value == -1);
    const auto& wheel = loaded.analysis.mechanicalEvents[2];
    REQUIRE(wheel.type == smp::MechanicalInputType::MouseWheel);
    REQUIRE(wheel.timestampTicks == 1900);
    REQUIRE(wheel.value == -120);
    REQUIRE(wheel.cursorX == 333);
    REQUIRE(wheel.cursorY == 444);
    const auto& groupAdd = loaded.analysis.mechanicalEvents[3];
    REQUIRE(groupAdd.type == smp::MechanicalInputType::ControlGroupAdd);
    REQUIRE(groupAdd.value == 3);
    REQUIRE(groupAdd.modifiers == smp::ModifierShift);
    const auto wheelUnixNs = smp::qpcTimestampToUnixNanoseconds(loaded, wheel.timestampTicks);
    REQUIRE(wheelUnixNs.has_value());
    const auto summary = smp::analysisToJson(loaded.analysis, loaded.sessionId);
    REQUIRE(summary["camera_navigation"]["control_group"]["recenters"].asInt() == 1);
    REQUIRE(summary["camera_navigation"]["location_hotkey"]["recalls"].asInt() == 2);
    REQUIRE(summary["camera_navigation"]["location_hotkey"]["repeated_recalls"].asInt() == 1);
    std::filesystem::remove_all(root);
}

TEST_CASE("navigation synchronization keeps QPC time separate from pause-excluded active time") {
    const auto root = temporaryRoot("nav-synchronized-timeline");
    const auto path = root / "synchronized.nav";
    smp::AnalysisResult result;
    result.activeDurationSeconds = 1.0;
    result.pausedDurationSeconds = 1.0;
    result.navigationEvents.push_back(
        {5100, 100.0, smp::CameraNavigationType::MinimapJump, -1, 350, 900, 0.0,
         smp::EdgeDirection::None, 350, 900});
    result.navigationEvents.push_back(
        {6600, 600.0, smp::CameraNavigationType::MinimapJump, -1, 360, 900, 0.0,
         smp::EdgeDirection::None, 360, 900});
    const smp::QpcWallClockAnchor anchor{5000, 10'000'000'000};

    // sessionStartUnixMs deliberately represents an earlier writer-creation time and is not the event anchor.
    smp::writeNavSession(path, result, "synchronized", 1000, 1234, anchor);
    const auto loaded = smp::readNavSession(path);
    REQUIRE(loaded.sessionStartUnixMs == 1234);
    REQUIRE(loaded.activeTimelineAnchor.has_value());
    REQUIRE(loaded.activeTimelineAnchor->qpcTicks == 5000);
    REQUIRE(loaded.analysis.navigationEvents[0].timestampTicks == 5100);
    REQUIRE_NEAR(loaded.analysis.navigationEvents[0].activeMs, 100.0, 0.001);
    REQUIRE(loaded.analysis.navigationEvents[1].timestampTicks == 6600);
    REQUIRE_NEAR(loaded.analysis.navigationEvents[1].activeMs, 600.0, 0.001);

    const auto firstUnixNs =
        smp::qpcTimestampToUnixNanoseconds(loaded, loaded.analysis.navigationEvents[0].timestampTicks);
    const auto secondUnixNs =
        smp::qpcTimestampToUnixNanoseconds(loaded, loaded.analysis.navigationEvents[1].timestampTicks);
    REQUIRE(firstUnixNs.has_value());
    REQUIRE(secondUnixNs.has_value());
    REQUIRE(*firstUnixNs == 10'100'000'000);
    REQUIRE(*secondUnixNs == 11'600'000'000);
    REQUIRE(*secondUnixNs - *firstUnixNs == 1'500'000'000); // Includes the one-second pause.
    REQUIRE_NEAR(loaded.analysis.navigationEvents[1].activeMs -
                     loaded.analysis.navigationEvents[0].activeMs,
                 500.0, 0.001); // Active time still excludes that pause.
    REQUIRE(*firstUnixNs != loaded.sessionStartUnixMs * 1'000'000 + 100'000'000);
    std::filesystem::remove_all(root);
}

TEST_CASE("mechanical input at the foreground-gain QPC anchor persists without shifting") {
    constexpr std::uint64_t observationTimestampTicks = 5000;
    smp::Config config;
    smp::Analyzer analyzer(config, 1000);

    smp::RawInputEvent event{};
    event.timestampTicks = observationTimestampTicks;
    event.type = smp::RawEventType::ForegroundGained;
    analyzer.process(event);

    event.type = smp::RawEventType::KeyDown;
    event.virtualKey = 'D';
    event.scanCode = 0x20;
    analyzer.process(event);
    analyzer.finalize(observationTimestampTicks + 10, 0);

    REQUIRE(analyzer.result().mechanicalEvents.size() == 1);
    REQUIRE(analyzer.result().mechanicalEvents[0].timestampTicks ==
            observationTimestampTicks);
    REQUIRE_NEAR(analyzer.result().mechanicalEvents[0].activeMs, 0.0, 0.001);

    const auto root = temporaryRoot("equal-anchor-mechanical-event");
    const auto path = root / "equal-anchor.nav";
    const smp::QpcWallClockAnchor anchor{
        observationTimestampTicks, 10'000'000'000};
    smp::writeNavSession(path, analyzer.result(), "equal-anchor", 1000,
                         1234, anchor);

    const auto loaded = smp::readNavSession(path);
    REQUIRE(loaded.activeTimelineAnchor.has_value());
    REQUIRE(loaded.activeTimelineAnchor->qpcTicks ==
            observationTimestampTicks);
    REQUIRE(loaded.analysis.mechanicalEvents.size() == 1);
    REQUIRE(loaded.analysis.mechanicalEvents[0].timestampTicks ==
            observationTimestampTicks);
    std::filesystem::remove_all(root);
}

TEST_CASE("schema version one navigation sessions remain readable") {
    const auto root = temporaryRoot("nav-v1-compatibility");
    const auto path = root / "legacy.nav";

    LegacyNavFileHeader header;
    std::memcpy(header.magic, "SCNV", 4);
    header.schemaVersion = 1;
    header.headerSize = sizeof(header);
    header.recordSize = sizeof(LegacyNavRecord);
    header.qpcFrequency = 1000;
    header.sessionStartUnixMs = 1234;
    header.activeDurationUs = 2'000'000;
    header.recordCount = 1;

    LegacyNavRecord record;
    record.activeUs = 1'000'000;
    record.durationUs = 200'000;
    record.cursorX = 500;
    record.cursorY = 400;
    record.type = 5; // EdgeScroll
    record.direction = static_cast<std::int8_t>(smp::EdgeDirection::Left);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&record), sizeof(record));
    output.close();

    const auto loaded = smp::readNavSession(path);
    REQUIRE(!loaded.activeTimelineAnchor.has_value());
    REQUIRE(loaded.analysis.mechanicalEvents.empty());
    REQUIRE(loaded.analysis.navigationEvents.size() == 1);
    const auto& edge = loaded.analysis.navigationEvents.front();
    REQUIRE(edge.type == smp::CameraNavigationType::EdgeScroll);
    REQUIRE_NEAR(edge.activeMs, 800.0, 0.001);
    REQUIRE(edge.timestampTicks == 800);
    REQUIRE(edge.cursorX == 500);
    REQUIRE(edge.cursorY == 400);
    REQUIRE(edge.startCursorX == 500);
    REQUIRE(edge.startCursorY == 400);
    REQUIRE(!smp::qpcTimestampToUnixNanoseconds(loaded, edge.timestampTicks).has_value());
    std::filesystem::remove_all(root);
}

TEST_CASE("schema version two edge timestamps and start cursors upgrade on read") {
    const auto root = temporaryRoot("nav-v2-compatibility");
    const auto path = root / "legacy-v2.nav";

    LegacyNavFileHeader header;
    std::memcpy(header.magic, "SCNV", 4);
    header.schemaVersion = 2;
    header.headerSize = sizeof(header);
    header.recordSize = sizeof(LegacyNavRecordV2);
    header.qpcFrequency = 1000;
    header.sessionStartUnixMs = 1234;
    header.activeDurationUs = 2'000'000;
    header.recordCount = 1;

    LegacyNavRecordV2 record;
    record.activeUs = 1'000'000;
    record.durationUs = 200'000;
    record.cursorX = 500;
    record.cursorY = 400;
    record.type = 5; // EdgeScroll
    record.direction = static_cast<std::int8_t>(smp::EdgeDirection::Left);
    record.startCursorX = 242;
    record.startCursorY = 400;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&record), sizeof(record));
    output.close();

    const auto loaded = smp::readNavSession(path);
    REQUIRE(!loaded.activeTimelineAnchor.has_value());
    REQUIRE(loaded.analysis.mechanicalEvents.empty());
    REQUIRE(loaded.analysis.navigationEvents.size() == 1);
    const auto& edge = loaded.analysis.navigationEvents.front();
    REQUIRE_NEAR(edge.activeMs, 800.0, 0.001);
    REQUIRE(edge.timestampTicks == 800);
    REQUIRE(edge.startCursorX == 242);
    REQUIRE(edge.startCursorY == 400);
    REQUIRE(edge.cursorX == 500);
    REQUIRE(edge.cursorY == 400);
    REQUIRE(!smp::qpcTimestampToUnixNanoseconds(loaded, edge.timestampTicks).has_value());
    std::filesystem::remove_all(root);
}

TEST_CASE("schema version three remains readable without inventing a synchronization anchor") {
    const auto root = temporaryRoot("nav-v3-compatibility");
    const auto path = root / "legacy-v3.nav";

    LegacyNavFileHeader header;
    std::memcpy(header.magic, "SCNV", 4);
    header.schemaVersion = 3;
    header.headerSize = sizeof(header);
    header.recordSize = sizeof(LegacyNavRecordV2);
    header.qpcFrequency = 1000;
    header.sessionStartUnixMs = 1234;
    header.activeDurationUs = 2'000'000;
    header.recordCount = 1;

    LegacyNavRecordV2 record;
    record.activeUs = 800'000; // Version 3 already stores an edge pan's start active time.
    record.durationUs = 200'000;
    record.cursorX = 500;
    record.cursorY = 400;
    record.type = 5; // EdgeScroll
    record.direction = static_cast<std::int8_t>(smp::EdgeDirection::Left);
    record.startCursorX = 242;
    record.startCursorY = 400;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&record), sizeof(record));
    output.close();

    const auto loaded = smp::readNavSession(path);
    REQUIRE(!loaded.activeTimelineAnchor.has_value());
    REQUIRE(loaded.analysis.mechanicalEvents.empty());
    REQUIRE(loaded.analysis.navigationEvents.size() == 1);
    const auto& edge = loaded.analysis.navigationEvents.front();
    REQUIRE_NEAR(edge.activeMs, 800.0, 0.001);
    REQUIRE(edge.timestampTicks == 800);
    REQUIRE(edge.startCursorX == 242);
    REQUIRE(!smp::qpcTimestampToUnixNanoseconds(loaded, edge.timestampTicks).has_value());
    std::filesystem::remove_all(root);
}

TEST_CASE("schema version four retains synchronized camera data with no invented mechanical stream") {
    const auto root = temporaryRoot("nav-v4-compatibility");
    const auto path = root / "legacy-v4.nav";

    LegacyNavFileHeader header;
    std::memcpy(header.magic, "SCNV", 4);
    header.schemaVersion = 4;
    header.headerSize = sizeof(header) + sizeof(LegacyTimelineAnchorV4);
    header.recordSize = sizeof(LegacyNavRecordV4);
    header.flags = 1;
    header.qpcFrequency = 1000;
    header.sessionStartUnixMs = 1234;
    header.activeDurationUs = 2'000'000;
    header.recordCount = 1;
    const LegacyTimelineAnchorV4 anchor{5000, 10'000'000'000};

    LegacyNavRecordV4 record;
    record.activeUs = 100'000;
    record.cursorX = 350;
    record.cursorY = 900;
    record.type = 4; // MinimapJump
    record.qpcOffsetTicks = 100;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&anchor), sizeof(anchor));
    output.write(reinterpret_cast<const char*>(&record), sizeof(record));
    output.close();

    const auto loaded = smp::readNavSession(path);
    REQUIRE(loaded.activeTimelineAnchor.has_value());
    REQUIRE(loaded.analysis.navigationEvents.size() == 1);
    REQUIRE(loaded.analysis.navigationEvents[0].type == smp::CameraNavigationType::MinimapJump);
    REQUIRE(loaded.analysis.navigationEvents[0].timestampTicks == 5100);
    REQUIRE_NEAR(loaded.analysis.navigationEvents[0].activeMs, 100.0, 0.001);
    REQUIRE(loaded.analysis.mechanicalEvents.empty());
    std::filesystem::remove_all(root);
}

TEST_CASE("navigation reader rejects invalid magic cleanly") {
    const auto root = temporaryRoot("nav-invalid-magic");
    const auto path = root / "invalid.nav";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    std::string bytes(60, '\0');
    bytes.replace(0, 4, "NOPE");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE(readFailsWith(path, "magic"));
    std::filesystem::remove_all(root);
}

TEST_CASE("navigation reader rejects unsupported schema version with a useful error") {
    const auto root = temporaryRoot("nav-schema");
    const auto path = root / "schema.nav";
    smp::AnalysisResult empty;
    smp::writeNavSession(path, empty, "schema", 1000, 1234);
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    const std::uint16_t unsupported = smp::navFileSchemaVersion + 1;
    file.seekp(4);
    file.write(reinterpret_cast<const char*>(&unsupported), sizeof(unsupported));
    file.close();
    REQUIRE(readFailsWith(path, "Unsupported navigation session schema version"));
    std::filesystem::remove_all(root);
}

TEST_CASE("empty navigation session is valid and contains only its compact header") {
    const auto root = temporaryRoot("nav-empty");
    const auto path = root / "empty.nav";
    smp::AnalysisResult empty;
    smp::writeNavSession(path, empty, "empty", 1000, 1234);
    REQUIRE(std::filesystem::file_size(path) == 84);
    const auto loaded = smp::readNavSession(path);
    REQUIRE(loaded.analysis.navigationEvents.empty());
    REQUIRE(loaded.analysis.recenters.empty());
    std::filesystem::remove_all(root);
}

TEST_CASE("latest session discovery selects nav files and ignores optional raw files") {
    const auto root = temporaryRoot("nav-latest");
    const auto older = root / "older.nav";
    const auto newer = root / "newer.nav";
    smp::AnalysisResult empty;
    smp::writeNavSession(older, empty, "older", 1000, 1000);
    smp::writeNavSession(newer, empty, "newer", 1000, 2000);
    std::ofstream(root / "newer.events.bin", std::ios::binary).put('\0');
    REQUIRE(smp::listNavSessions(root).size() == 2);
    REQUIRE(smp::resolveNavSession(root, "latest") == newer);
    std::filesystem::remove_all(root);
}

TEST_CASE("CSV export is generated explicitly from navigation binary records") {
    const auto root = temporaryRoot("nav-export");
    const auto sessions = root / "sessions";
    const auto exports = root / "exports";
    std::filesystem::create_directories(sessions);
    smp::writeNavSession(sessions / "known.nav", sampleAnalysis(), "known", 1000, 1234,
                         smp::QpcWallClockAnchor{0, 1'234'000'000});
    const auto csvPath = smp::exportSessionCsv(sessions, exports, "known");
    REQUIRE(std::filesystem::exists(csvPath));
    std::ifstream input(csvPath, std::ios::binary);
    std::ostringstream csv;
    csv << input.rdbuf();
    REQUIRE(csv.str().find("active_ms,type,id,cursor_x,cursor_y,duration_ms,edge_direction") !=
            std::string::npos);
    REQUIRE(csv.str().find("1000.123,CONTROL_GROUP_JUMP,1,900,500,0.000,") != std::string::npos);
    REQUIRE(csv.str().find("1200.234,CONTROL_GROUP_RECENTER,1,901,501,0.000,") != std::string::npos);
    REQUIRE(csv.str().find("1600.456,LOCATION_HOTKEY_REPEAT,2,903,503,0.000,") != std::string::npos);
    REQUIRE(csv.str().find("2000.678,EDGE_SCROLL,,500,500,200.456,LEFT") != std::string::npos);
    input.close();
    std::filesystem::remove_all(root);
}

TEST_CASE("normal session saves one nav file while save raw adds only the unchanged raw stream") {
    const auto normalRoot = temporaryRoot("normal-storage");
    {
        smp::SessionWriter writer(normalRoot, 1000, 10, false);
        smp::RawInputEvent raw{};
        raw.sequence = 1;
        REQUIRE(writer.submitRaw(raw));
        writer.stop();
        smp::AnalysisResult compact;
        compact.mechanicalEvents.push_back(
            {100, 100.0, smp::MechanicalInputType::KeyPress, 'D', 0x20,
             smp::ModifierNone, -1, 0, 0});
        writer.setActiveTimelineAnchor({0, 1'000'000'000});
        writer.writeNavigation(compact);
    }
    std::size_t normalFiles = 0;
    for (const auto& entry : std::filesystem::directory_iterator(normalRoot)) {
        REQUIRE(entry.is_regular_file());
        REQUIRE(entry.path().extension() == ".nav");
        ++normalFiles;
    }
    REQUIRE(normalFiles == 1);

    const auto rawRoot = temporaryRoot("raw-storage");
    std::filesystem::path rawPath;
    {
        smp::SessionWriter writer(rawRoot, 1000, 10, true);
        rawPath = writer.rawPath();
        smp::RawInputEvent raw{};
        raw.sequence = 1;
        raw.timestampTicks = 100;
        raw.type = smp::RawEventType::KeyDown;
        REQUIRE(writer.submitRaw(raw));
        writer.stop();
        REQUIRE(writer.droppedEvents() == 0);
        writer.writeNavigation(smp::AnalysisResult{});
    }
    REQUIRE(std::filesystem::file_size(rawPath) > sizeof(smp::RawInputEvent));
    REQUIRE(smp::listNavSessions(rawRoot).size() == 1);
    std::size_t rawFiles = 0;
    for (const auto& entry : std::filesystem::directory_iterator(rawRoot)) {
        REQUIRE(entry.is_regular_file());
        ++rawFiles;
    }
    REQUIRE(rawFiles == 2);
    std::filesystem::remove_all(normalRoot);
    std::filesystem::remove_all(rawRoot);
}
