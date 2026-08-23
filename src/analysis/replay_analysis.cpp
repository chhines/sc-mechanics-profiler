#include "analysis/replay_analysis.h"

#include "platform/resource_ids.h"
#include "util/json.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace smp {
namespace {

constexpr std::size_t maximumAlignedControlGroupEvents = 4000;
constexpr double minimumPlayerSequenceScore = 0.65;
constexpr double minimumPlayerScoreLead = 0.10;
constexpr int boxSelectionMinimumDragPixels = 4;
constexpr double replayControlGroupEditMatchWindowMs = 300.0;
constexpr const wchar_t* bundledParserFilename = L"screp-v1.13.3.exe";

struct SequenceEvent {
    int group{};
    std::size_t sourceIndex{};
};

struct TimelineAnchor {
    std::int64_t replayFrame{};
    double liveActiveMs{};
};

struct MappedProductionEvent {
    const ReplayProductionEvent* event{};
    double activeMs{};
    bool used{};
};

struct MappedSelectionEvent {
    const ReplaySelectionEvent* event{};
    double activeMs{};
    bool used{};
};

struct ReplayPosition {
    std::int64_t frame{};
    std::size_t commandIndex{};
};

enum class CorrelationCandidateKind {
    ControlGroup,
    Click
};

struct CorrelationCandidate {
    CorrelationCandidateKind kind{CorrelationCandidateKind::ControlGroup};
    ProductionVisit visit;
    ReplayPosition context;
    std::size_t physicalContextEventIndex{};
    std::size_t shortWindowPhysicalPresses{};
    std::uint64_t shortWindowEndTimestampTicks{};
    std::optional<std::size_t> existingVisitIndex;
    bool replayCreatedControlGroup{};
};

struct ClickCandidate {
    std::size_t clickMechanicalEventIndex{};
    double clickActiveMs{};
    std::uint64_t clickTimestampTicks{};
    double firstPressActiveMs{};
    std::uint64_t firstPressTimestampTicks{};
    double finalPressActiveMs{};
    std::uint64_t finalPressTimestampTicks{};
    int clickX{};
    int clickY{};
    ProductionSelectionAccess selectionAccess{ProductionSelectionAccess::DirectClick};
    int physicalPresses{};
    std::vector<std::uint16_t> physicalKeys;
};

class ScopedPathRemoval {
  public:
    explicit ScopedPathRemoval(std::filesystem::path path) : path_(std::move(path)) {}
    ~ScopedPathRemoval() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

  private:
    std::filesystem::path path_;
};

std::optional<double> qpcElapsedMs(std::uint64_t start, std::uint64_t end,
                                   std::uint64_t frequency) noexcept {
    if (frequency == 0 || end < start)
        return std::nullopt;
    return static_cast<double>(static_cast<long double>(end - start) * 1000.0L /
                               static_cast<long double>(frequency));
}

ReplayPosition positionOf(const ReplayControlGroupEvent& event) noexcept {
    return {event.replayFrame, event.commandIndex};
}

ReplayPosition positionOf(const ReplayControlGroupEditEvent& event) noexcept {
    return {event.replayFrame, event.commandIndex};
}

ReplayPosition positionOf(const ReplaySelectionEvent& event) noexcept {
    return {event.replayFrame, event.commandIndex};
}

ReplayPosition positionOf(const ReplayProductionEvent& event) noexcept {
    return {event.replayFrame, event.commandIndex};
}

ReplayPosition positionOf(const ReplayCommandTargetEvent& event) noexcept {
    return {event.replayFrame, event.commandIndex};
}

ReplayPosition positionOf(const ReplayBuildEvent& event) noexcept {
    return {event.replayFrame, event.commandIndex};
}

bool positionLess(ReplayPosition first, ReplayPosition second) noexcept {
    return first.frame < second.frame ||
           (first.frame == second.frame && first.commandIndex < second.commandIndex);
}

bool positionLessOrEqual(ReplayPosition first, ReplayPosition second) noexcept {
    return !positionLess(second, first);
}

std::string normalizedUnitName(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char character : value) {
        if (std::isalnum(character))
            normalized.push_back(static_cast<char>(std::toupper(character)));
    }
    return normalized;
}

std::string canonicalUnitName(std::string value) {
    static const std::unordered_map<std::string, std::string> aliases{
        {"TEMPLAR", "HIGHTEMPLAR"},
        {"DTEMPLAR", "DARKTEMPLAR"},
        {"TANK", "SIEGETANK"},
        {"VESSEL", "SCIENCEVESSEL"},
        {"BCRUISER", "BATTLECRUISER"},
        {"FRIGATE", "VALKYRIE"},
        {"MUTALID", "MUTALISK"},
        {"AVENGER", "SCOURGE"},
        {"INFESTED", "INFESTEDTERRAN"},
        {"SIEGETANKTANKMODE", "SIEGETANK"},
        {"SIEGETANKSIEGEMODE", "SIEGETANK"},
    };
    if (const auto found = aliases.find(value); found != aliases.end())
        return found->second;
    return value;
}

std::string productionCommandUnit(std::string_view command) {
    constexpr std::array<std::string_view, 3> prefixes{
        "STR_MAKE_P_", "STR_MAKE_T_", "STR_MAKE_Z_"};
    for (const auto prefix : prefixes) {
        if (command.starts_with(prefix))
            return canonicalUnitName(normalizedUnitName(command.substr(prefix.size())));
    }
    return {};
}

bool isStandaloneModifier(std::uint16_t virtualKey) noexcept {
    return virtualKey == VK_CONTROL || virtualKey == VK_LCONTROL || virtualKey == VK_RCONTROL ||
           virtualKey == VK_SHIFT || virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT ||
           virtualKey == VK_MENU || virtualKey == VK_LMENU || virtualKey == VK_RMENU;
}

bool isProductionPress(const MechanicalInputEvent& event, const MacroHotkeyProfile& hotkeys) {
    return event.type == MechanicalInputType::KeyPress &&
           (event.modifiers & (ModifierCtrl | ModifierShift | ModifierAlt)) == 0 &&
           !isStandaloneModifier(event.virtualKey) &&
           !hotkeys.compatibleProductionCommands(event.virtualKey).empty();
}

bool isHardProductionContextBoundary(MechanicalInputType type) noexcept {
    return type == MechanicalInputType::ControlGroupSelect ||
           type == MechanicalInputType::ControlGroupAssign ||
           type == MechanicalInputType::ControlGroupAdd ||
           type == MechanicalInputType::LocationRecall ||
           type == MechanicalInputType::LocationAssign ||
           type == MechanicalInputType::MouseLeftDown;
}

bool replaySemanticsAllowPhysicalKey(
    std::uint16_t key, const std::vector<const ReplayProductionEvent*>& replayEvents,
    const MacroHotkeyProfile& hotkeys) {
    return std::any_of(replayEvents.begin(), replayEvents.end(),
                       [&](const ReplayProductionEvent* replayEvent) {
                           return replayProductionCompatibleWithPhysicalKey(*replayEvent, key,
                                                                            hotkeys);
                       });
}

std::vector<std::size_t> collectConfirmedPhysicalBurst(
    const AnalysisResult& result, const MacroHotkeyProfile& hotkeys,
    std::uint64_t qpcFrequency, std::size_t contextEventIndex,
    const std::vector<const ReplayProductionEvent*>& confirmingReplayEvents) {
    std::vector<std::size_t> physicalPresses;
    if (contextEventIndex >= result.mechanicalEvents.size() || qpcFrequency == 0 ||
        confirmingReplayEvents.empty())
        return physicalPresses;

    for (std::size_t index = contextEventIndex + 1;
         index < result.mechanicalEvents.size(); ++index) {
        const auto& event = result.mechanicalEvents[index];
        if (isHardProductionContextBoundary(event.type))
            break;
        if (event.type != MechanicalInputType::KeyPress)
            continue;
        if (isStandaloneModifier(event.virtualKey))
            continue;
        if (!isProductionPress(event, hotkeys) ||
            !replaySemanticsAllowPhysicalKey(event.virtualKey, confirmingReplayEvents, hotkeys))
            break;

        if (!physicalPresses.empty()) {
            const auto& previous = result.mechanicalEvents[physicalPresses.back()];
            const auto realGap = qpcElapsedMs(previous.timestampTicks, event.timestampTicks,
                                              qpcFrequency);
            const double activeGap = event.activeMs - previous.activeMs;
            if (!realGap || activeGap < 0.0 ||
                *realGap > productionBurstContinuationGapMs ||
                activeGap > productionBurstContinuationGapMs ||
                *realGap - activeGap > qpcActivePauseToleranceMs)
                break;
        }
        physicalPresses.push_back(index);
    }
    return physicalPresses;
}

