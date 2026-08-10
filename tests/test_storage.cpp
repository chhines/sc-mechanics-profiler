#include "test_framework.h"

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
#pragma pack(pop)

static_assert(sizeof(LegacyNavFileHeader) == 60);
static_assert(sizeof(LegacyNavRecord) == 28);

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

TEST_CASE("normalized minimap calibration and capture key persist without an absolute minimap rectangle") {
    const auto root = temporaryRoot("config-test");
    const auto path = root / "config.json";

    smp::Config config;
    config.calibratedMinimap = smp::NormalizedScreenRect{0.025694, 0.739815, 0.193750, 0.962963};
    config.calibrationCaptureKey = 0x79; // F10, proving the key is configurable
    config.save(path);

    const auto loaded = smp::Config::loadOrCreate(path);
    REQUIRE(loaded.calibratedMinimap.has_value());
    REQUIRE_NEAR(loaded.calibratedMinimap->left, 0.025694, 0.000001);
    REQUIRE_NEAR(loaded.calibratedMinimap->bottom, 0.962963, 0.000001);
    REQUIRE(loaded.calibrationCaptureKey == 0x79);
    const auto json = smp::json::parseFile(path);
    REQUIRE(json["screen_regions"]["minimap"]["left_norm"].isNumber());
    REQUIRE(json["screen"]["minimap"].isNull());
    std::filesystem::remove_all(root);
}

TEST_CASE("compact navigation binary round trips transitions recenters and metadata") {
    const auto root = temporaryRoot("nav-roundtrip");
    const auto path = root / "sample.nav";
    const auto expected = sampleAnalysis();
    smp::writeNavSession(path, expected, "sample", 10'000'000, 1'786'281'445'123);

    REQUIRE(std::filesystem::exists(path));
    REQUIRE(!std::filesystem::exists(path.string() + ".tmp"));
    REQUIRE(std::filesystem::file_size(path) == 60 + 6 * 36);
    const auto loaded = smp::readNavSession(path);
    REQUIRE(loaded.sessionId == "sample");
    REQUIRE(loaded.qpcFrequency == 10'000'000);
    REQUIRE(loaded.sessionStartUnixMs == 1'786'281'445'123);
    REQUIRE_NEAR(loaded.analysis.activeDurationSeconds, expected.activeDurationSeconds, 0.000001);
    REQUIRE_NEAR(loaded.analysis.pausedDurationSeconds, expected.pausedDurationSeconds, 0.000001);
    REQUIRE(loaded.analysis.droppedEventCount == 3);
    REQUIRE(loaded.analysis.locationRecallCount == 2);
    REQUIRE(loaded.analysis.navigationEvents.size() == 4);
    REQUIRE(loaded.analysis.recenters.size() == 2);
    REQUIRE(loaded.analysis.navigationEvents[0].type == smp::CameraNavigationType::ControlGroupJump);
    REQUIRE(loaded.analysis.navigationEvents[0].id == 1);
    REQUIRE_NEAR(loaded.analysis.navigationEvents[0].activeMs, 1000.123, 0.001);
    REQUIRE(loaded.analysis.navigationEvents[1].type == smp::CameraNavigationType::LocationHotkey);
    REQUIRE(loaded.analysis.navigationEvents[2].type == smp::CameraNavigationType::MinimapJump);
    REQUIRE(loaded.analysis.navigationEvents[2].cursorX == 373);
    REQUIRE(loaded.analysis.navigationEvents[2].cursorY == 871);
    REQUIRE(loaded.analysis.navigationEvents[3].type == smp::CameraNavigationType::EdgeScroll);
    REQUIRE(loaded.analysis.navigationEvents[3].edgeDirection == smp::EdgeDirection::Left);
    REQUIRE(loaded.analysis.navigationEvents[3].cursorX == 500);
    REQUIRE(loaded.analysis.navigationEvents[3].cursorY == 500);
    REQUIRE(loaded.analysis.navigationEvents[3].startCursorX == 242);
    REQUIRE(loaded.analysis.navigationEvents[3].startCursorY == 500);
    REQUIRE_NEAR(loaded.analysis.navigationEvents[3].durationMs, 200.456, 0.001);
    REQUIRE(loaded.analysis.recenters[0].type == smp::CameraRecenterType::ControlGroup);
    REQUIRE(loaded.analysis.recenters[1].type == smp::CameraRecenterType::LocationHotkey);
    const auto summary = smp::analysisToJson(loaded.analysis, loaded.sessionId);
    REQUIRE(summary["camera_navigation"]["control_group"]["recenters"].asInt() == 1);
    REQUIRE(summary["camera_navigation"]["location_hotkey"]["recalls"].asInt() == 2);
    REQUIRE(summary["camera_navigation"]["location_hotkey"]["repeated_recalls"].asInt() == 1);
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
    REQUIRE(loaded.analysis.navigationEvents.size() == 1);
    const auto& edge = loaded.analysis.navigationEvents.front();
    REQUIRE(edge.type == smp::CameraNavigationType::EdgeScroll);
    REQUIRE(edge.cursorX == 500);
    REQUIRE(edge.cursorY == 400);
    REQUIRE(edge.startCursorX == 500);
    REQUIRE(edge.startCursorY == 400);
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
    REQUIRE(std::filesystem::file_size(path) == 60);
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
    smp::writeNavSession(sessions / "known.nav", sampleAnalysis(), "known", 1000, 1234);
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
        writer.writeNavigation(smp::AnalysisResult{});
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
