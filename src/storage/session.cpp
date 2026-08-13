#include "storage/session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <windows.h>

namespace smp {
namespace {

#pragma pack(push, 1)
struct RawBinaryHeader {
    char magic[8]{};
    std::uint32_t schemaVersion{1};
    std::uint32_t eventSize{};
    std::uint64_t qpcFrequency{};
};

struct NavFileHeaderDisk {
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

struct NavRecordDiskV1 {
    std::uint64_t activeUs{};
    std::uint64_t durationUs{};
    std::int32_t cursorX{};
    std::int32_t cursorY{};
    std::uint8_t type{};
    std::int8_t id{-1};
    std::int8_t direction{};
    std::uint8_t reserved{};
};

struct NavRecordDiskV2 {
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

struct NavTimelineAnchorDiskV4 {
    std::uint64_t activeTimelineStartQpcTicks{};
    std::int64_t activeTimelineStartUnixNs{};
};

struct NavSectionsDiskV5 {
    std::uint16_t mechanicalRecordSize{};
    std::uint16_t reserved{};
    std::uint32_t mechanicalRecordCount{};
};

struct NavRecordDiskV4 {
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

struct MechanicalRecordDiskV5 {
    std::uint64_t activeUs{};
    std::uint64_t qpcOffsetTicks{};
    std::int32_t cursorX{};
    std::int32_t cursorY{};
    std::uint16_t virtualKey{};
    std::uint16_t scanCode{};
    std::uint16_t modifiers{};
    std::int16_t value{-1};
    std::uint8_t type{};
    std::uint8_t reserved{};
};
#pragma pack(pop)

static_assert(sizeof(RawBinaryHeader) == 24);
static_assert(sizeof(NavFileHeaderDisk) == 60);
static_assert(sizeof(NavRecordDiskV1) == 28);
static_assert(sizeof(NavRecordDiskV2) == 36);
static_assert(sizeof(NavTimelineAnchorDiskV4) == 16);
static_assert(sizeof(NavSectionsDiskV5) == 8);
static_assert(sizeof(NavRecordDiskV4) == 44);
static_assert(sizeof(MechanicalRecordDiskV5) == 34);

enum class NavRecordType : std::uint8_t {
    ControlGroupJump,
    ControlGroupRecenter,
    LocationHotkeyJump,
    LocationHotkeyRepeat,
    MinimapJump,
    EdgeScroll
};

constexpr char navMagic[4]{'S', 'C', 'N', 'V'};
constexpr std::uint16_t legacyNavFileSchemaVersion = 1;
constexpr std::uint16_t startTimestampNavFileSchemaVersion = 3;
constexpr std::uint16_t synchronizedTimelineNavFileSchemaVersion = 4;
constexpr std::uint16_t mechanicalStreamNavFileSchemaVersion = 5;
constexpr std::uint16_t hasActiveTimelineAnchorFlag = 1;

std::int64_t unixMilliseconds(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

std::string makeSessionId(std::chrono::system_clock::time_point time) {
    const std::time_t value = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    localtime_s(&local, &value);
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d_%H%M%S");
    return output.str();
}

bool sessionNameExists(const std::filesystem::path& root, const std::string& id) {
    return std::filesystem::exists(root / (id + ".nav")) ||
           std::filesystem::exists(root / (id + ".nav.tmp")) ||
           std::filesystem::exists(root / (id + ".events.bin"));
}

std::uint64_t millisecondsToMicroseconds(double milliseconds, const char* field) {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0)
        throw std::runtime_error(std::string("Invalid ") + field + " in navigation session");
    const long double microseconds = static_cast<long double>(milliseconds) * 1000.0L;
    if (microseconds > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        throw std::runtime_error(std::string(field) + " is too large for navigation session");
    return static_cast<std::uint64_t>(std::floor(microseconds + 0.5L));
}

std::uint64_t secondsToMicroseconds(double seconds, const char* field) {
    return millisecondsToMicroseconds(seconds * 1000.0, field);
}

double microsecondsToMilliseconds(std::uint64_t microseconds) {
    return static_cast<double>(microseconds) / 1000.0;
}

double microsecondsToSeconds(std::uint64_t microseconds) {
    return static_cast<double>(microseconds) / 1'000'000.0;
}

std::int8_t checkedId(int id) {
    if (id < std::numeric_limits<std::int8_t>::min() || id > std::numeric_limits<std::int8_t>::max())
        throw std::runtime_error("Navigation record id is out of range");
    return static_cast<std::int8_t>(id);
}

std::int8_t checkedDirection(EdgeDirection direction) {
    const int value = static_cast<int>(direction);
    if (value < static_cast<int>(EdgeDirection::None) || value > static_cast<int>(EdgeDirection::BottomRight))
        throw std::runtime_error("Navigation edge direction is out of range");
    return static_cast<std::int8_t>(value);
}

std::uint64_t qpcOffsetTicks(std::uint64_t timestampTicks, const QpcWallClockAnchor& anchor) {
    if (timestampTicks < anchor.qpcTicks)
        throw std::runtime_error("Navigation event precedes the active-timeline QPC anchor");
    return timestampTicks - anchor.qpcTicks;
}

std::vector<NavRecordDiskV4> makeDiskRecords(const AnalysisResult& result,
                                             const QpcWallClockAnchor& anchor) {
    std::vector<NavRecordDiskV4> records;
    records.reserve(result.navigationEvents.size() + result.recenters.size());
    for (const auto& event : result.navigationEvents) {
        NavRecordType type{};
        switch (event.type) {
        case CameraNavigationType::ControlGroupJump:
            type = NavRecordType::ControlGroupJump;
            break;
        case CameraNavigationType::LocationHotkey:
            type = NavRecordType::LocationHotkeyJump;
            break;
        case CameraNavigationType::MinimapJump:
            type = NavRecordType::MinimapJump;
            break;
        case CameraNavigationType::EdgeScroll:
            type = NavRecordType::EdgeScroll;
            break;
        }
        records.push_back({millisecondsToMicroseconds(event.activeMs, "event timestamp"),
                           millisecondsToMicroseconds(event.durationMs, "event duration"), event.cursorX,
                           event.cursorY, static_cast<std::uint8_t>(type), checkedId(event.id),
                           checkedDirection(event.edgeDirection), 0, event.startCursorX, event.startCursorY,
                           qpcOffsetTicks(event.timestampTicks, anchor)});
    }
    for (const auto& event : result.recenters) {
        const auto type = event.type == CameraRecenterType::ControlGroup ? NavRecordType::ControlGroupRecenter
                                                                         : NavRecordType::LocationHotkeyRepeat;
        records.push_back({millisecondsToMicroseconds(event.activeMs, "recenter timestamp"), 0, event.cursorX,
                           event.cursorY, static_cast<std::uint8_t>(type), checkedId(event.id), 0, 0,
                           event.cursorX, event.cursorY, qpcOffsetTicks(event.timestampTicks, anchor)});
    }
    std::stable_sort(records.begin(), records.end(),
                     [](const auto& first, const auto& second) { return first.activeUs < second.activeUs; });
    return records;
}

std::vector<MechanicalRecordDiskV5> makeMechanicalDiskRecords(
    const AnalysisResult& result, const QpcWallClockAnchor& anchor) {
    std::vector<MechanicalRecordDiskV5> records;
    records.reserve(result.mechanicalEvents.size());
    for (const auto& event : result.mechanicalEvents) {
        if (event.value < std::numeric_limits<std::int16_t>::min() ||
            event.value > std::numeric_limits<std::int16_t>::max())
            throw std::runtime_error("Mechanical input value is out of range");
        records.push_back({millisecondsToMicroseconds(event.activeMs, "mechanical event timestamp"),
                           qpcOffsetTicks(event.timestampTicks, anchor), event.cursorX, event.cursorY,
                           event.virtualKey, event.scanCode, event.modifiers,
                           static_cast<std::int16_t>(event.value), static_cast<std::uint8_t>(event.type), 0});
    }
    return records;
}

const char* navRecordTypeName(NavRecordType type) {
    switch (type) {
    case NavRecordType::ControlGroupJump:
        return "CONTROL_GROUP_JUMP";
    case NavRecordType::ControlGroupRecenter:
        return "CONTROL_GROUP_RECENTER";
    case NavRecordType::LocationHotkeyJump:
        return "LOCATION_HOTKEY";
    case NavRecordType::LocationHotkeyRepeat:
        return "LOCATION_HOTKEY_REPEAT";
    case NavRecordType::MinimapJump:
        return "MINIMAP_JUMP";
    case NavRecordType::EdgeScroll:
        return "EDGE_SCROLL";
    }
    return "UNKNOWN";
}

std::size_t navigationCount(const AnalysisResult& result, CameraNavigationType type) {
    return static_cast<std::size_t>(std::count_if(result.navigationEvents.begin(), result.navigationEvents.end(),
                                                   [type](const auto& event) { return event.type == type; }));
}

std::size_t recenterCount(const AnalysisResult& result, CameraRecenterType type) {
    return static_cast<std::size_t>(std::count_if(result.recenters.begin(), result.recenters.end(),
                                                   [type](const auto& event) { return event.type == type; }));
}

std::optional<double> percentile(std::vector<double> values, double probability) {
    if (values.empty())
        return std::nullopt;
    std::sort(values.begin(), values.end());
    const double index = probability * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(index));
    const auto upper = static_cast<std::size_t>(std::ceil(index));
    const double fraction = index - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

json::Value optionalJson(const std::optional<double>& value) {
    return value ? json::Value(*value) : json::Value(nullptr);
}

json::Value durationJson(const std::vector<double>& values) {
    return json::Value::Object{{"count", static_cast<double>(values.size())},
                               {"median", optionalJson(percentile(values, 0.5))},
                               {"p90", optionalJson(percentile(values, 0.9))}};
}

json::Value controlGroupsJson(const AnalysisResult& result) {
    std::array<std::size_t, 10> counts{};
    for (const auto& event : result.navigationEvents) {
        if (event.type == CameraNavigationType::ControlGroupJump && event.id >= 0 && event.id <= 9)
            ++counts[static_cast<std::size_t>(event.id)];
    }
    json::Value::Object object;
    for (std::size_t group = 0; group < counts.size(); ++group) {
        if (counts[group] > 0)
            object[std::to_string(group)] = static_cast<double>(counts[group]);
    }
    return object;
}

json::Value locationsJson(const AnalysisResult& result) {
    json::Value::Object object;
    for (const auto& event : result.navigationEvents) {
        if (event.type != CameraNavigationType::LocationHotkey)
            continue;
        const auto key = "F" + std::to_string(event.id);
        object[key] = object[key].asNumber() + 1.0;
    }
    return object;
}

json::Value edgeDirectionsJson(const AnalysisResult& result) {
    json::Value::Object object;
    for (const auto& event : result.navigationEvents) {
        if (event.type != CameraNavigationType::EdgeScroll)
            continue;
        const std::string key = edgeDirectionName(event.edgeDirection);
        object[key] = object[key].asNumber() + 1.0;
    }
    return object;
}

std::vector<double> edgeDurations(const AnalysisResult& result) {
    std::vector<double> values;
    for (const auto& event : result.navigationEvents) {
        if (event.type == CameraNavigationType::EdgeScroll)
            values.push_back(event.durationMs);
    }
    return values;
}

json::Value productionContextJson(const ProductionContextId& context) {
    json::Value root(json::Value::Object{{"kind", productionContextKindName(context.kind)}});
    if (context.kind == ProductionContextKind::ReplaySelection) {
        json::Value::Array tags;
        tags.reserve(context.unitTags.size());
        for (const auto tag : context.unitTags)
            tags.emplace_back(static_cast<double>(tag));
        root["unit_tags"] = std::move(tags);
    } else if (context.kind == ProductionContextKind::ControlGroup) {
        root["control_group"] = context.controlGroup;
        root["generation"] = static_cast<double>(context.assignmentGeneration);
    } else if (context.kind == ProductionContextKind::LocationHotkey) {
        root["location_hotkey"] = context.locationHotkey;
        root["generation"] = static_cast<double>(context.assignmentGeneration);
    }
    return root;
}

json::Value productionVisitsJson(const ProductionAnalysis& analysis) {
    json::Value root(json::Value::Object{{"available", analysis.visitsAvailable}});
    if (!analysis.visitsAvailable)
        root["reason"] = analysis.visitsUnavailableReason;

    json::Value::Array likelyGroups;
    likelyGroups.reserve(analysis.likelyProductionGroups.size());
    for (const auto& likely : analysis.likelyProductionGroups) {
        json::Value::Array keys;
        keys.reserve(likely.observedProductionKeys.size());
        for (const auto key : likely.observedProductionKeys)
            keys.emplace_back(static_cast<int>(key));
        likelyGroups.emplace_back(json::Value::Object{{"group", likely.group},
                                                       {"observed_virtual_keys", std::move(keys)}});
    }

    json::Value::Array visits;
    visits.reserve(analysis.productionVisits.size());
    for (const auto& visit : analysis.productionVisits) {
        json::Value::Array units;
        units.reserve(visit.producedUnits.size());
        for (const auto& unit : visit.producedUnits)
            units.emplace_back(unit);
        json::Value::Array physicalKeys;
        physicalKeys.reserve(visit.physicalProductionKeys.size());
        for (const auto key : visit.physicalProductionKeys)
            physicalKeys.emplace_back(static_cast<int>(key));
        visits.emplace_back(json::Value::Object{
            {"product_type", macroProductTypeName(visit.productType)},
            {"access_method", productionAccessMethodName(visit.accessMethod)},
            {"selection_access", productionSelectionAccessName(visit.selectionAccess)},
            {"camera_access", productionCameraAccessName(visit.cameraAccess)},
            {"camera_episode_id",
             visit.cameraEpisodeId != 0 ? json::Value(static_cast<double>(visit.cameraEpisodeId))
                                        : json::Value(nullptr)},
            {"camera_anchor_kind", productionCameraAnchorKindName(visit.cameraAnchorKind)},
            {"camera_anchor_id",
             visit.cameraAnchorId >= 0 ? json::Value(visit.cameraAnchorId) : json::Value(nullptr)},
            {"camera_anchor_qpc",
             visit.cameraEpisodeId != 0
                 ? json::Value(static_cast<double>(visit.cameraAnchorTimestampTicks))
                 : json::Value(nullptr)},
            {"start_active_ms", visit.startActiveMs},
            {"context_active_ms", visit.contextActiveMs},
            {"first_production_active_ms", visit.firstProductionActiveMs},
            {"end_active_ms", visit.endActiveMs},
            {"access_latency_ms", visit.accessLatencyMs},
            {"production_latency_ms", visit.productionLatencyMs},
            {"execution_duration_ms", visit.executionDurationMs},
            {"production_burst_span_ms", visit.productionBurstSpanMs},
            {"duration_ms", visit.durationMs},
            {"control_group", visit.controlGroup >= 0 ? json::Value(visit.controlGroup) : json::Value(nullptr)},
            {"location_hotkey",
             visit.locationHotkey >= 0 ? json::Value(visit.locationHotkey) : json::Value(nullptr)},
            {"physical_production_presses", visit.physicalProductionPresses},
            {"physical_production_keys", std::move(physicalKeys)},
            {"replay_production_commands", visit.replayProductionCommands},
            {"produced_units", std::move(units)},
            {"replay_confirmed", visit.replayConfirmed},
            {"production_context", productionContextJson(visit.productionContext)}});
    }
    root["count"] = static_cast<double>(analysis.productionVisits.size());
    root["heuristic_control_groups"] = std::move(likelyGroups);
    root["visits"] = std::move(visits);
    return root;
}

json::Value productMacroCyclesJson(const ProductMacroCycleAnalysis& analysis) {
    json::Value root(json::Value::Object{{"available", analysis.available}});
    if (!analysis.available) {
        root["reason"] = analysis.unavailableReason;
        return root;
    }
    json::Value::Array cycles;
    cycles.reserve(analysis.cycles.size());
    for (const auto& cycle : analysis.cycles) {
        json::Value::Array visitIndices;
        visitIndices.reserve(cycle.visitIndices.size());
        for (const auto index : cycle.visitIndices)
            visitIndices.emplace_back(static_cast<double>(index));
        cycles.emplace_back(json::Value::Object{{"start_active_ms", cycle.startActiveMs},
                                                {"end_active_ms", cycle.endActiveMs},
                                                {"execution_end_active_ms",
                                                 cycle.executionEndActiveMs},
                                                {"duration_ms", cycle.durationMs},
                                                {"full_span_ms", cycle.fullSpanMs},
                                                {"visit_count", static_cast<double>(cycle.visitIndices.size())},
                                                {"visit_indices", std::move(visitIndices)}});
    }
    root["count"] = static_cast<double>(analysis.cycles.size());
    root["average_duration_ms"] = optionalJson(analysis.averageDurationMs);
    root["best_duration_ms"] = optionalJson(analysis.bestDurationMs);
    root["slowest_duration_ms"] = optionalJson(analysis.slowestDurationMs);
    root["production_visit_count"] = static_cast<double>(analysis.productionVisitCount);
    root["cycles"] = std::move(cycles);
    return root;
}

json::Value replayCorrelationJson(const ReplayCorrelationDiagnostics& correlation) {
    json::Value root(json::Value::Object{{"available", correlation.available},
                                         {"parser", correlation.parser}});
    if (!correlation.available) {
        root["reason"] = correlation.unavailableReason;
        root["unmatched_production_visits"] =
            static_cast<double>(correlation.unmatchedProductionVisits);
        return root;
    }
    root["player_id"] = correlation.playerId;
    root["player_name"] = correlation.playerName;
    root["sequence_score"] = correlation.sequenceScore;
    root["runner_up_sequence_score"] = correlation.runnerUpSequenceScore;
    root["matched_control_group_events"] =
        static_cast<double>(correlation.matchedControlGroupEvents);
    root["timeline_anchors"] = static_cast<double>(correlation.timelineAnchors);
    root["matched_production_visits"] =
        static_cast<double>(correlation.matchedProductionVisits);
    root["unmatched_production_visits"] =
        static_cast<double>(correlation.unmatchedProductionVisits);
    root["replay_created_control_group_visits"] =
        static_cast<double>(correlation.replayCreatedControlGroupVisits);
    root["matched_click_visits"] = static_cast<double>(correlation.matchedClickVisits);
    root["matched_replay_production_events"] =
        static_cast<double>(correlation.matchedReplayProductionEvents);
    root["unmatched_replay_production_events"] =
        static_cast<double>(correlation.unmatchedReplayProductionEvents);
    root["extended_production_visits"] =
        static_cast<double>(correlation.extendedProductionVisits);
    root["extended_physical_production_presses"] =
        static_cast<double>(correlation.extendedPhysicalProductionPresses);
    return root;
}

json::Value macroHotkeysJson(const MacroHotkeyProfile& profile) {
    json::Value::Object bindings;
    for (const auto& binding : profile.productionCommands)
        bindings[binding.command] = binding.boundKey;
    json::Value root(json::Value::Object{{"source", profile.source},
                                         {"production_bindings", std::move(bindings)}});
    root["custom_hotkeys_enabled"] =
        profile.customHotkeysEnabled ? json::Value(*profile.customHotkeysEnabled) : json::Value(nullptr);
    return root;
}

} // namespace

SessionWriter::SessionWriter(const std::filesystem::path& sessionsRoot, std::uint64_t qpcFrequency,
                             int flushIntervalMs, bool saveRaw)
    : qpcFrequency_(qpcFrequency), flushIntervalMs_(flushIntervalMs), rawEnabled_(saveRaw) {
    std::filesystem::create_directories(sessionsRoot);
    const auto start = std::chrono::system_clock::now();
    sessionStartUnixMs_ = unixMilliseconds(start);
    const auto base = makeSessionId(start);
    sessionId_ = base;
    for (int suffix = 1; sessionNameExists(sessionsRoot, sessionId_); ++suffix)
        sessionId_ = base + "_" + std::to_string(suffix);
    navPath_ = sessionsRoot / (sessionId_ + ".nav");
    const auto temporary = std::filesystem::path(navPath_.string() + ".tmp");
    std::ofstream reservation(temporary, std::ios::binary | std::ios::trunc);
    if (!reservation)
        throw std::runtime_error("Unable to reserve navigation session file: " + temporary.string());

    if (!rawEnabled_)
        return;
    rawPath_ = sessionsRoot / (sessionId_ + ".events.bin");
    rawFile_.open(rawPath_, std::ios::binary | std::ios::trunc);
    if (!rawFile_)
        throw std::runtime_error("Unable to create raw event file: " + rawPath_.string());
    RawBinaryHeader header{};
    std::memcpy(header.magic, "SMPRAW1", 7);
    header.eventSize = sizeof(RawInputEvent);
    header.qpcFrequency = qpcFrequency_;
    rawFile_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    thread_ = std::thread(&SessionWriter::run, this);
}

SessionWriter::~SessionWriter() {
    stop();
}

bool SessionWriter::submitRaw(const RawInputEvent& event) noexcept {
    if (!rawEnabled_)
        return true;
    if (failed_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (rawQueue_.tryPush(event))
        return true;
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void SessionWriter::setActiveTimelineAnchor(QpcWallClockAnchor anchor) noexcept {
    if (!activeTimelineAnchor_)
        activeTimelineAnchor_ = anchor;
}

void SessionWriter::stop() {
    if (!thread_.joinable())
        return;
    stopping_.store(true, std::memory_order_release);
    thread_.join();
    rawFile_.flush();
}

void SessionWriter::run() {
    auto lastFlush = std::chrono::steady_clock::now();
    while (!stopping_.load(std::memory_order_acquire) || !rawQueue_.empty()) {
        bool wrote = false;
        RawInputEvent raw{};
        while (rawQueue_.tryPop(raw)) {
            rawFile_.write(reinterpret_cast<const char*>(&raw), sizeof(raw));
            wrote = true;
        }
        if (!rawFile_)
            failed_.store(true, std::memory_order_release);
        const auto now = std::chrono::steady_clock::now();
        if (now - lastFlush >= std::chrono::milliseconds(flushIntervalMs_)) {
            rawFile_.flush();
            lastFlush = now;
        }
        if (!wrote)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::filesystem::path SessionWriter::writeNavigation(const AnalysisResult& result) {
    return writeNavSession(navPath_, result, sessionId_, qpcFrequency_, sessionStartUnixMs_,
                           activeTimelineAnchor_);
}

std::filesystem::path writeNavSession(const std::filesystem::path& navPath, const AnalysisResult& result,
                                      const std::string&, std::uint64_t qpcFrequency,
                                      std::int64_t sessionStartUnixMs,
                                      std::optional<QpcWallClockAnchor> activeTimelineAnchor) {
    std::filesystem::create_directories(navPath.parent_path());
    const bool hasRecords = !result.navigationEvents.empty() || !result.recenters.empty() ||
                            !result.mechanicalEvents.empty();
    if (hasRecords && !activeTimelineAnchor)
        throw std::runtime_error("Navigation events require an active-timeline QPC anchor");
    const auto records = activeTimelineAnchor ? makeDiskRecords(result, *activeTimelineAnchor)
                                              : std::vector<NavRecordDiskV4>{};
    const auto mechanicalRecords =
        activeTimelineAnchor ? makeMechanicalDiskRecords(result, *activeTimelineAnchor)
                             : std::vector<MechanicalRecordDiskV5>{};
    if (records.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Too many records for navigation session");
    if (mechanicalRecords.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Too many mechanical records for navigation session");

    NavFileHeaderDisk header{};
    std::memcpy(header.magic, navMagic, sizeof(navMagic));
    header.schemaVersion = navFileSchemaVersion;
    header.headerSize = sizeof(header) + sizeof(NavTimelineAnchorDiskV4) + sizeof(NavSectionsDiskV5);
    header.recordSize = sizeof(NavRecordDiskV4);
    if (activeTimelineAnchor)
        header.flags |= hasActiveTimelineAnchorFlag;
    header.qpcFrequency = qpcFrequency;
    header.sessionStartUnixMs = sessionStartUnixMs;
    header.activeDurationUs = secondsToMicroseconds(result.activeDurationSeconds, "active duration");
    header.pausedDurationUs = secondsToMicroseconds(result.pausedDurationSeconds, "paused duration");
    header.droppedEventCount = result.droppedEventCount;
    header.recordCount = static_cast<std::uint32_t>(records.size());
    NavTimelineAnchorDiskV4 timelineAnchor{};
    if (activeTimelineAnchor) {
        timelineAnchor.activeTimelineStartQpcTicks = activeTimelineAnchor->qpcTicks;
        timelineAnchor.activeTimelineStartUnixNs = activeTimelineAnchor->unixNanoseconds;
    }
    NavSectionsDiskV5 sections{};
    sections.mechanicalRecordSize = sizeof(MechanicalRecordDiskV5);
    sections.mechanicalRecordCount = static_cast<std::uint32_t>(mechanicalRecords.size());

    const auto temporary = std::filesystem::path(navPath.string() + ".tmp");
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Unable to create temporary navigation session: " + temporary.string());
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(reinterpret_cast<const char*>(&timelineAnchor), sizeof(timelineAnchor));
        output.write(reinterpret_cast<const char*>(&sections), sizeof(sections));
        if (!records.empty())
            output.write(reinterpret_cast<const char*>(records.data()),
                         static_cast<std::streamsize>(records.size() * sizeof(NavRecordDiskV4)));
        if (!mechanicalRecords.empty())
            output.write(reinterpret_cast<const char*>(mechanicalRecords.data()),
                         static_cast<std::streamsize>(mechanicalRecords.size() *
                                                      sizeof(MechanicalRecordDiskV5)));
        output.flush();
        if (!output)
            throw std::runtime_error("Unable to write navigation session: " + temporary.string());
        output.close();
        std::error_code renameError;
        std::filesystem::rename(temporary, navPath, renameError);
        if (renameError)
            throw std::runtime_error("Unable to finalize navigation session: " + renameError.message());
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    return navPath;
}

NavSession readNavSession(const std::filesystem::path& navPath) {
    std::ifstream input(navPath, std::ios::binary);
    if (!input)
        throw std::runtime_error("Unable to open navigation session: " + navPath.string());
    NavFileHeaderDisk header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (input.gcount() != sizeof(header))
        throw std::runtime_error("Navigation session header is truncated: " + navPath.string());
    if (std::memcmp(header.magic, navMagic, sizeof(navMagic)) != 0)
        throw std::runtime_error("Invalid navigation session magic; expected SCNV: " + navPath.string());

    // TODO: Remove compatibility branches for reading pre-v5 .nav files once they are no longer needed.
    // All newly written files use schema v5. The v5 format still reuses the v4 timeline-anchor
    // and camera-record layouts.
    if (header.schemaVersion < legacyNavFileSchemaVersion || header.schemaVersion > navFileSchemaVersion)
        throw std::runtime_error("Unsupported navigation session schema version " +
                                 std::to_string(header.schemaVersion));
    const std::uint16_t expectedHeaderSize =
        header.schemaVersion >= mechanicalStreamNavFileSchemaVersion
            ? sizeof(NavFileHeaderDisk) + sizeof(NavTimelineAnchorDiskV4) + sizeof(NavSectionsDiskV5)
        : header.schemaVersion >= synchronizedTimelineNavFileSchemaVersion
            ? sizeof(NavFileHeaderDisk) + sizeof(NavTimelineAnchorDiskV4)
            : sizeof(NavFileHeaderDisk);
    const std::uint16_t expectedRecordSize =
        header.schemaVersion == legacyNavFileSchemaVersion
            ? sizeof(NavRecordDiskV1)
            : (header.schemaVersion >= synchronizedTimelineNavFileSchemaVersion ? sizeof(NavRecordDiskV4)
                                                                                 : sizeof(NavRecordDiskV2));
    if (header.headerSize != expectedHeaderSize || header.recordSize != expectedRecordSize)
        throw std::runtime_error("Unsupported navigation session record layout");

    std::optional<QpcWallClockAnchor> activeTimelineAnchor;
    if (header.schemaVersion >= synchronizedTimelineNavFileSchemaVersion) {
        NavTimelineAnchorDiskV4 timelineAnchor{};
        input.read(reinterpret_cast<char*>(&timelineAnchor), sizeof(timelineAnchor));
        if (!input)
            throw std::runtime_error("Navigation session timeline anchor is truncated");
        if ((header.flags & hasActiveTimelineAnchorFlag) != 0) {
            if (header.qpcFrequency == 0)
                throw std::runtime_error("Navigation session timeline anchor has no QPC frequency");
            activeTimelineAnchor =
                QpcWallClockAnchor{timelineAnchor.activeTimelineStartQpcTicks,
                                   timelineAnchor.activeTimelineStartUnixNs};
        }
    }

    std::uint32_t mechanicalRecordCount{};
    std::uint16_t mechanicalRecordSize{};
    if (header.schemaVersion >= mechanicalStreamNavFileSchemaVersion) {
        NavSectionsDiskV5 sections{};
        input.read(reinterpret_cast<char*>(&sections), sizeof(sections));
        if (!input)
            throw std::runtime_error("Navigation session section table is truncated");
        mechanicalRecordCount = sections.mechanicalRecordCount;
        mechanicalRecordSize = sections.mechanicalRecordSize;
        if (mechanicalRecordSize != sizeof(MechanicalRecordDiskV5))
            throw std::runtime_error("Unsupported mechanical input record layout");
    }
    if (header.schemaVersion >= synchronizedTimelineNavFileSchemaVersion && !activeTimelineAnchor &&
        (header.recordCount != 0 || mechanicalRecordCount != 0))
        throw std::runtime_error("Navigation session records have no active-timeline QPC anchor");

    const std::uintmax_t expectedSize = header.headerSize +
                                        static_cast<std::uintmax_t>(header.recordCount) * header.recordSize +
                                        static_cast<std::uintmax_t>(mechanicalRecordCount) * mechanicalRecordSize;
    if (std::filesystem::file_size(navPath) != expectedSize)
        throw std::runtime_error("Navigation session is truncated or has an invalid record count");

    NavSession session;
    session.sessionId = navPath.stem().string();
    session.qpcFrequency = header.qpcFrequency;
    session.sessionStartUnixMs = header.sessionStartUnixMs;
    session.activeTimelineAnchor = activeTimelineAnchor;
    session.analysis.activeDurationSeconds = microsecondsToSeconds(header.activeDurationUs);
    session.analysis.pausedDurationSeconds = microsecondsToSeconds(header.pausedDurationUs);
    session.analysis.droppedEventCount = header.droppedEventCount;
    session.analysis.navigationEvents.reserve(header.recordCount);
    session.analysis.recenters.reserve(header.recordCount);
    session.analysis.mechanicalEvents.reserve(mechanicalRecordCount);

    for (std::uint32_t index = 0; index < header.recordCount; ++index) {
        NavRecordDiskV4 record{};
        if (header.schemaVersion == legacyNavFileSchemaVersion) {
            NavRecordDiskV1 legacy{};
            input.read(reinterpret_cast<char*>(&legacy), sizeof(legacy));
            record = {legacy.activeUs, legacy.durationUs, legacy.cursorX, legacy.cursorY, legacy.type,
                      legacy.id, legacy.direction, legacy.reserved, legacy.cursorX, legacy.cursorY, 0};
        } else if (header.schemaVersion < synchronizedTimelineNavFileSchemaVersion) {
            NavRecordDiskV2 legacy{};
            input.read(reinterpret_cast<char*>(&legacy), sizeof(legacy));
            record = {legacy.activeUs, legacy.durationUs, legacy.cursorX, legacy.cursorY, legacy.type,
                      legacy.id, legacy.direction, legacy.reserved, legacy.startCursorX,
                      legacy.startCursorY, 0};
        } else {
            input.read(reinterpret_cast<char*>(&record), sizeof(record));
        }
        if (!input)
            throw std::runtime_error("Navigation session record is truncated");
        if (record.type > static_cast<std::uint8_t>(NavRecordType::EdgeScroll))
            throw std::runtime_error("Navigation session contains an unknown record type");
        if (record.direction < static_cast<std::int8_t>(EdgeDirection::None) ||
            record.direction > static_cast<std::int8_t>(EdgeDirection::BottomRight))
            throw std::runtime_error("Navigation session contains an invalid edge direction");

        const auto type = static_cast<NavRecordType>(record.type);
        std::uint64_t principalActiveUs = record.activeUs;
        if (type == NavRecordType::EdgeScroll && header.schemaVersion < startTimestampNavFileSchemaVersion)
            principalActiveUs = record.activeUs >= record.durationUs ? record.activeUs - record.durationUs : 0;
        const double activeMs = microsecondsToMilliseconds(principalActiveUs);
        const double durationMs = microsecondsToMilliseconds(record.durationUs);
        const auto direction = static_cast<EdgeDirection>(record.direction);
        std::uint64_t timestampTicks{};
        if (header.schemaVersion >= synchronizedTimelineNavFileSchemaVersion) {
            if (!activeTimelineAnchor ||
                record.qpcOffsetTicks > std::numeric_limits<std::uint64_t>::max() - activeTimelineAnchor->qpcTicks)
                throw std::runtime_error("Navigation session contains an invalid QPC offset");
            timestampTicks = activeTimelineAnchor->qpcTicks + record.qpcOffsetTicks;
        } else if (header.qpcFrequency != 0) {
            timestampTicks = static_cast<std::uint64_t>(
                std::llround(activeMs * static_cast<double>(header.qpcFrequency) / 1000.0));
        }
        switch (type) {
        case NavRecordType::ControlGroupJump:
            session.analysis.navigationEvents.push_back(
                {timestampTicks, activeMs, CameraNavigationType::ControlGroupJump, record.id, record.cursorX,
                 record.cursorY, durationMs, direction, record.startCursorX, record.startCursorY});
            break;
        case NavRecordType::ControlGroupRecenter:
            session.analysis.recenters.push_back({timestampTicks, activeMs, CameraRecenterType::ControlGroup,
                                                  record.id, record.cursorX, record.cursorY});
            break;
        case NavRecordType::LocationHotkeyJump:
            ++session.analysis.locationRecallCount;
            session.analysis.navigationEvents.push_back(
                {timestampTicks, activeMs, CameraNavigationType::LocationHotkey, record.id, record.cursorX,
                 record.cursorY, durationMs, direction, record.startCursorX, record.startCursorY});
            break;
        case NavRecordType::LocationHotkeyRepeat:
            ++session.analysis.locationRecallCount;
            session.analysis.recenters.push_back({timestampTicks, activeMs, CameraRecenterType::LocationHotkey,
                                                  record.id, record.cursorX, record.cursorY});
            break;
        case NavRecordType::MinimapJump:
            session.analysis.navigationEvents.push_back(
                {timestampTicks, activeMs, CameraNavigationType::MinimapJump, record.id, record.cursorX,
                 record.cursorY, durationMs, direction, record.startCursorX, record.startCursorY});
            break;
        case NavRecordType::EdgeScroll:
            session.analysis.navigationEvents.push_back(
                {timestampTicks, activeMs, CameraNavigationType::EdgeScroll, record.id, record.cursorX,
                 record.cursorY, durationMs, direction, record.startCursorX, record.startCursorY});
            break;
        }
    }

    for (std::uint32_t index = 0; index < mechanicalRecordCount; ++index) {
        MechanicalRecordDiskV5 record{};
        input.read(reinterpret_cast<char*>(&record), sizeof(record));
        if (!input)
            throw std::runtime_error("Mechanical input record is truncated");
        if (record.type > static_cast<std::uint8_t>(MechanicalInputType::MouseWheel))
            throw std::runtime_error("Navigation session contains an unknown mechanical input type");
        if (!activeTimelineAnchor ||
            record.qpcOffsetTicks > std::numeric_limits<std::uint64_t>::max() - activeTimelineAnchor->qpcTicks)
            throw std::runtime_error("Mechanical input record contains an invalid QPC offset");
        const auto timestampTicks = activeTimelineAnchor->qpcTicks + record.qpcOffsetTicks;
        session.analysis.mechanicalEvents.push_back(
            {timestampTicks, microsecondsToMilliseconds(record.activeUs),
             static_cast<MechanicalInputType>(record.type), record.virtualKey, record.scanCode,
             record.modifiers, record.value, record.cursorX, record.cursorY});
    }
    return session;
}

std::optional<std::int64_t> qpcTimestampToUnixNanoseconds(const NavSession& session,
                                                         std::uint64_t timestampTicks) noexcept {
    if (!session.activeTimelineAnchor || session.qpcFrequency == 0)
        return std::nullopt;
    const auto delta = qpcDeltaNanoseconds(session.activeTimelineAnchor->qpcTicks, timestampTicks,
                                           session.qpcFrequency);
    if (!delta)
        return std::nullopt;
    const auto anchorUnixNs = session.activeTimelineAnchor->unixNanoseconds;
    if (*delta > 0 && anchorUnixNs > std::numeric_limits<std::int64_t>::max() - *delta)
        return std::nullopt;
    if (*delta < 0 && anchorUnixNs < std::numeric_limits<std::int64_t>::min() - *delta)
        return std::nullopt;
    return anchorUnixNs + *delta;
}

std::vector<std::filesystem::path> listNavSessions(const std::filesystem::path& sessionsRoot) {
    std::vector<std::pair<std::int64_t, std::filesystem::path>> discovered;
    if (!std::filesystem::exists(sessionsRoot))
        return {};
    for (const auto& entry : std::filesystem::directory_iterator(sessionsRoot)) {
        if (entry.is_regular_file() && entry.path().extension() == ".nav") {
            const auto session = readNavSession(entry.path());
            discovered.emplace_back(session.sessionStartUnixMs, entry.path());
        }
    }
    std::sort(discovered.begin(), discovered.end(), [](const auto& first, const auto& second) {
        return first.first != second.first ? first.first < second.first : first.second < second.second;
    });
    std::vector<std::filesystem::path> result;
    result.reserve(discovered.size());
    for (auto& [start, path] : discovered) {
        (void)start;
        result.push_back(std::move(path));
    }
    return result;
}

std::filesystem::path resolveNavSession(const std::filesystem::path& sessionsRoot, const std::string& selector) {
    if (selector == "latest") {
        const auto sessions = listNavSessions(sessionsRoot);
        if (sessions.empty())
            throw std::runtime_error("No recorded .nav sessions were found");
        return sessions.back();
    }
    std::filesystem::path filename(selector);
    if (filename.extension() != ".nav")
        filename += ".nav";
    const auto path = sessionsRoot / filename;
    if (!std::filesystem::is_regular_file(path))
        throw std::runtime_error("Session not found: " + selector);
    return path;
}

json::Value analysisToJson(const AnalysisResult& result, const std::string& sessionId) {
    ProductionAnalysis production;
    production.visitsUnavailableReason = "Production-visit analysis was not persisted for this session";
    production.workerMacroCycles.productType = MacroProductType::Worker;
    production.workerMacroCycles.unavailableReason = "Replay correlation was not persisted for this session";
    production.armyMacroCycles.productType = MacroProductType::Army;
    production.armyMacroCycles.unavailableReason = "Replay correlation was not persisted for this session";
    production.replayCorrelation.unavailableReason = "Replay correlation was not persisted for this session";
    MacroHotkeyProfile macroHotkeys;
    return analysisToJson(result, sessionId, production, macroHotkeys);
}

json::Value analysisToJson(const AnalysisResult& result, const std::string& sessionId,
                           const ProductionAnalysis& production,
                           const MacroHotkeyProfile& macroHotkeys) {
    const auto controlGroupTransitions = navigationCount(result, CameraNavigationType::ControlGroupJump);
    const auto locationTransitions = navigationCount(result, CameraNavigationType::LocationHotkey);
    const auto minimapTransitions = navigationCount(result, CameraNavigationType::MinimapJump);
    const auto edgeEpisodes = navigationCount(result, CameraNavigationType::EdgeScroll);
    const auto total = result.navigationEvents.size();
    const double minutes = result.activeDurationSeconds / 60.0;

    json::Value root(json::Value::Object{});
    root["schema_version"] = 4;
    root["analysis_version"] = "camera-nav-production-macro-3";
    root["session"] = json::Value::Object{{"id", sessionId},
                                          {"active_duration_seconds", result.activeDurationSeconds},
                                          {"paused_duration_seconds", result.pausedDurationSeconds},
                                          {"dropped_event_count", static_cast<double>(result.droppedEventCount)}};
    root["camera_navigation"] = json::Value::Object{
        {"total_transitions", static_cast<double>(total)},
        {"transitions_per_minute", minutes > 0.0 ? static_cast<double>(total) / minutes : 0.0},
        {"control_group",
         json::Value::Object{{"transitions", static_cast<double>(controlGroupTransitions)},
                             {"recenters", static_cast<double>(recenterCount(result, CameraRecenterType::ControlGroup))},
                             {"by_group", controlGroupsJson(result)}}},
        {"location_hotkey",
         json::Value::Object{
             {"recalls", static_cast<double>(result.locationRecallCount)},
             {"transitions", static_cast<double>(locationTransitions)},
             {"repeated_recalls", static_cast<double>(recenterCount(result, CameraRecenterType::LocationHotkey))},
             {"by_location", locationsJson(result)}}},
        {"minimap", json::Value::Object{{"transitions", static_cast<double>(minimapTransitions)}}},
        {"edge_scroll", json::Value::Object{{"episodes", static_cast<double>(edgeEpisodes)},
                                             {"duration_ms", durationJson(edgeDurations(result))},
                                             {"by_direction", edgeDirectionsJson(result)}}}};
    root["production_visits"] = productionVisitsJson(production);
    root["worker_macro_cycles"] = productMacroCyclesJson(production.workerMacroCycles);
    root["army_macro_cycles"] = productMacroCyclesJson(production.armyMacroCycles);
    root["macro_cycle_diagnostics"] = json::Value::Object{
        {"worker_repeated_context_splits",
         static_cast<double>(production.workerMacroCycles.repeatedContextSplits)},
        {"army_repeated_context_splits",
         static_cast<double>(production.armyMacroCycles.repeatedContextSplits)},
        {"worker_assignment_interruption_splits",
         static_cast<double>(production.workerMacroCycles.assignmentInterruptionSplits)},
        {"army_assignment_interruption_splits",
         static_cast<double>(production.armyMacroCycles.assignmentInterruptionSplits)}};
    root["replay_correlation"] = replayCorrelationJson(production.replayCorrelation);
    root["macro_hotkeys"] = macroHotkeysJson(macroHotkeys);
    return root;
}

std::filesystem::path writeAnalysisJson(const std::filesystem::path& navPath,
                                        const json::Value& analysis) {
    auto destination = navPath;
    destination.replace_extension(".json");
    const auto temporary = std::filesystem::path(destination.string() + ".tmp");
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Unable to create temporary analysis JSON: " + temporary.string());
        output << json::stringify(analysis);
        output.flush();
        if (!output)
            throw std::runtime_error("Unable to write analysis JSON: " + temporary.string());
        output.close();
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Unable to finalize analysis JSON: " +
                                     std::system_category().message(static_cast<int>(GetLastError())));
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    return destination;
}

std::filesystem::path exportSessionCsv(const std::filesystem::path& sessionsRoot,
                                       const std::filesystem::path& exportRoot, const std::string& selector) {
    const auto navPath = resolveNavSession(sessionsRoot, selector);
    const auto session = readNavSession(navPath);
    const auto records =
        makeDiskRecords(session.analysis, session.activeTimelineAnchor.value_or(QpcWallClockAnchor{}));
    std::filesystem::create_directories(exportRoot);
    const auto destination =
        exportRoot / ("Starcraft Mechanics Profiler_" + session.sessionId + ".csv");
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("Unable to create CSV export: " + destination.string());
    output << "active_ms,type,id,cursor_x,cursor_y,duration_ms,edge_direction\n";
    output << std::fixed << std::setprecision(3);
    for (const auto& record : records) {
        const auto type = static_cast<NavRecordType>(record.type);
        output << microsecondsToMilliseconds(record.activeUs) << ',' << navRecordTypeName(type) << ',';
        if (record.id >= 0)
            output << static_cast<int>(record.id);
        output << ',' << record.cursorX << ',' << record.cursorY << ','
               << microsecondsToMilliseconds(record.durationUs) << ',';
        if (type == NavRecordType::EdgeScroll)
            output << edgeDirectionName(static_cast<EdgeDirection>(record.direction));
        output << '\n';
    }
    output.flush();
    if (!output)
        throw std::runtime_error("Unable to write CSV export: " + destination.string());
    return destination;
}

} // namespace smp