void applyConfirmedPhysicalBurst(ProductionVisit& visit, const AnalysisResult& result,
                                 const std::vector<std::size_t>& physicalPresses,
                                 std::uint64_t qpcFrequency) {
    if (physicalPresses.empty())
        return;
    visit.physicalProductionKeys.clear();
    visit.physicalProductionKeys.reserve(physicalPresses.size());
    for (const auto index : physicalPresses)
        visit.physicalProductionKeys.push_back(result.mechanicalEvents[index].virtualKey);
    visit.physicalProductionPresses = static_cast<int>(physicalPresses.size());
    const auto& firstPress = result.mechanicalEvents[physicalPresses.front()];
    const auto& finalPress = result.mechanicalEvents[physicalPresses.back()];
    visit.firstProductionActiveMs = firstPress.activeMs;
    visit.firstProductionTimestampTicks = firstPress.timestampTicks;
    visit.endActiveMs = finalPress.activeMs;
    visit.endTimestampTicks = finalPress.timestampTicks;
    refreshProductionVisitTiming(visit, qpcFrequency);
}

std::vector<std::pair<std::size_t, std::size_t>>
longestCommonSubsequence(const std::vector<SequenceEvent>& first,
                         const std::vector<SequenceEvent>& second) {
    const std::size_t rows = first.size() + 1;
    const std::size_t columns = second.size() + 1;
    if (rows == 1 || columns == 1)
        return {};

    std::vector<std::uint16_t> previous(columns);
    std::vector<std::uint16_t> current(columns);
    std::vector<std::uint8_t> direction(rows * columns);
    for (std::size_t i = 1; i < rows; ++i) {
        for (std::size_t j = 1; j < columns; ++j) {
            const auto cell = i * columns + j;
            if (first[i - 1].group == second[j - 1].group) {
                current[j] = static_cast<std::uint16_t>(previous[j - 1] + 1);
                direction[cell] = 1;
            } else if (previous[j] >= current[j - 1]) {
                current[j] = previous[j];
                direction[cell] = 2;
            } else {
                current[j] = current[j - 1];
                direction[cell] = 3;
            }
        }
        std::swap(previous, current);
        std::fill(current.begin(), current.end(), std::uint16_t{0});
    }

    std::vector<std::pair<std::size_t, std::size_t>> matches;
    std::size_t i = first.size();
    std::size_t j = second.size();
    while (i > 0 && j > 0) {
        switch (direction[i * columns + j]) {
        case 1:
            matches.emplace_back(first[i - 1].sourceIndex, second[j - 1].sourceIndex);
            --i;
            --j;
            break;
        case 2:
            --i;
            break;
        default:
            --j;
            break;
        }
    }
    std::reverse(matches.begin(), matches.end());
    return matches;
}

double sequenceScore(std::size_t matches, std::size_t liveCount, std::size_t replayCount) noexcept {
    if (matches == 0 || liveCount == 0 || replayCount == 0)
        return 0.0;
    const double liveCoverage = static_cast<double>(matches) / static_cast<double>(liveCount);
    const double replayCoverage = static_cast<double>(matches) / static_cast<double>(replayCount);
    return 2.0 * liveCoverage * replayCoverage / (liveCoverage + replayCoverage);
}

std::vector<SequenceEvent> liveControlGroupSequence(const std::vector<MechanicalInputEvent>& events) {
    std::vector<SequenceEvent> sequence;
    sequence.reserve(std::min(events.size(), maximumAlignedControlGroupEvents));
    for (std::size_t index = 0; index < events.size() && sequence.size() < maximumAlignedControlGroupEvents;
         ++index) {
        const auto& event = events[index];
        if (event.type == MechanicalInputType::ControlGroupSelect && event.value >= 0 && event.value <= 9)
            sequence.push_back({event.value, index});
    }
    return sequence;
}

std::vector<SequenceEvent> replayControlGroupSequence(const ReplayData& replay, int playerId) {
    std::vector<SequenceEvent> sequence;
    sequence.reserve(std::min(replay.controlGroupSelections.size(), maximumAlignedControlGroupEvents));
    for (std::size_t index = 0;
         index < replay.controlGroupSelections.size() && sequence.size() < maximumAlignedControlGroupEvents;
         ++index) {
        const auto& event = replay.controlGroupSelections[index];
        if (event.playerId == playerId && event.group >= 0 && event.group <= 9)
            sequence.push_back({event.group, index});
    }
    return sequence;
}

std::vector<TimelineAnchor> makeTimelineAnchors(const std::vector<MechanicalInputEvent>& liveEvents,
                                                const ReplayData& replay,
                                                const ReplayPlayerMatch& match) {
    std::vector<TimelineAnchor> anchors;
    anchors.reserve(match.matchedEventIndices.size());
    for (const auto [liveIndex, replayIndex] : match.matchedEventIndices) {
        if (liveIndex >= liveEvents.size() || replayIndex >= replay.controlGroupSelections.size())
            continue;
        const TimelineAnchor candidate{replay.controlGroupSelections[replayIndex].replayFrame,
                                       liveEvents[liveIndex].activeMs};
        if (!anchors.empty() &&
            (candidate.replayFrame <= anchors.back().replayFrame ||
             candidate.liveActiveMs <= anchors.back().liveActiveMs))
            continue;
        anchors.push_back(candidate);
    }
    return anchors;
}

double boundedFrameSlope(const TimelineAnchor& first, const TimelineAnchor& second) noexcept {
    if (second.replayFrame <= first.replayFrame)
        return 42.0;
    return std::clamp((second.liveActiveMs - first.liveActiveMs) /
                          static_cast<double>(second.replayFrame - first.replayFrame),
                      5.0, 80.0);
}

double replayFrameToActiveMs(std::int64_t frame, const std::vector<TimelineAnchor>& anchors) noexcept {
    if (anchors.empty())
        return static_cast<double>(frame) * 42.0;
    if (anchors.size() == 1)
        return anchors.front().liveActiveMs +
               static_cast<double>(frame - anchors.front().replayFrame) * 42.0;
    if (frame <= anchors.front().replayFrame) {
        return anchors.front().liveActiveMs +
               static_cast<double>(frame - anchors.front().replayFrame) *
                   boundedFrameSlope(anchors[0], anchors[1]);
    }
    if (frame >= anchors.back().replayFrame) {
        return anchors.back().liveActiveMs +
               static_cast<double>(frame - anchors.back().replayFrame) *
                   boundedFrameSlope(anchors[anchors.size() - 2], anchors.back());
    }
    const auto upper = std::upper_bound(
        anchors.begin(), anchors.end(), frame,
        [](std::int64_t value, const TimelineAnchor& anchor) { return value < anchor.replayFrame; });
    const auto& second = *upper;
    const auto& first = *(upper - 1);
    return first.liveActiveMs + static_cast<double>(frame - first.replayFrame) *
                                    boundedFrameSlope(first, second);
}

double distanceToVisit(double eventActiveMs, const ProductionVisit& visit) noexcept {
    if (eventActiveMs < visit.contextActiveMs)
        return visit.contextActiveMs - eventActiveMs;
    if (eventActiveMs > visit.endActiveMs)
        return eventActiveMs - visit.endActiveMs;
    return 0.0;
}

void applyReplayEvents(ProductionVisit& visit, const std::vector<const ReplayProductionEvent*>& events) {
    if (events.empty())
        return;
    visit.replayConfirmed = true;
    visit.replayProductionCommands = static_cast<int>(events.size());
    bool worker = false;
    bool army = false;
    for (const auto* event : events) {
        const auto type = classifyReplayProduction(*event);
        worker = worker || type == MacroProductType::Worker;
        army = army || type == MacroProductType::Army;
        visit.producedUnits.push_back(event->unit);
    }
    visit.productType = worker == army ? MacroProductType::Unknown
                                       : (worker ? MacroProductType::Worker : MacroProductType::Army);
}

bool clickIsMinimapJump(const MechanicalInputEvent& click, const AnalysisResult& result) noexcept {
    return std::any_of(result.navigationEvents.begin(), result.navigationEvents.end(),
                       [&](const CameraNavigationEvent& event) {
                           return event.type == CameraNavigationType::MinimapJump &&
                                  event.timestampTicks == click.timestampTicks;
                       });
}

