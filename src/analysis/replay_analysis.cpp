#include "analysis/replay_analysis.h"

#include "platform/resource_ids.h"
#include "util/json.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace smp {
namespace {

constexpr std::size_t maximumAlignedControlGroupEvents = 4000;
constexpr double minimumPlayerSequenceScore = 0.65;
constexpr double minimumPlayerScoreLead = 0.10;
constexpr DWORD replayParserTimeoutMs = 30000;
constexpr const wchar_t* bundledParserFilename = L"screp-v1.11.3.exe";

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

struct ClickCandidate {
    double clickActiveMs{};
    std::uint64_t clickTimestampTicks{};
    double finalPressActiveMs{};
    std::uint64_t finalPressTimestampTicks{};
    int physicalPresses{};
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
    if (eventActiveMs < visit.startActiveMs)
        return visit.startActiveMs - eventActiveMs;
    if (eventActiveMs > visit.endActiveMs)
        return eventActiveMs - visit.endActiveMs;
    return 0.0;
}

double distanceToClickCandidate(double eventActiveMs, const ClickCandidate& candidate) noexcept {
    if (eventActiveMs < candidate.clickActiveMs)
        return candidate.clickActiveMs - eventActiveMs;
    if (eventActiveMs > candidate.finalPressActiveMs)
        return eventActiveMs - candidate.finalPressActiveMs;
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
        ClickCandidate candidate{click.activeMs, click.timestampTicks, click.activeMs,
                                 click.timestampTicks, 0};
        for (std::size_t index = clickIndex + 1; index < events.size(); ++index) {
            const auto& event = events[index];
            const auto realElapsed = qpcElapsedMs(click.timestampTicks, event.timestampTicks, qpcFrequency);
            const double activeElapsed = event.activeMs - click.activeMs;
            if (!realElapsed || activeElapsed < 0.0 || *realElapsed > productionVisitWindowMs ||
                activeElapsed > productionVisitWindowMs ||
                *realElapsed - activeElapsed > qpcActivePauseToleranceMs)
                break;
            if (event.type == MechanicalInputType::MouseLeftUp)
                continue;
            if (isProductionPress(event, hotkeys)) {
                ++candidate.physicalPresses;
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
    visit.startActiveMs = candidate.clickActiveMs;
    visit.startTimestampTicks = candidate.clickTimestampTicks;
    visit.endActiveMs = candidate.finalPressActiveMs;
    visit.endTimestampTicks = candidate.finalPressTimestampTicks;
    visit.physicalProductionPresses = candidate.physicalPresses;

    struct Access {
        double activeMs{};
        std::uint64_t timestampTicks{};
        ProductionAccessMethod method{ProductionAccessMethod::ScreenClick};
        int location{-1};
    };
    std::optional<Access> mostRecent;
    for (const auto& event : result.mechanicalEvents) {
        if (event.type != MechanicalInputType::LocationRecall ||
            event.timestampTicks > candidate.clickTimestampTicks)
            continue;
        const auto realGap = qpcElapsedMs(event.timestampTicks, candidate.clickTimestampTicks, qpcFrequency);
        const double activeGap = candidate.clickActiveMs - event.activeMs;
        if (realGap && activeGap >= 0.0 && *realGap <= productionAccessNavigationWindowMs &&
            *realGap - activeGap <= qpcActivePauseToleranceMs &&
            (!mostRecent || event.timestampTicks > mostRecent->timestampTicks))
            mostRecent = Access{event.activeMs, event.timestampTicks,
                                ProductionAccessMethod::LocationHotkeyClick, event.value};
    }
    for (const auto& event : result.navigationEvents) {
        if (event.type != CameraNavigationType::MinimapJump ||
            event.timestampTicks >= candidate.clickTimestampTicks)
            continue;
        const auto realGap = qpcElapsedMs(event.timestampTicks, candidate.clickTimestampTicks, qpcFrequency);
        const double activeGap = candidate.clickActiveMs - event.activeMs;
        if (realGap && activeGap >= 0.0 && *realGap <= productionAccessNavigationWindowMs &&
            *realGap - activeGap <= qpcActivePauseToleranceMs &&
            (!mostRecent || event.timestampTicks > mostRecent->timestampTicks))
            mostRecent = Access{event.activeMs, event.timestampTicks,
                                ProductionAccessMethod::MinimapClick, -1};
    }
    if (mostRecent) {
        visit.accessMethod = mostRecent->method;
        visit.startActiveMs = mostRecent->activeMs;
        visit.startTimestampTicks = mostRecent->timestampTicks;
        visit.locationHotkey = mostRecent->location;
    }
    visit.durationMs = qpcElapsedMs(visit.startTimestampTicks, visit.endTimestampTicks, qpcFrequency)
                           .value_or(std::max(0.0, visit.endActiveMs - visit.startActiveMs));
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

std::filesystem::path extractBundledParser() {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_SCREP_BIN), RT_RCDATA);
    if (!resource)
        throw std::runtime_error("The bundled replay parser resource is missing");
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD resourceSize = SizeofResource(module, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || resourceSize == 0)
        throw std::runtime_error("The bundled replay parser resource is invalid");

    const auto destination = localParserPath();
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
               const std::filesystem::path& output) {
    std::wstring command = quoted(parser) +
                           L" -cmds -computed=false -header=true -map=false -indent=false -outfile " +
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
    const DWORD wait = WaitForSingleObject(process.hProcess, replayParserTimeoutMs);
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
    for (const auto& playerValue : root["Header"]["Players"].asArray()) {
        const int id = playerValue["ID"].asInt(-1);
        if (id >= 0)
            replay.players.push_back({id, playerValue["Name"].asString()});
    }
    for (const auto& command : root["Commands"]["Cmds"].asArray()) {
        const auto frame = static_cast<std::int64_t>(command["Frame"].asNumber(-1.0));
        const int playerId = command["PlayerID"].asInt(-1);
        const auto type = command["Type"]["Name"].asString();
        if (frame < 0 || playerId < 0)
            continue;
        if (type == "Hotkey" && command["HotkeyType"]["Name"].asString() == "Select") {
            const int group = command["Group"].asInt(-1);
            if (group >= 0 && group <= 9)
                replay.controlGroupSelections.push_back({frame, playerId, group});
            continue;
        }
        ReplayProductionEvent production;
        production.replayFrame = frame;
        production.playerId = playerId;
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

ReplayExtractionResult extractReplayWithBundledScrep(const std::filesystem::path& replayPath) noexcept {
    ReplayExtractionResult result;
    result.parser = "screp-v1.11.3";
    try {
        if (!std::filesystem::is_regular_file(replayPath))
            throw std::runtime_error("Replay file is missing");
        const auto parser = extractBundledParser();
        const auto output = replayParserOutputPath();
        ScopedPathRemoval removeOutput(output);
        runParser(parser, replayPath, output);
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

    std::vector<MappedProductionEvent> mapped;
    for (const auto& event : replay.productionEvents) {
        if (event.playerId == playerMatch.playerId &&
            classifyReplayProduction(event) != MacroProductType::Unknown)
            mapped.push_back({&event, replayFrameToActiveMs(event.replayFrame, anchors), false});
    }

    std::vector<std::vector<const ReplayProductionEvent*>> assigned(analysis.productionVisits.size());
    for (auto& mappedEvent : mapped) {
        std::optional<std::size_t> bestVisit;
        double bestDistance = replayProductionMatchWindowMs + 1.0;
        for (std::size_t index = 0; index < analysis.productionVisits.size(); ++index) {
            const double distance = distanceToVisit(mappedEvent.activeMs, analysis.productionVisits[index]);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestVisit = index;
            }
        }
        if (bestVisit && bestDistance <= replayProductionMatchWindowMs) {
            assigned[*bestVisit].push_back(mappedEvent.event);
            mappedEvent.used = true;
        }
    }
    for (std::size_t index = 0; index < analysis.productionVisits.size(); ++index)
        applyReplayEvents(analysis.productionVisits[index], assigned[index]);

    const auto clickCandidates = collectClickCandidates(result, hotkeys, qpcFrequency);
    std::vector<std::vector<const ReplayProductionEvent*>> clickAssignments(clickCandidates.size());
    for (auto& mappedEvent : mapped) {
        if (mappedEvent.used)
            continue;
        std::optional<std::size_t> bestCandidate;
        double bestDistance = replayProductionMatchWindowMs + 1.0;
        for (std::size_t index = 0; index < clickCandidates.size(); ++index) {
            const double distance = distanceToClickCandidate(mappedEvent.activeMs, clickCandidates[index]);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestCandidate = index;
            }
        }
        if (bestCandidate && bestDistance <= replayProductionMatchWindowMs) {
            clickAssignments[*bestCandidate].push_back(mappedEvent.event);
            mappedEvent.used = true;
        }
    }
    for (std::size_t index = 0; index < clickCandidates.size(); ++index) {
        if (clickAssignments[index].empty())
            continue;
        auto visit = makeClickVisit(clickCandidates[index], result, qpcFrequency);
        applyReplayEvents(visit, clickAssignments[index]);
        analysis.productionVisits.push_back(std::move(visit));
    }
    std::sort(analysis.productionVisits.begin(), analysis.productionVisits.end(),
              [](const ProductionVisit& first, const ProductionVisit& second) {
                  if (first.startTimestampTicks != second.startTimestampTicks)
                      return first.startTimestampTicks < second.startTimestampTicks;
                  return first.endTimestampTicks < second.endTimestampTicks;
              });

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
        groupProductionVisits(analysis.productionVisits, MacroProductType::Worker, qpcFrequency);
    analysis.armyMacroCycles =
        groupProductionVisits(analysis.productionVisits, MacroProductType::Army, qpcFrequency);
    return analysis;
}

} // namespace smp