std::vector<ClickCandidate> collectClickCandidates(const AnalysisResult& result,
                                                   const MacroHotkeyProfile& hotkeys,
                                                   std::uint64_t qpcFrequency) {
    std::vector<ClickCandidate> candidates;
    const auto& events = result.mechanicalEvents;
    for (std::size_t clickIndex = 0; clickIndex < events.size(); ++clickIndex) {
        const auto& click = events[clickIndex];
        if (click.type != MechanicalInputType::MouseLeftDown || clickIsMinimapJump(click, result))
            continue;
        ClickCandidate candidate;
        candidate.clickMechanicalEventIndex = clickIndex;
        candidate.clickActiveMs = click.activeMs;
        candidate.clickTimestampTicks = click.timestampTicks;
        candidate.firstPressActiveMs = click.activeMs;
        candidate.firstPressTimestampTicks = click.timestampTicks;
        candidate.finalPressActiveMs = click.activeMs;
        candidate.finalPressTimestampTicks = click.timestampTicks;
        candidate.clickX = click.cursorX;
        candidate.clickY = click.cursorY;
        for (std::size_t index = clickIndex + 1; index < events.size(); ++index) {
            const auto& event = events[index];
            const auto realElapsed = qpcElapsedMs(click.timestampTicks, event.timestampTicks, qpcFrequency);
            const double activeElapsed = event.activeMs - click.activeMs;
            if (!realElapsed || activeElapsed < 0.0 || *realElapsed > productionVisitWindowMs ||
                activeElapsed > productionVisitWindowMs ||
                *realElapsed - activeElapsed > qpcActivePauseToleranceMs)
                break;
            if (event.type == MechanicalInputType::MouseLeftUp) {
                if (std::abs(event.cursorX - candidate.clickX) >= boxSelectionMinimumDragPixels ||
                    std::abs(event.cursorY - candidate.clickY) >= boxSelectionMinimumDragPixels)
                    candidate.selectionAccess = ProductionSelectionAccess::BoxSelect;
                continue;
            }
            if (isProductionPress(event, hotkeys)) {
                if (candidate.physicalPresses == 0) {
                    candidate.firstPressActiveMs = event.activeMs;
                    candidate.firstPressTimestampTicks = event.timestampTicks;
                }
                ++candidate.physicalPresses;
                candidate.physicalKeys.push_back(event.virtualKey);
                candidate.finalPressActiveMs = event.activeMs;
                candidate.finalPressTimestampTicks = event.timestampTicks;
                continue;
            }
            if (event.type == MechanicalInputType::KeyPress && isStandaloneModifier(event.virtualKey))
                continue;
            if (event.type == MechanicalInputType::MouseLeftDown ||
                event.type == MechanicalInputType::MouseRightDown ||
                event.type == MechanicalInputType::MouseMiddleDown ||
                event.type == MechanicalInputType::MouseWheel ||
                event.type == MechanicalInputType::ControlGroupSelect ||
                event.type == MechanicalInputType::ControlGroupAssign ||
                event.type == MechanicalInputType::ControlGroupAdd ||
                event.type == MechanicalInputType::LocationRecall ||
                event.type == MechanicalInputType::LocationAssign ||
                event.type == MechanicalInputType::KeyPress)
                break;
        }
        if (candidate.physicalPresses > 0)
            candidates.push_back(candidate);
    }
    return candidates;
}

ProductionVisit makeClickVisit(const ClickCandidate& candidate, const AnalysisResult& result,
                               std::uint64_t qpcFrequency) {
    ProductionVisit visit;
    visit.accessMethod = ProductionAccessMethod::ScreenClick;
    visit.selectionAccess = candidate.selectionAccess;
    visit.startActiveMs = candidate.clickActiveMs;
    visit.startTimestampTicks = candidate.clickTimestampTicks;
    visit.contextActiveMs = candidate.clickActiveMs;
    visit.contextTimestampTicks = candidate.clickTimestampTicks;
    visit.firstProductionActiveMs = candidate.firstPressActiveMs;
    visit.firstProductionTimestampTicks = candidate.firstPressTimestampTicks;
    visit.endActiveMs = candidate.finalPressActiveMs;
    visit.endTimestampTicks = candidate.finalPressTimestampTicks;
    visit.physicalProductionPresses = candidate.physicalPresses;
    visit.physicalProductionKeys = candidate.physicalKeys;

    struct AccessContext {
        double activeMs{};
        std::uint64_t timestampTicks{};
        ProductionAccessMethod method{ProductionAccessMethod::ScreenClick};
        int location{-1};
        std::uint32_t assignmentGeneration{};
    };
    std::optional<AccessContext> mostRecent;
    const auto consider = [&](AccessContext context) {
        if (context.timestampTicks > candidate.clickTimestampTicks)
            return;
        const auto realGap = qpcElapsedMs(context.timestampTicks, candidate.clickTimestampTicks,
                                          qpcFrequency);
        const double activeGap = candidate.clickActiveMs - context.activeMs;
        if (!realGap || activeGap < 0.0 || *realGap > productionAccessNavigationWindowMs ||
            *realGap - activeGap > qpcActivePauseToleranceMs)
            return;
        if (!mostRecent || context.timestampTicks >= mostRecent->timestampTicks)
            mostRecent = context;
    };
    std::unordered_map<int, std::uint32_t> locationAssignmentGenerations;
    std::unordered_map<std::uint64_t, std::unordered_map<int, std::uint32_t>>
        recallGenerationsByTimestamp;
    for (const auto& event : result.mechanicalEvents) {
        if (event.type == MechanicalInputType::LocationAssign && event.value >= 0) {
            ++locationAssignmentGenerations[event.value];
        } else if (event.type == MechanicalInputType::LocationRecall && event.value >= 0) {
            const auto generation = locationAssignmentGenerations[event.value];
            recallGenerationsByTimestamp[event.timestampTicks][event.value] = generation;
            consider({event.activeMs, event.timestampTicks,
                      ProductionAccessMethod::LocationHotkeyClick, event.value, generation});
        }
    }
    for (const auto& event : result.navigationEvents) {
        switch (event.type) {
        case CameraNavigationType::LocationHotkey: {
            std::uint32_t generation{};
            const auto atTimestamp = recallGenerationsByTimestamp.find(event.timestampTicks);
            if (atTimestamp != recallGenerationsByTimestamp.end()) {
                const auto atLocation = atTimestamp->second.find(event.id);
                if (atLocation != atTimestamp->second.end())
                    generation = atLocation->second;
            }
            consider({event.activeMs, event.timestampTicks,
                      ProductionAccessMethod::LocationHotkeyClick, event.id, generation});
            break;
        }
        case CameraNavigationType::MinimapJump:
            consider({event.activeMs, event.timestampTicks,
                      ProductionAccessMethod::MinimapClick, -1, 0});
            break;
        case CameraNavigationType::ControlGroupJump:
        case CameraNavigationType::EdgeScroll:
            consider({event.activeMs, event.timestampTicks,
                      ProductionAccessMethod::ScreenClick, -1, 0});
            break;
        }
    }
    if (mostRecent && mostRecent->method != ProductionAccessMethod::ScreenClick) {
        visit.accessMethod = mostRecent->method;
        visit.startActiveMs = mostRecent->activeMs;
        visit.startTimestampTicks = mostRecent->timestampTicks;
        visit.locationHotkey = mostRecent->location;
    }
    if (visit.accessMethod == ProductionAccessMethod::LocationHotkeyClick) {
        visit.productionContext = makeLocationHotkeyProductionContext(
            visit.locationHotkey, mostRecent->assignmentGeneration);
    }
    refreshProductionVisitTiming(visit, qpcFrequency);
    return visit;
}

std::filesystem::path localParserPath() {
    PWSTR localAppDataRaw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppDataRaw)))
        throw std::runtime_error("Unable to locate Local AppData for the replay parser");
    const std::filesystem::path localAppData(localAppDataRaw);
    CoTaskMemFree(localAppDataRaw);
    return localAppData / "Starcraft Mechanics Profiler" / "tools" / bundledParserFilename;
}

std::filesystem::path extractBundledParser(const std::filesystem::path& destination) {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_SCREP_BIN), RT_RCDATA);
    if (!resource)
        throw std::runtime_error("The bundled replay parser resource is missing");
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD resourceSize = SizeofResource(module, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || resourceSize == 0)
        throw std::runtime_error("The bundled replay parser resource is invalid");

    std::error_code sizeError;
    if (std::filesystem::is_regular_file(destination, sizeError) &&
        std::filesystem::file_size(destination, sizeError) == resourceSize && !sizeError)
        return destination;

    std::filesystem::create_directories(destination.parent_path());
    const auto temporary = std::filesystem::path(destination.string() + ".tmp");
    ScopedPathRemoval removeTemporary(temporary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("Unable to extract the bundled replay parser");
    output.write(static_cast<const char*>(bytes), resourceSize);
    output.flush();
    if (!output)
        throw std::runtime_error("Unable to write the bundled replay parser");
    output.close();
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Unable to finalize the bundled replay parser");
    return destination;
}

std::wstring quoted(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

std::filesystem::path replayParserOutputPath() {
    wchar_t temporaryDirectory[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, temporaryDirectory);
    if (length == 0 || length > MAX_PATH)
        throw std::runtime_error("Unable to locate the temporary folder for replay analysis");
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    const auto directory = std::filesystem::path(temporaryDirectory) / "Starcraft Mechanics Profiler";
    std::filesystem::create_directories(directory);
    return directory / ("replay-" + std::to_string(GetCurrentProcessId()) + "-" +
                        std::to_string(ticks.QuadPart) + ".json");
}

void runParser(const std::filesystem::path& parser, const std::filesystem::path& replay,
               const std::filesystem::path& output, std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero())
        throw std::runtime_error("Replay parser timed out");
    std::wstring command = quoted(parser) +
                           L" -cmds -computed=false -header=true -map=true -indent=false -outfile " +
                           quoted(output) + L" " + quoted(replay);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(parser.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, parser.parent_path().c_str(), &startup, &process))
        throw std::runtime_error("Unable to start the bundled replay parser");
    CloseHandle(process.hThread);
    const auto timeoutCount = static_cast<unsigned long long>(timeout.count());
    const DWORD waitMilliseconds = static_cast<DWORD>(
        std::min(timeoutCount, static_cast<unsigned long long>(INFINITE - 1)));
    const DWORD wait = WaitForSingleObject(process.hProcess, waitMilliseconds);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        throw std::runtime_error("Replay parser timed out");
    }
    DWORD exitCode = 1;
    const bool exitRead = GetExitCodeProcess(process.hProcess, &exitCode) != FALSE;
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || !exitRead || exitCode != 0)
        throw std::runtime_error("Replay parser failed");
}

struct ReplayControlGroupSnapshot {
    const ReplayControlGroupEditEvent* event{};
    double activeMs{};
    std::vector<std::uint32_t> unitTags;
    std::vector<std::string> unitTypes;
    bool used{};
};

struct ReplayCommandTargetSnapshot {
    const ReplayCommandTargetEvent* event{};
    double activeMs{};
    std::vector<std::uint32_t> unitTags;
};

enum class ReplayStateActionKind {
    Selection,
    GroupSelect,
    GroupEdit,
    CommandTarget,
    Build,
    Production
};

struct ReplayStateAction {
    ReplayPosition position;
    ReplayStateActionKind kind{};
    const void* event{};
};

void addUniqueTags(std::vector<std::uint32_t>& target,
                   const std::vector<std::uint32_t>& source) {
    for (const auto tag : source) {
        if (std::find(target.begin(), target.end(), tag) == target.end())
            target.push_back(tag);
    }
}

void removeTags(std::vector<std::uint32_t>& target,
                const std::vector<std::uint32_t>& source) {
    target.erase(std::remove_if(target.begin(), target.end(), [&](std::uint32_t tag) {
                     return std::find(source.begin(), source.end(), tag) != source.end();
                 }),
                 target.end());
}

std::optional<std::string> workerTypeForPlayer(const ReplayData& replay,
                                               int playerId) {
    const auto player = std::find_if(
        replay.players.begin(), replay.players.end(),
        [playerId](const ReplayPlayer& candidate) {
            return candidate.id == playerId;
        });
    if (player == replay.players.end())
        return std::nullopt;
    const auto race = normalizedUnitName(player->race);
    if (race == "PROTOSS")
        return "Probe";
    if (race == "TERRAN")
        return "SCV";
    if (race == "ZERG")
        return "Drone";
    return std::nullopt;
}

std::vector<ReplayControlGroupSnapshot> reconstructControlGroupSnapshots(
    const ReplayData& replay, int playerId, const std::vector<TimelineAnchor>& anchors,
    std::unordered_set<std::uint32_t>& productionBuildingTags,
    std::unordered_set<std::uint32_t>& workerTags,
    std::vector<ReplayCommandTargetSnapshot>& commandTargets) {
    std::vector<ReplayStateAction> actions;
    for (const auto& selection : replay.selections) {
        if (selection.playerId == playerId)
            actions.push_back({positionOf(selection), ReplayStateActionKind::Selection, &selection});
    }
    for (const auto& selection : replay.controlGroupSelections) {
        if (selection.playerId == playerId)
            actions.push_back({positionOf(selection), ReplayStateActionKind::GroupSelect, &selection});
    }
    for (const auto& edit : replay.controlGroupEdits) {
        if (edit.playerId == playerId)
            actions.push_back({positionOf(edit), ReplayStateActionKind::GroupEdit, &edit});
    }
    for (const auto& command : replay.commandTargets) {
        if (command.playerId == playerId)
            actions.push_back(
                {positionOf(command), ReplayStateActionKind::CommandTarget,
                 &command});
    }
    for (const auto& build : replay.buildEvents) {
        if (build.playerId == playerId)
            actions.push_back(
                {positionOf(build), ReplayStateActionKind::Build, &build});
    }
    for (const auto& production : replay.productionEvents) {
        if (production.playerId == playerId)
            actions.push_back({positionOf(production), ReplayStateActionKind::Production, &production});
    }
    std::sort(actions.begin(), actions.end(), [](const auto& first, const auto& second) {
        return positionLess(first.position, second.position);
    });

    std::vector<std::uint32_t> currentSelection;
    std::array<std::vector<std::uint32_t>, 10> groupBindings;
    std::unordered_map<std::uint32_t, std::string> typeByTag;
    const auto workerType = workerTypeForPlayer(replay, playerId);
    std::vector<ReplayControlGroupSnapshot> snapshots;
    snapshots.reserve(replay.controlGroupEdits.size());
    for (const auto& action : actions) {
        switch (action.kind) {
        case ReplayStateActionKind::Selection: {
            const auto& selection = *static_cast<const ReplaySelectionEvent*>(action.event);
            if (selection.unitTypes.size() == selection.unitTags.size()) {
                for (std::size_t index = 0; index < selection.unitTags.size(); ++index)
                    typeByTag[selection.unitTags[index]] = selection.unitTypes[index];
            }
            if (selection.kind == ReplaySelectionKind::Select)
                currentSelection = selection.unitTags;
            else if (selection.kind == ReplaySelectionKind::Add)
                addUniqueTags(currentSelection, selection.unitTags);
            else
                removeTags(currentSelection, selection.unitTags);
            break;
        }
        case ReplayStateActionKind::GroupSelect: {
            const auto& selection = *static_cast<const ReplayControlGroupEvent*>(action.event);
            currentSelection = groupBindings[static_cast<std::size_t>(selection.group)];
            break;
        }
        case ReplayStateActionKind::GroupEdit: {
            const auto& edit = *static_cast<const ReplayControlGroupEditEvent*>(action.event);
            ReplayControlGroupSnapshot snapshot;
            snapshot.event = &edit;
            snapshot.activeMs = replayFrameToActiveMs(edit.replayFrame, anchors);
            snapshot.unitTags = currentSelection;
            for (const auto tag : currentSelection) {
                if (const auto found = typeByTag.find(tag); found != typeByTag.end() &&
                    std::find(snapshot.unitTypes.begin(), snapshot.unitTypes.end(), found->second) ==
                        snapshot.unitTypes.end())
                    snapshot.unitTypes.push_back(found->second);
            }
            snapshots.push_back(std::move(snapshot));
            auto& binding = groupBindings[static_cast<std::size_t>(edit.group)];
            if (edit.operation == ArmyControlGroupOperation::Assign)
                binding = currentSelection;
            else
                addUniqueTags(binding, currentSelection);
            break;
        }
        case ReplayStateActionKind::CommandTarget: {
            const auto& command =
                *static_cast<const ReplayCommandTargetEvent*>(action.event);
            commandTargets.push_back(
                {&command, replayFrameToActiveMs(command.replayFrame, anchors),
                 currentSelection});
            break;
        }
        case ReplayStateActionKind::Build:
            if (currentSelection.size() == 1) {
                workerTags.insert(currentSelection.front());
                if (workerType)
                    typeByTag.try_emplace(currentSelection.front(), *workerType);
            }
            break;
        case ReplayStateActionKind::Production: {
            const auto& production =
                *static_cast<const ReplayProductionEvent*>(action.event);
            if (production.kind == ReplayProductionKind::Train)
                productionBuildingTags.insert(currentSelection.begin(), currentSelection.end());
            break;
        }
        }
    }
    for (auto& snapshot : snapshots) {
        snapshot.unitTypes.clear();
        for (const auto tag : snapshot.unitTags) {
            if (const auto found = typeByTag.find(tag); found != typeByTag.end() &&
                std::find(snapshot.unitTypes.begin(), snapshot.unitTypes.end(),
                          found->second) == snapshot.unitTypes.end())
                snapshot.unitTypes.push_back(found->second);
        }
    }
    return snapshots;
}

bool sameReplayUnitMembership(const std::vector<std::uint32_t>& first,
                              const std::vector<std::uint32_t>& second) {
    if (first.empty() || first.size() != second.size())
        return false;
    auto sortedFirst = first;
    auto sortedSecond = second;
    std::sort(sortedFirst.begin(), sortedFirst.end());
    std::sort(sortedSecond.begin(), sortedSecond.end());
    return sortedFirst == sortedSecond;
}

struct ScoutingMapGeometry {
    double ownX{};
    double ownY{};
    std::vector<std::pair<double, double>> enemyStarts;
};

std::optional<ScoutingMapGeometry> scoutingMapGeometry(const ReplayData& replay,
                                                       int playerId) {
    const auto player = std::find_if(
        replay.players.begin(), replay.players.end(),
        [playerId](const ReplayPlayer& candidate) {
            return candidate.id == playerId && candidate.slotId >= 0;
        });
    if (player == replay.players.end())
        return std::nullopt;
    const auto ownStart = std::find_if(
        replay.startLocations.begin(), replay.startLocations.end(),
        [&](const ReplayStartLocation& candidate) {
            return candidate.slotId == player->slotId;
        });
    if (ownStart == replay.startLocations.end())
        return std::nullopt;

    ScoutingMapGeometry geometry;
    geometry.ownX = ownStart->x;
    geometry.ownY = ownStart->y;
    for (const auto& other : replay.players) {
        if (other.id == playerId || other.slotId < 0)
            continue;
        const auto otherStart = std::find_if(
            replay.startLocations.begin(), replay.startLocations.end(),
            [&](const ReplayStartLocation& candidate) {
                return candidate.slotId == other.slotId;
            });
        if (otherStart == replay.startLocations.end())
            continue;
        const std::pair<double, double> point{otherStart->x, otherStart->y};
        if (std::find(geometry.enemyStarts.begin(), geometry.enemyStarts.end(),
                      point) == geometry.enemyStarts.end())
            geometry.enemyStarts.push_back(point);
    }
    if (geometry.enemyStarts.empty() && replay.mapWidthPixels > 0.0 &&
        replay.mapHeightPixels > 0.0) {
        // Real 1v1 replays identify the occupied opponent start above. The map
        // center is only a compatibility fallback for incomplete/synthetic replay
        // metadata where no other occupied player start is available.
        geometry.enemyStarts.emplace_back(replay.mapWidthPixels * 0.5,
                                          replay.mapHeightPixels * 0.5);
    }
    if (geometry.enemyStarts.empty())
        return std::nullopt;
    return geometry;
}

std::vector<ScoutingUnitCommandEvidence> scoutingCommandEvidence(
    const ArmyControlGroupAnalysis& analysis,
    const std::vector<ReplayControlGroupSnapshot>& snapshots,
    const std::vector<std::optional<std::size_t>>& matchedSnapshotIndices,
    const std::vector<ReplayCommandTargetSnapshot>& commandTargets,
    const ReplayData& replay, int playerId) {
    std::vector<ScoutingUnitCommandEvidence> evidence;
    const auto geometry = scoutingMapGeometry(replay, playerId);
    if (!geometry)
        return evidence;

    const auto distanceSquared = [](double firstX, double firstY,
                                    double secondX, double secondY) {
        const double dx = firstX - secondX;
        const double dy = firstY - secondY;
        return dx * dx + dy * dy;
    };

    for (std::size_t physicalIndex = 0;
         physicalIndex < analysis.edits.size(); ++physicalIndex) {
        if (!matchedSnapshotIndices[physicalIndex])
            continue;
        const auto snapshotIndex = *matchedSnapshotIndices[physicalIndex];
        const auto& snapshot = snapshots[snapshotIndex];
        if (snapshot.event->operation != ArmyControlGroupOperation::Assign ||
            snapshot.unitTags.size() != 1)
            continue;
        const auto unitTag = snapshot.unitTags.front();
        const ReplayPosition assignmentPosition = positionOf(*snapshot.event);
        for (const auto& command : commandTargets) {
            const ReplayPosition commandPosition = positionOf(*command.event);
            if (!positionLess(assignmentPosition, commandPosition))
                continue;
            if (std::find(command.unitTags.begin(), command.unitTags.end(), unitTag) ==
                command.unitTags.end())
                continue;

            const auto enemy = std::min_element(
                geometry->enemyStarts.begin(), geometry->enemyStarts.end(),
                [&](const auto& first, const auto& second) {
                    return distanceSquared(command.event->x, command.event->y,
                                           first.first, first.second) <
                           distanceSquared(command.event->x, command.event->y,
                                           second.first, second.second);
                });
            if (enemy == geometry->enemyStarts.end())
                continue;
            evidence.push_back(
                {physicalIndex, unitTag, geometry->ownX, geometry->ownY,
                 enemy->first, enemy->second, command.event->x,
                 command.event->y, command.activeMs});
        }
    }
    return evidence;
}

bool productionBuildingUnitType(std::string_view type) {
    static const std::unordered_set<std::string> types{
        "COMMANDCENTER", "BARRACKS", "FACTORY", "STARPORT", "SCIENCEFACILITY",
        "MACHINESHOP", "CONTROLTOWER", "NUCLEARSILO", "ACADEMY", "NEXUS",
        "GATEWAY", "ROBOTICSFACILITY", "STARGATE", "CITADELOFADUN",
        "TEMPLARARCHIVES", "FLEETBEACON", "ROBOTICSSUPPORTBAY", "OBSERVATORY",
        "HATCHERY", "LAIR", "HIVE", "LARVA", "HYDRALISKDEN", "SPIRE",
        "GREATERSPIRE", "QUEENSNEST", "ULTRALISKCAVERN", "DEFILERMOUND"};
    return types.contains(normalizedUnitName(type));
}

ArmyControlGroupScope classifyControlGroupScope(
    const ReplayControlGroupSnapshot& snapshot,
    const std::unordered_set<std::uint32_t>& productionBuildingTags,
    const std::unordered_set<std::uint32_t>& workerTags) {
    if (snapshot.unitTags.empty())
        return ArmyControlGroupScope::Uncertain;
    if (!snapshot.unitTags.empty() &&
        std::all_of(snapshot.unitTags.begin(), snapshot.unitTags.end(), [&](std::uint32_t tag) {
            return productionBuildingTags.contains(tag);
        }))
        return ArmyControlGroupScope::ProductionBuilding;
    if (std::all_of(snapshot.unitTags.begin(), snapshot.unitTags.end(),
                    [&](std::uint32_t tag) {
                        return workerTags.contains(tag);
                    }))
        return ArmyControlGroupScope::Worker;
    const bool hasProduction = std::any_of(snapshot.unitTypes.begin(), snapshot.unitTypes.end(),
                                           productionBuildingUnitType);
    if (hasProduction &&
        std::all_of(snapshot.unitTypes.begin(), snapshot.unitTypes.end(),
                    productionBuildingUnitType))
        return ArmyControlGroupScope::ProductionBuilding;
    return ArmyControlGroupScope::Army;
}

void correlateArmyControlGroupManagement(ArmyControlGroupAnalysis& analysis,
                                         const AnalysisResult& result,
                                         std::uint64_t qpcFrequency,
                                         const ReplayData& replay, int playerId,
                                         const std::vector<TimelineAnchor>& anchors) {
    std::unordered_set<std::uint32_t> productionBuildingTags;
    std::unordered_set<std::uint32_t> workerTags;
    std::vector<ReplayCommandTargetSnapshot> commandTargets;
    auto snapshots = reconstructControlGroupSnapshots(replay, playerId, anchors,
                                                      productionBuildingTags,
                                                      workerTags,
                                                      commandTargets);
    std::vector<std::optional<std::size_t>> matchedSnapshotIndices(
        analysis.edits.size());
    for (std::size_t physicalIndex = 0; physicalIndex < analysis.edits.size();
         ++physicalIndex) {
        auto& physical = analysis.edits[physicalIndex];
        std::optional<std::size_t> best;
        double bestDistance = replayControlGroupEditMatchWindowMs + 1.0;
        double secondDistance = replayControlGroupEditMatchWindowMs + 1.0;
        for (std::size_t index = 0; index < snapshots.size(); ++index) {
            const auto& candidate = snapshots[index];
            if (candidate.used || candidate.event->group != physical.group ||
                candidate.event->operation != physical.operation)
                continue;
            const double distance = std::abs(candidate.activeMs - physical.operationActiveMs);
            if (distance > replayControlGroupEditMatchWindowMs)
                continue;
            if (distance < bestDistance) {
                secondDistance = bestDistance;
                bestDistance = distance;
                best = index;
            } else if (distance < secondDistance) {
                secondDistance = distance;
            }
        }
        if (!best)
            continue;
        if (secondDistance <= replayControlGroupEditMatchWindowMs &&
            secondDistance - bestDistance < 20.0) {
            physical.bindingConfidence = ArmyControlGroupBindingConfidence::Ambiguous;
            continue;
        }
        auto& snapshot = snapshots[*best];
        snapshot.used = true;
        matchedSnapshotIndices[physicalIndex] = *best;
        physical.replayConfirmed = true;
        physical.bindingConfidence = ArmyControlGroupBindingConfidence::ReplayConfirmed;
        physical.selectedUnitTags = snapshot.unitTags;
        physical.selectedUnitTypes = snapshot.unitTypes;
        physical.selectedUnitCount = static_cast<int>(snapshot.unitTags.size());
        physical.scope = classifyControlGroupScope(
            snapshot, productionBuildingTags, workerTags);
    }
    analysis.available = true;
    analysis.unavailableReason.clear();
    applyScoutingUnitClassification(
        analysis,
        scoutingCommandEvidence(analysis, snapshots, matchedSnapshotIndices,
                                commandTargets, replay, playerId));
    analyzeScoutingUnitActivity(analysis, result, qpcFrequency);
}

void markReplayUnavailable(ProductionAnalysis& analysis, const std::string& reason,
                           std::string parser = {}) {
    analysis.replayCorrelation = {};
    analysis.replayCorrelation.unavailableReason = reason;
    analysis.replayCorrelation.parser = std::move(parser);
    analysis.replayCorrelation.unmatchedProductionVisits = analysis.productionVisits.size();
    analysis.workerMacroCycles = {};
    analysis.workerMacroCycles.productType = MacroProductType::Worker;
    analysis.workerMacroCycles.unavailableReason = reason;
    analysis.armyMacroCycles = {};
    analysis.armyMacroCycles.productType = MacroProductType::Army;
    analysis.armyMacroCycles.unavailableReason = reason;
    analysis.armyControlGroupManagement.available = false;
    analysis.armyControlGroupManagement.unavailableReason = reason;
}

} // namespace

ReplayData parseScrepReplayJson(const std::string& replayJson) {
    const auto root = json::parse(replayJson);
    if (!root.isObject() || !root["Header"].isObject() || !root["Commands"].isObject())
        throw std::runtime_error("Replay parser returned an incomplete document");
    ReplayData replay;
    replay.totalFrames = static_cast<std::int64_t>(root["Header"]["Frames"].asNumber(-1.0));
    if (replay.totalFrames < 0)
        throw std::runtime_error("Replay parser returned an invalid frame count");
    const double mapWidthTiles = root["Header"]["MapWidth"].asNumber(0.0);
    const double mapHeightTiles = root["Header"]["MapHeight"].asNumber(0.0);
    if (std::isfinite(mapWidthTiles) && mapWidthTiles > 0.0)
        replay.mapWidthPixels = mapWidthTiles * 32.0;
    if (std::isfinite(mapHeightTiles) && mapHeightTiles > 0.0)
        replay.mapHeightPixels = mapHeightTiles * 32.0;
    for (const auto& playerValue : root["Header"]["Players"].asArray()) {
        const int id = playerValue["ID"].asInt(-1);
        if (id >= 0)
            replay.players.push_back(
                {id, playerValue["Name"].asString(),
                 playerValue["SlotID"].asInt(-1),
                 playerValue["Race"].isObject()
                     ? playerValue["Race"]["Name"].asString()
                     : playerValue["Race"].asString()});
    }
    for (const auto& location : root["MapData"]["StartLocations"].asArray()) {
        const int slotId = location["SlotID"].asInt(-1);
        const double x = location["X"].asNumber(-1.0);
        const double y = location["Y"].asNumber(-1.0);
        if (slotId >= 0 && std::isfinite(x) && std::isfinite(y) && x >= 0.0 &&
            y >= 0.0)
            replay.startLocations.push_back({slotId, x, y});
    }
    const auto& commands = root["Commands"]["Cmds"].asArray();
    for (std::size_t commandIndex = 0; commandIndex < commands.size(); ++commandIndex) {
        const auto& command = commands[commandIndex];
        const auto frame = static_cast<std::int64_t>(command["Frame"].asNumber(-1.0));
        const int playerId = command["PlayerID"].asInt(-1);
        const auto type = command["Type"]["Name"].asString();
        if (frame < 0 || playerId < 0)
            continue;
        if (type == "Hotkey") {
            const auto hotkeyType = command["HotkeyType"]["Name"].asString();
            const int group = command["Group"].asInt(-1);
            if (group >= 0 && group <= 9) {
                if (hotkeyType == "Select") {
                    replay.controlGroupSelections.push_back({frame, playerId, group, commandIndex});
                } else if (hotkeyType == "Assign" || hotkeyType == "Add") {
                    replay.controlGroupEdits.push_back(
                        {frame, playerId, group,
                         hotkeyType == "Assign" ? ArmyControlGroupOperation::Assign
                                                : ArmyControlGroupOperation::Add,
                         commandIndex});
                }
            }
            continue;
        }
        if (type == "Select" || type == "Select Add" || type == "Select Remove") {
            ReplaySelectionEvent selection;
            selection.replayFrame = frame;
            selection.playerId = playerId;
            selection.commandIndex = commandIndex;
            selection.kind = type == "Select"       ? ReplaySelectionKind::Select
                             : type == "Select Add" ? ReplaySelectionKind::Add
                                                    : ReplaySelectionKind::Remove;
            for (const auto& unitTag : command["UnitTags"].asArray()) {
                const double value = unitTag.asNumber(-1.0);
                if (value >= 0.0 && value <= static_cast<double>(UINT32_MAX))
                    selection.unitTags.push_back(static_cast<std::uint32_t>(value));
            }
            for (const auto& unitType : command["UnitTypes"].asArray()) {
                const auto name = unitType.isObject() ? unitType["Name"].asString()
                                                      : unitType.asString();
                if (!name.empty())
                    selection.unitTypes.push_back(name);
            }
            if (!selection.unitTags.empty())
                replay.selections.push_back(std::move(selection));
            continue;
        }
        if (type == "Right Click") {
            const double x = command["Pos"]["X"].asNumber(-1.0);
            const double y = command["Pos"]["Y"].asNumber(-1.0);
            if (std::isfinite(x) && std::isfinite(y) && x >= 0.0 && y >= 0.0)
                replay.commandTargets.push_back(
                    {frame, playerId, x, y, commandIndex});
            continue;
        }
        if (type == "Build") {
            replay.buildEvents.push_back({frame, playerId, commandIndex});
            continue;
        }
        ReplayProductionEvent production;
        production.replayFrame = frame;
        production.playerId = playerId;
        production.commandIndex = commandIndex;
        if (type == "Train") {
            production.kind = ReplayProductionKind::Train;
        } else if (type == "Unit Morph") {
            production.kind = ReplayProductionKind::UnitMorph;
        } else if (type == "Train Fighter") {
            production.kind = ReplayProductionKind::TrainFighter;
            production.unit = "Interceptor/Scarab";
            replay.productionEvents.push_back(std::move(production));
            continue;
        } else {
            continue;
        }
        production.unit = command["Unit"]["Name"].asString();
        production.unitId = command["Unit"]["ID"].asInt(-1);
        if (classifyReplayProduction(production) != MacroProductType::Unknown)
            replay.productionEvents.push_back(std::move(production));
    }
    return replay;
}

namespace {

ReplayExtractionResult extractReplayWithBundledScrepImpl(
    const std::filesystem::path& replayPath,
    const std::filesystem::path* parserDestination,
    std::chrono::milliseconds parserTimeout) noexcept {
    ReplayExtractionResult result;
    result.parser = bundledReplayParserDiagnostic;
    try {
        if (!std::filesystem::is_regular_file(replayPath))
            throw std::runtime_error("Replay file is missing");
        const auto parser = extractBundledParser(parserDestination ? *parserDestination
                                                                   : localParserPath());
        const auto output = replayParserOutputPath();
        ScopedPathRemoval removeOutput(output);
        runParser(parser, replayPath, output, parserTimeout);
        std::ifstream input(output, std::ios::binary);
        if (!input)
            throw std::runtime_error("Replay parser produced no output");
        std::ostringstream text;
        text << input.rdbuf();
        result.replay = parseScrepReplayJson(text.str());
        result.available = true;
    } catch (const std::exception& error) {
        result.unavailableReason = error.what();
    } catch (...) {
        result.unavailableReason = "Replay parser failed";
    }
    return result;
}

} // namespace

ReplayExtractionResult extractReplayWithBundledScrep(
    const std::filesystem::path& replayPath, std::chrono::milliseconds parserTimeout) noexcept {
    return extractReplayWithBundledScrepImpl(replayPath, nullptr, parserTimeout);
}

ReplayExtractionResult extractReplayWithBundledScrepForValidation(
    const std::filesystem::path& replayPath,
    const std::filesystem::path& parserDestination,
    std::chrono::milliseconds parserTimeout) noexcept {
    return extractReplayWithBundledScrepImpl(replayPath, &parserDestination, parserTimeout);
}

ReplayPlayerMatch identifyReplayPlayer(const std::vector<MechanicalInputEvent>& liveEvents,
                                       const ReplayData& replay) {
    ReplayPlayerMatch result;
    const auto live = liveControlGroupSequence(liveEvents);
    if (live.size() < 2) {
        result.unavailableReason =
            "Live recording contains too few control-group selections for replay-player identification";
        return result;
    }

    struct Candidate {
        ReplayPlayer player;
        double score{};
        std::vector<std::pair<std::size_t, std::size_t>> matches;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(replay.players.size());
    for (const auto& player : replay.players) {
        const auto replaySequence = replayControlGroupSequence(replay, player.id);
        auto matches = longestCommonSubsequence(live, replaySequence);
        candidates.push_back({player, sequenceScore(matches.size(), live.size(), replaySequence.size()),
                              std::move(matches)});
    }
    if (candidates.empty()) {
        result.unavailableReason = "Replay contains no players";
        return result;
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& first, const Candidate& second) {
        if (first.score != second.score)
            return first.score > second.score;
        return first.matches.size() > second.matches.size();
    });
    const auto& best = candidates.front();
    const double runnerUp = candidates.size() > 1 ? candidates[1].score : 0.0;
    const std::size_t requiredMatches = live.size() <= 3
        ? std::min<std::size_t>(2, live.size())
        : 3;
    if (best.matches.size() < requiredMatches || best.score < minimumPlayerSequenceScore) {
        result.unavailableReason = "No replay player has a high-confidence control-group sequence match";
        return result;
    }
    if (candidates.size() > 1 && best.score - runnerUp < minimumPlayerScoreLead) {
        result.unavailableReason = "Replay player match is ambiguous";
        return result;
    }
    result.available = true;
    result.playerId = best.player.id;
    result.playerName = best.player.name;
    result.sequenceScore = best.score;
    result.runnerUpSequenceScore = runnerUp;
    result.matchedEventIndices = best.matches;
    return result;
}

MacroProductType classifyReplayProduction(const ReplayProductionEvent& event) noexcept {
    if (event.kind == ReplayProductionKind::TrainFighter)
        return MacroProductType::Army;
    if (event.unitId == 0x07 || event.unitId == 0x29 || event.unitId == 0x40 ||
        event.unit == "SCV" || event.unit == "Drone" || event.unit == "Probe")
        return MacroProductType::Worker;
    static const std::unordered_set<int> ordinaryArmyUnitIds{
        0x00, 0x01, 0x02, 0x03, 0x05, 0x08, 0x09, 0x0b, 0x0c, 0x20, 0x22, 0x3a,
        0x25, 0x26, 0x27, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x32, 0x3e, 0x67,
        0x3c, 0x3d, 0x41, 0x42, 0x43, 0x45, 0x46, 0x47, 0x48, 0x49, 0x53, 0x54, 0x55};
    return ordinaryArmyUnitIds.contains(event.unitId) ? MacroProductType::Army
                                                      : MacroProductType::Unknown;
}

bool replayProductionCompatibleWithPhysicalKey(const ReplayProductionEvent& replay,
                                               std::uint16_t physicalKey,
                                               const MacroHotkeyProfile& hotkeys) {
    const auto commands = hotkeys.compatibleProductionCommands(physicalKey);
    if (commands.empty())
        return false;
    if (replay.kind == ReplayProductionKind::TrainFighter) {
        return std::any_of(commands.begin(), commands.end(), [](const std::string& command) {
            const auto unit = productionCommandUnit(command);
            return unit == "INTERCEPTOR" || unit == "SCARAB";
        });
    }
    const auto replayUnit = canonicalUnitName(normalizedUnitName(replay.unit));
    if (replayUnit.empty())
        return false;
    return std::any_of(commands.begin(), commands.end(), [&](const std::string& command) {
        return productionCommandUnit(command) == replayUnit;
    });
}

ProductionAnalysis correlateProductionVisitsWithReplay(
    const AnalysisResult& result, const MacroHotkeyProfile& hotkeys, std::uint64_t qpcFrequency,
    ProductionAnalysis analysis, const ReplayData& replay, std::string parserName) {
    if (!analysis.visitsAvailable) {
        markReplayUnavailable(analysis, analysis.visitsUnavailableReason, std::move(parserName));
        return analysis;
    }
    const auto playerMatch = identifyReplayPlayer(result.mechanicalEvents, replay);
    if (!playerMatch.available) {
        markReplayUnavailable(analysis, playerMatch.unavailableReason, std::move(parserName));
        return analysis;
    }
    const auto anchors = makeTimelineAnchors(result.mechanicalEvents, replay, playerMatch);
    if (anchors.empty()) {
        markReplayUnavailable(analysis, "Replay/live timeline has no usable anchors", std::move(parserName));
        return analysis;
    }
    if (replay.totalFrames > 0 && result.activeDurationSeconds > 0.0) {
        const double replayDurationSeconds = static_cast<double>(replay.totalFrames) * 0.042;
        const double durationRatio = result.activeDurationSeconds / replayDurationSeconds;
        if (durationRatio < 0.15 || durationRatio > 1.50) {
            markReplayUnavailable(analysis, "Replay duration does not match the recorded session",
                                  std::move(parserName));
            return analysis;
        }
    }
    analysis.replayCorrelation = {};

    std::vector<MappedProductionEvent> mapped;
    for (const auto& event : replay.productionEvents) {
        if (event.playerId == playerMatch.playerId &&
            classifyReplayProduction(event) != MacroProductType::Unknown)
            mapped.push_back({&event, replayFrameToActiveMs(event.replayFrame, anchors), false});
    }
    std::sort(mapped.begin(), mapped.end(), [](const auto& first, const auto& second) {
        return positionLess(positionOf(*first.event), positionOf(*second.event));
    });

    std::vector<MappedSelectionEvent> mappedSelections;
    for (const auto& selection : replay.selections) {
        if (selection.playerId == playerMatch.playerId && !selection.unitTags.empty())
            mappedSelections.push_back(
                {&selection, replayFrameToActiveMs(selection.replayFrame, anchors), false});
    }
    std::sort(mappedSelections.begin(), mappedSelections.end(),
              [](const auto& first, const auto& second) {
                  return positionLess(positionOf(*first.event), positionOf(*second.event));
              });

    std::unordered_map<std::size_t, std::size_t> matchedReplayControlGroupByLiveIndex;
    for (const auto [liveIndex, replayIndex] : playerMatch.matchedEventIndices)
        matchedReplayControlGroupByLiveIndex.emplace(liveIndex, replayIndex);

    std::vector<CorrelationCandidate> candidates;
    const auto controlGroupCandidates =
        detectControlGroupProductionCandidates(result, hotkeys, qpcFrequency);
    candidates.reserve(controlGroupCandidates.size());
    for (const auto& controlGroup : controlGroupCandidates) {
        const auto matched =
            matchedReplayControlGroupByLiveIndex.find(controlGroup.selectMechanicalEventIndex);
        if (matched == matchedReplayControlGroupByLiveIndex.end() ||
            matched->second >= replay.controlGroupSelections.size())
            continue;
        const auto& replaySelect = replay.controlGroupSelections[matched->second];
        if (replaySelect.playerId != playerMatch.playerId ||
            replaySelect.group != controlGroup.visit.controlGroup)
            continue;

        std::optional<std::size_t> existingVisitIndex;
        for (std::size_t index = 0; index < analysis.productionVisits.size(); ++index) {
            const auto& visit = analysis.productionVisits[index];
            if (visit.accessMethod == ProductionAccessMethod::ControlGroup &&
                visit.controlGroup == controlGroup.visit.controlGroup &&
                visit.startTimestampTicks == controlGroup.visit.startTimestampTicks) {
                existingVisitIndex = index;
                break;
            }
        }
        ProductionVisit visit = existingVisitIndex ? analysis.productionVisits[*existingVisitIndex]
                                                   : controlGroup.visit;
        if (visit.physicalProductionKeys.empty())
            visit.physicalProductionKeys = controlGroup.visit.physicalProductionKeys;
        if (!knownProductionContext(visit.productionContext)) {
            visit.productionContext = controlGroup.visit.productionContext;
        }
        candidates.push_back({CorrelationCandidateKind::ControlGroup, std::move(visit),
                              positionOf(replaySelect), controlGroup.selectMechanicalEventIndex,
                              controlGroup.visit.physicalProductionKeys.size(),
                              controlGroup.visit.endTimestampTicks, existingVisitIndex,
                              !existingVisitIndex.has_value()});
    }

    const auto clickCandidates = collectClickCandidates(result, hotkeys, qpcFrequency);
    std::optional<ReplayPosition> previousClickSelection;
    for (const auto& click : clickCandidates) {
        auto clickVisit = makeClickVisit(click, result, qpcFrequency);
        std::optional<std::size_t> bestSelection;
        double bestSelectionDistance = replaySelectionMatchWindowMs + 1.0;
        for (std::size_t selectionIndex = 0; selectionIndex < mappedSelections.size();
             ++selectionIndex) {
            const auto& selection = mappedSelections[selectionIndex];
            if (selection.used ||
                (previousClickSelection &&
                 !positionLess(*previousClickSelection, positionOf(*selection.event))))
                continue;
            const double selectionDistance =
                std::abs(selection.activeMs - click.clickActiveMs);
            if (selectionDistance > replaySelectionMatchWindowMs)
                continue;

            std::optional<ReplayPosition> nextSelection;
            if (selectionIndex + 1 < mappedSelections.size())
                nextSelection = positionOf(*mappedSelections[selectionIndex + 1].event);
            const bool hasCompatibleProduction = std::any_of(
                mapped.begin(), mapped.end(), [&](const MappedProductionEvent& production) {
                    const auto productionPosition = positionOf(*production.event);
                    if (!positionLessOrEqual(positionOf(*selection.event), productionPosition) ||
                        (nextSelection && !positionLess(productionPosition, *nextSelection)) ||
                        distanceToVisit(production.activeMs, clickVisit) >
                            replayProductionMatchWindowMs)
                        return false;
                    return std::any_of(
                        click.physicalKeys.begin(), click.physicalKeys.end(),
                        [&](std::uint16_t key) {
                            return replayProductionCompatibleWithPhysicalKey(
                                *production.event, key, hotkeys);
                        });
                });
            if (!hasCompatibleProduction || selectionDistance >= bestSelectionDistance)
                continue;
            bestSelectionDistance = selectionDistance;
            bestSelection = selectionIndex;
        }
        if (!bestSelection)
            continue;
        auto& selection = mappedSelections[*bestSelection];
        selection.used = true;
        previousClickSelection = positionOf(*selection.event);
        if (selection.event->kind == ReplaySelectionKind::Select) {
            clickVisit.productionContext =
                makeReplaySelectionProductionContext(selection.event->unitTags);
        }
        candidates.push_back({CorrelationCandidateKind::Click, clickVisit,
                              positionOf(*selection.event), click.clickMechanicalEventIndex,
                              static_cast<std::size_t>(click.physicalPresses),
                              click.finalPressTimestampTicks, std::nullopt, false});
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& first, const auto& second) {
        if (first.visit.contextTimestampTicks != second.visit.contextTimestampTicks)
            return first.visit.contextTimestampTicks < second.visit.contextTimestampTicks;
        return positionLess(first.context, second.context);
    });
    std::vector<CorrelationCandidate> orderedCandidates;
    orderedCandidates.reserve(candidates.size());
    for (auto& candidate : candidates) {
        if (!orderedCandidates.empty() &&
            !positionLess(orderedCandidates.back().context, candidate.context))
            continue;
        orderedCandidates.push_back(std::move(candidate));
    }

    std::vector<ReplayPosition> replayContexts;
    replayContexts.reserve(replay.controlGroupSelections.size() + mappedSelections.size());
    for (const auto& selection : replay.controlGroupSelections) {
        if (selection.playerId == playerMatch.playerId)
            replayContexts.push_back(positionOf(selection));
    }
    for (const auto& selection : mappedSelections)
        replayContexts.push_back(positionOf(*selection.event));
    std::sort(replayContexts.begin(), replayContexts.end(), positionLess);
    replayContexts.erase(
        std::unique(replayContexts.begin(), replayContexts.end(),
                    [](ReplayPosition first, ReplayPosition second) {
                        return !positionLess(first, second) && !positionLess(second, first);
                    }),
        replayContexts.end());

    for (std::size_t candidateIndex = 0; candidateIndex < orderedCandidates.size();
         ++candidateIndex) {
        auto& candidate = orderedCandidates[candidateIndex];
        const auto nextContextIterator = std::find_if(
            replayContexts.begin(), replayContexts.end(), [&](ReplayPosition context) {
                return positionLess(candidate.context, context);
            });
        const std::optional<ReplayPosition> nextContext =
            nextContextIterator == replayContexts.end()
                ? std::nullopt
                : std::optional<ReplayPosition>(*nextContextIterator);
        std::vector<const ReplayProductionEvent*> assigned;
        for (auto& production : mapped) {
            if (production.used ||
                !positionLessOrEqual(candidate.context, positionOf(*production.event)) ||
                (nextContext && !positionLess(positionOf(*production.event), *nextContext)) ||
                distanceToVisit(production.activeMs, candidate.visit) >
                    replayProductionMatchWindowMs)
                continue;
            const bool compatible = std::any_of(
                candidate.visit.physicalProductionKeys.begin(),
                candidate.visit.physicalProductionKeys.end(), [&](std::uint16_t key) {
                    return replayProductionCompatibleWithPhysicalKey(*production.event, key,
                                                                     hotkeys);
                });
            if (!compatible)
                continue;
            assigned.push_back(production.event);
            if (assigned.size() >= candidate.shortWindowPhysicalPresses)
                break;
        }
        if (assigned.empty())
            continue;

        const auto physicalPresses = collectConfirmedPhysicalBurst(
            result, hotkeys, qpcFrequency, candidate.physicalContextEventIndex, assigned);
        if (physicalPresses.empty())
            continue;
        applyConfirmedPhysicalBurst(candidate.visit, result, physicalPresses, qpcFrequency);

        assigned.erase(
            std::remove_if(assigned.begin(), assigned.end(), [&](const auto* replayEvent) {
                return std::none_of(
                    candidate.visit.physicalProductionKeys.begin(),
                    candidate.visit.physicalProductionKeys.end(), [&](std::uint16_t key) {
                        return replayProductionCompatibleWithPhysicalKey(*replayEvent, key,
                                                                         hotkeys);
                    });
            }),
            assigned.end());
        if (assigned.empty())
            continue;
        if (assigned.size() > physicalPresses.size())
            assigned.resize(physicalPresses.size());

        for (auto& production : mapped) {
            if (production.used ||
                std::find(assigned.begin(), assigned.end(), production.event) != assigned.end() ||
                !positionLessOrEqual(candidate.context, positionOf(*production.event)) ||
                (nextContext && !positionLess(positionOf(*production.event), *nextContext)) ||
                distanceToVisit(production.activeMs, candidate.visit) >
                    replayProductionMatchWindowMs)
                continue;
            const bool compatible = std::any_of(
                candidate.visit.physicalProductionKeys.begin(),
                candidate.visit.physicalProductionKeys.end(), [&](std::uint16_t key) {
                    return replayProductionCompatibleWithPhysicalKey(*production.event, key,
                                                                     hotkeys);
                });
            if (!compatible)
                continue;
            assigned.push_back(production.event);
            if (assigned.size() >= physicalPresses.size())
                break;
        }

        for (auto& production : mapped) {
            if (std::find(assigned.begin(), assigned.end(), production.event) != assigned.end())
                production.used = true;
        }
        if (candidate.visit.endTimestampTicks > candidate.shortWindowEndTimestampTicks &&
            physicalPresses.size() > candidate.shortWindowPhysicalPresses) {
            ++analysis.replayCorrelation.extendedProductionVisits;
            analysis.replayCorrelation.extendedPhysicalProductionPresses +=
                physicalPresses.size() - candidate.shortWindowPhysicalPresses;
        }

        if (candidate.existingVisitIndex) {
            auto& existing = analysis.productionVisits[*candidate.existingVisitIndex];
            existing = candidate.visit;
            applyReplayEvents(existing, assigned);
        } else {
            applyReplayEvents(candidate.visit, assigned);
            analysis.productionVisits.push_back(std::move(candidate.visit));
            if (candidate.replayCreatedControlGroup)
                ++analysis.replayCorrelation.replayCreatedControlGroupVisits;
            else
                ++analysis.replayCorrelation.matchedClickVisits;
        }
        analysis.replayCorrelation.matchedReplayProductionEvents += assigned.size();
    }
    analysis.replayCorrelation.unmatchedReplayProductionEvents =
        static_cast<std::size_t>(std::count_if(mapped.begin(), mapped.end(),
                                               [](const auto& event) { return !event.used; }));
    std::sort(analysis.productionVisits.begin(), analysis.productionVisits.end(),
              [](const ProductionVisit& first, const ProductionVisit& second) {
                  if (first.contextTimestampTicks != second.contextTimestampTicks)
                      return first.contextTimestampTicks < second.contextTimestampTicks;
                  if (first.endTimestampTicks != second.endTimestampTicks)
                      return first.endTimestampTicks < second.endTimestampTicks;
                  return first.startTimestampTicks < second.startTimestampTicks;
              });
    annotateProductionAccessTelemetry(analysis.productionVisits, result);

    analysis.replayCorrelation.available = true;
    analysis.replayCorrelation.playerId = playerMatch.playerId;
    analysis.replayCorrelation.playerName = playerMatch.playerName;
    analysis.replayCorrelation.sequenceScore = playerMatch.sequenceScore;
    analysis.replayCorrelation.runnerUpSequenceScore = playerMatch.runnerUpSequenceScore;
    analysis.replayCorrelation.matchedControlGroupEvents = playerMatch.matchedEventIndices.size();
    analysis.replayCorrelation.timelineAnchors = anchors.size();
    analysis.replayCorrelation.parser = std::move(parserName);
    for (const auto& visit : analysis.productionVisits) {
        if (visit.replayConfirmed)
            ++analysis.replayCorrelation.matchedProductionVisits;
        else
            ++analysis.replayCorrelation.unmatchedProductionVisits;
    }
    analysis.workerMacroCycles =
        groupProductionVisits(analysis.productionVisits, MacroProductType::Worker,
                              result.mechanicalEvents, qpcFrequency);
    analysis.armyMacroCycles =
        groupProductionVisits(analysis.productionVisits, MacroProductType::Army,
                              result.mechanicalEvents, qpcFrequency);
    correlateArmyControlGroupManagement(analysis.armyControlGroupManagement, result,
                                        qpcFrequency, replay, playerMatch.playerId,
                                        anchors);
    return analysis;
}

} // namespace smp
