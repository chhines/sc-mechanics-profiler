#include "analysis/production_visit.h"

#include "util/json.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace smp {
namespace {

constexpr const char* unreadableHotkeysReason = "StarCraft hotkey configuration could not be read";

struct ControlGroupVisit {
    int group{};
    std::uint32_t assignmentGeneration{};
    std::size_t selectEventIndex{};
    double selectActiveMs{};
    std::uint64_t selectTimestampTicks{};
    std::vector<std::size_t> productionPressIndices;
    bool mouseContradiction{};
};

struct KeyEvidence {
    int visits{};
    int presses{};
};

struct GroupEvidence {
    int candidateVisits{};
    int cleanVisits{};
    int cleanPresses{};
    int mouseContradictions{};
    std::unordered_map<std::uint16_t, KeyEvidence> keys;
};

struct CameraEpisode {
    std::uint64_t id{};
    ProductionCameraAccess access{ProductionCameraAccess::None};
    ProductionCameraAnchorKind anchorKind{ProductionCameraAnchorKind::None};
    int anchorId{-1};
    std::uint64_t anchorTimestampTicks{};
};

enum class VisitAccessTechnique : std::uint8_t {
    ControlGroup,
    LocationHotkeyClick,
    ControlGroupCenterClick,
    Other
};

std::string trim(std::string value) {
    const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string normalizedProductionName(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch))
            normalized.push_back(static_cast<char>(std::toupper(ch)));
    }
    return normalized;
}

std::uint16_t hotkeyVirtualKey(const std::string& boundKey) {
    if (boundKey.size() == 1) {
        const auto ch = static_cast<unsigned char>(boundKey.front());
        if (std::isalnum(ch))
            return static_cast<std::uint16_t>(std::toupper(ch));
    }
    std::string upper;
    upper.reserve(boundKey.size());
    for (const unsigned char ch : boundKey)
        upper.push_back(static_cast<char>(std::toupper(ch)));
    if (upper.size() >= 2 && upper.front() == 'F') {
        try {
            const int number = std::stoi(upper.substr(1));
            if (number >= 1 && number <= 24)
                return static_cast<std::uint16_t>(VK_F1 + number - 1);
        } catch (...) {
        }
    }
    return 0;
}

MacroHotkeyProfile unavailableProfile(std::string reason,
                                      std::optional<bool> customHotkeysEnabled = std::nullopt) {
    MacroHotkeyProfile profile;
    profile.unavailableReason = std::move(reason);
    profile.customHotkeysEnabled = customHotkeysEnabled;
    return profile;
}

bool startsWith(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool isStandaloneModifier(std::uint16_t virtualKey) noexcept {
    return virtualKey == VK_CONTROL || virtualKey == VK_LCONTROL || virtualKey == VK_RCONTROL ||
           virtualKey == VK_SHIFT || virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT ||
           virtualKey == VK_MENU || virtualKey == VK_LMENU || virtualKey == VK_RMENU;
}

bool isMouseActivity(MechanicalInputType type) noexcept {
    return type == MechanicalInputType::MouseLeftDown || type == MechanicalInputType::MouseLeftUp ||
           type == MechanicalInputType::MouseRightDown || type == MechanicalInputType::MouseRightUp ||
           type == MechanicalInputType::MouseMiddleDown || type == MechanicalInputType::MouseMiddleUp ||
           type == MechanicalInputType::MouseWheel;
}

std::optional<double> qpcElapsedMs(std::uint64_t start, std::uint64_t end,
                                   std::uint64_t frequency) noexcept {
    if (frequency == 0 || end < start)
        return std::nullopt;
    const long double milliseconds = static_cast<long double>(end - start) * 1000.0L /
                                     static_cast<long double>(frequency);
    if (milliseconds > static_cast<long double>(std::numeric_limits<double>::max()))
        return std::nullopt;
    return static_cast<double>(milliseconds);
}

bool withinVisitWindow(const ControlGroupVisit& visit, const MechanicalInputEvent& event,
                       std::uint64_t qpcFrequency) noexcept {
    const auto realElapsed = qpcElapsedMs(visit.selectTimestampTicks, event.timestampTicks, qpcFrequency);
    const double activeElapsed = event.activeMs - visit.selectActiveMs;
    return realElapsed && *realElapsed <= productionVisitWindowMs && activeElapsed >= 0.0 &&
           activeElapsed <= productionVisitWindowMs &&
           *realElapsed - activeElapsed <= qpcActivePauseToleranceMs;
}

bool productionCompatiblePress(const MechanicalInputEvent& event,
                               const MacroHotkeyProfile& hotkeys) {
    // Shift-modified production is intentionally excluded until its physical-command semantics are validated.
    if (event.type != MechanicalInputType::KeyPress ||
        (event.modifiers & (ModifierCtrl | ModifierShift | ModifierAlt)) != 0 ||
        isStandaloneModifier(event.virtualKey))
        return false;
    return !hotkeys.compatibleProductionCommands(event.virtualKey).empty();
}

std::vector<ControlGroupVisit> collectControlGroupVisits(const std::vector<MechanicalInputEvent>& events,
                                                         const MacroHotkeyProfile& hotkeys,
                                                         std::uint64_t qpcFrequency) {
    std::vector<ControlGroupVisit> visits;
    std::optional<ControlGroupVisit> current;
    // Generation zero means no assignment has been observed in this captured session.
    std::array<std::uint32_t, 10> assignmentGenerations{};
    const auto finish = [&]() {
        if (current) {
            visits.push_back(std::move(*current));
            current.reset();
        }
    };

    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (current && !withinVisitWindow(*current, event, qpcFrequency))
            finish();

        if (event.type == MechanicalInputType::ControlGroupSelect) {
            finish();
            if (event.value >= 0 && event.value <= 9) {
                current = ControlGroupVisit{
                    event.value,
                    assignmentGenerations[static_cast<std::size_t>(event.value)],
                    index,
                    event.activeMs,
                    event.timestampTicks};
            }
            continue;
        }
        if (event.type == MechanicalInputType::ControlGroupAssign ||
            event.type == MechanicalInputType::ControlGroupAdd) {
            finish();
            if (event.value >= 0 && event.value <= 9)
                ++assignmentGenerations[static_cast<std::size_t>(event.value)];
            continue;
        }
        if (!current)
            continue;

        if (event.type == MechanicalInputType::KeyPress) {
            if (isStandaloneModifier(event.virtualKey))
                continue;
            if (productionCompatiblePress(event, hotkeys)) {
                current->productionPressIndices.push_back(index);
                continue;
            }
            finish();
            continue;
        }
        if (isMouseActivity(event.type)) {
            if (!current->productionPressIndices.empty())
                current->mouseContradiction = true;
            finish();
            continue;
        }
        if (event.type == MechanicalInputType::LocationRecall ||
            event.type == MechanicalInputType::LocationAssign) {
            finish();
        }
    }
    finish();
    return visits;
}

std::unordered_map<int, std::unordered_set<std::uint16_t>>
learnedKeyMap(const std::vector<LikelyProductionGroup>& likelyGroups) {
    std::unordered_map<int, std::unordered_set<std::uint16_t>> learned;
    for (const auto& group : likelyGroups)
        learned[group.group] = {group.observedProductionKeys.begin(), group.observedProductionKeys.end()};
    return learned;
}

void markUnavailable(ProductMacroCycleAnalysis& cycles, MacroProductType type,
                     const std::string& reason) {
    cycles = {};
    cycles.productType = type;
    cycles.unavailableReason = reason;
}

std::size_t accessMethodIndex(ProductionAccessMethod method) noexcept {
    return static_cast<std::size_t>(method);
}

const MechanicalInputEvent* firstAssignmentInterruption(
    const std::vector<MechanicalInputEvent>& events, std::uint64_t previousVisitEnd,
    std::uint64_t nextVisitContext) noexcept {
    if (previousVisitEnd >= nextVisitContext)
        return nullptr;
    const auto first = std::upper_bound(
        events.begin(), events.end(), previousVisitEnd,
        [](std::uint64_t timestamp, const MechanicalInputEvent& event) {
            return timestamp < event.timestampTicks;
        });
    for (auto event = first;
         event != events.end() && event->timestampTicks < nextVisitContext; ++event) {
        if (event->type == MechanicalInputType::ControlGroupAssign ||
            event->type == MechanicalInputType::ControlGroupAdd ||
            event->type == MechanicalInputType::LocationAssign)
            return &*event;
    }
    return nullptr;
}

MacroAccessStyle deriveMacroAccessStyle(MacroCycle& cycle,
                                        const std::vector<ProductionVisit>& visits) {
    cycle.controlGroupVisitCount = 0;
    cycle.directClickVisitCount = 0;
    cycle.boxSelectVisitCount = 0;
    cycle.cameraEpisodeCount = 0;

    std::unordered_set<std::uint64_t> cameraEpisodes;
    std::unordered_set<std::uint64_t> centerClickEpisodes;
    for (const auto visitIndex : cycle.visitIndices) {
        if (visitIndex >= visits.size())
            continue;
        const auto& visit = visits[visitIndex];
        if (visit.cameraEpisodeId != 0)
            cameraEpisodes.insert(visit.cameraEpisodeId);
        if ((visit.selectionAccess == ProductionSelectionAccess::DirectClick ||
             visit.selectionAccess == ProductionSelectionAccess::BoxSelect) &&
            visit.cameraEpisodeId != 0 &&
            visit.cameraAccess == ProductionCameraAccess::ControlGroupDoubleTap &&
            visit.cameraAnchorKind == ProductionCameraAnchorKind::ControlGroup)
            centerClickEpisodes.insert(visit.cameraEpisodeId);
    }
    cycle.cameraEpisodeCount = cameraEpisodes.size();

    std::array<bool, 4> techniques{};
    for (const auto visitIndex : cycle.visitIndices) {
        if (visitIndex >= visits.size()) {
            techniques[static_cast<std::size_t>(VisitAccessTechnique::Other)] = true;
            continue;
        }
        const auto& visit = visits[visitIndex];
        VisitAccessTechnique technique = VisitAccessTechnique::Other;
        switch (visit.selectionAccess) {
        case ProductionSelectionAccess::ControlGroup:
            ++cycle.controlGroupVisitCount;
            technique = visit.cameraEpisodeId != 0 &&
                                centerClickEpisodes.contains(visit.cameraEpisodeId)
                            ? VisitAccessTechnique::ControlGroupCenterClick
                            : VisitAccessTechnique::ControlGroup;
            break;
        case ProductionSelectionAccess::DirectClick:
            ++cycle.directClickVisitCount;
            if (visit.cameraEpisodeId != 0 &&
                visit.cameraAccess == ProductionCameraAccess::LocationHotkey &&
                visit.cameraAnchorKind == ProductionCameraAnchorKind::LocationHotkey)
                technique = VisitAccessTechnique::LocationHotkeyClick;
            else if (visit.cameraEpisodeId != 0 &&
                     visit.cameraAccess == ProductionCameraAccess::ControlGroupDoubleTap &&
                     visit.cameraAnchorKind == ProductionCameraAnchorKind::ControlGroup)
                technique = VisitAccessTechnique::ControlGroupCenterClick;
            break;
        case ProductionSelectionAccess::BoxSelect:
            ++cycle.boxSelectVisitCount;
            if (visit.cameraEpisodeId != 0 &&
                visit.cameraAccess == ProductionCameraAccess::LocationHotkey &&
                visit.cameraAnchorKind == ProductionCameraAnchorKind::LocationHotkey)
                technique = VisitAccessTechnique::LocationHotkeyClick;
            else if (visit.cameraEpisodeId != 0 &&
                     visit.cameraAccess == ProductionCameraAccess::ControlGroupDoubleTap &&
                     visit.cameraAnchorKind == ProductionCameraAnchorKind::ControlGroup)
                technique = VisitAccessTechnique::ControlGroupCenterClick;
            break;
        case ProductionSelectionAccess::Other:
            break;
        }
        techniques[static_cast<std::size_t>(technique)] = true;
    }

    const auto techniqueCount = static_cast<std::size_t>(
        std::count(techniques.begin(), techniques.end(), true));
    if (techniqueCount != 1)
        return techniqueCount > 1 ? MacroAccessStyle::Mixed : MacroAccessStyle::Other;
    if (techniques[static_cast<std::size_t>(VisitAccessTechnique::ControlGroup)])
        return MacroAccessStyle::ControlGroupOnly;
    if (techniques[static_cast<std::size_t>(VisitAccessTechnique::LocationHotkeyClick)])
        return MacroAccessStyle::LocationHotkeyClick;
    if (techniques[static_cast<std::size_t>(VisitAccessTechnique::ControlGroupCenterClick)])
        return MacroAccessStyle::ControlGroupCenterClick;
    return MacroAccessStyle::Other;
}

} // namespace

const char* macroProductTypeName(MacroProductType type) noexcept {
    switch (type) {
    case MacroProductType::Worker:
        return "worker";
    case MacroProductType::Army:
        return "army";
    case MacroProductType::Unknown:
        return "unknown";
    }
    return "unknown";
}

const char* productionAccessMethodName(ProductionAccessMethod method) noexcept {
    switch (method) {
    case ProductionAccessMethod::ControlGroup:
        return "control_group";
    case ProductionAccessMethod::LocationHotkeyClick:
        return "location_hotkey_click";
    case ProductionAccessMethod::MinimapClick:
        return "minimap_click";
    case ProductionAccessMethod::ScreenClick:
        return "screen_click";
    }
    return "screen_click";
}

const char* productionSelectionAccessName(ProductionSelectionAccess access) noexcept {
    switch (access) {
    case ProductionSelectionAccess::ControlGroup:
        return "control_group";
    case ProductionSelectionAccess::DirectClick:
        return "direct_click";
    case ProductionSelectionAccess::BoxSelect:
        return "box_select";
    case ProductionSelectionAccess::Other:
        return "other";
    }
    return "other";
}

const char* productionCameraAccessName(ProductionCameraAccess access) noexcept {
    switch (access) {
    case ProductionCameraAccess::None:
        return "none";
    case ProductionCameraAccess::LocationHotkey:
        return "location_hotkey";
    case ProductionCameraAccess::ControlGroupDoubleTap:
        return "control_group_double_tap";
    case ProductionCameraAccess::Minimap:
        return "minimap";
    case ProductionCameraAccess::EdgeScroll:
        return "edge_scroll";
    case ProductionCameraAccess::Other:
        return "other";
    }
    return "other";
}

const char* productionCameraAnchorKindName(ProductionCameraAnchorKind kind) noexcept {
    switch (kind) {
    case ProductionCameraAnchorKind::None:
        return "none";
    case ProductionCameraAnchorKind::LocationHotkey:
        return "location_hotkey";
    case ProductionCameraAnchorKind::ControlGroup:
        return "control_group";
    case ProductionCameraAnchorKind::Minimap:
        return "minimap";
    case ProductionCameraAnchorKind::EdgeScroll:
        return "edge_scroll";
    case ProductionCameraAnchorKind::Other:
        return "other";
    }
    return "other";
}

const char* macroAccessStyleName(MacroAccessStyle style) noexcept {
    switch (style) {
    case MacroAccessStyle::ControlGroupOnly:
        return "control_group_only";
    case MacroAccessStyle::LocationHotkeyClick:
        return "location_hotkey_click";
    case MacroAccessStyle::ControlGroupCenterClick:
        return "control_group_center_click";
    case MacroAccessStyle::Mixed:
        return "mixed";
    case MacroAccessStyle::Other:
        return "other";
    }
    return "other";
}

std::size_t macroAccessStyleIndex(MacroAccessStyle style) noexcept {
    return static_cast<std::size_t>(style);
}

const char* productionContextKindName(ProductionContextKind kind) noexcept {
    switch (kind) {
    case ProductionContextKind::ReplaySelection:
        return "replay_selection";
    case ProductionContextKind::ControlGroup:
        return "control_group";
    case ProductionContextKind::LocationHotkey:
        return "location_hotkey";
    case ProductionContextKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

ProductionContextId
makeReplaySelectionProductionContext(std::vector<std::uint32_t> unitTags) {
    std::sort(unitTags.begin(), unitTags.end());
    unitTags.erase(std::unique(unitTags.begin(), unitTags.end()), unitTags.end());
    if (unitTags.empty())
        return {};
    ProductionContextId context;
    context.kind = ProductionContextKind::ReplaySelection;
    context.unitTags = std::move(unitTags);
    return context;
}

ProductionContextId makeControlGroupProductionContext(
    int controlGroup, std::uint32_t assignmentGeneration) noexcept {
    if (controlGroup < 0 || controlGroup > 9)
        return {};
    ProductionContextId context;
    context.kind = ProductionContextKind::ControlGroup;
    context.controlGroup = controlGroup;
    context.assignmentGeneration = assignmentGeneration;
    return context;
}

ProductionContextId makeLocationHotkeyProductionContext(
    int locationHotkey, std::uint32_t assignmentGeneration) noexcept {
    if (locationHotkey < 0)
        return {};
    ProductionContextId context;
    context.kind = ProductionContextKind::LocationHotkey;
    context.locationHotkey = locationHotkey;
    context.assignmentGeneration = assignmentGeneration;
    return context;
}

bool knownProductionContext(const ProductionContextId& context) noexcept {
    switch (context.kind) {
    case ProductionContextKind::ReplaySelection:
        return !context.unitTags.empty();
    case ProductionContextKind::ControlGroup:
        return context.controlGroup >= 0 && context.controlGroup <= 9;
    case ProductionContextKind::LocationHotkey:
        return context.locationHotkey >= 0;
    case ProductionContextKind::Unknown:
        return false;
    }
    return false;
}

bool sameProductionContext(const ProductionContextId& first,
                           const ProductionContextId& second) noexcept {
    if (!knownProductionContext(first) || !knownProductionContext(second) ||
        first.kind != second.kind)
        return false;
    switch (first.kind) {
    case ProductionContextKind::ReplaySelection:
        return first.unitTags == second.unitTags;
    case ProductionContextKind::ControlGroup:
        return first.controlGroup == second.controlGroup &&
               first.assignmentGeneration == second.assignmentGeneration;
    case ProductionContextKind::LocationHotkey:
        return first.locationHotkey == second.locationHotkey &&
               first.assignmentGeneration == second.assignmentGeneration;
    case ProductionContextKind::Unknown:
        return false;
    }
    return false;
}

void refreshProductionVisitTiming(ProductionVisit& visit,
                                  std::uint64_t qpcFrequency) noexcept {
    const auto span = [&](std::uint64_t startTicks, std::uint64_t endTicks,
                          double startActiveMs, double endActiveMs) {
        return qpcElapsedMs(startTicks, endTicks, qpcFrequency)
            .value_or(std::max(0.0, endActiveMs - startActiveMs));
    };
    visit.accessLatencyMs =
        span(visit.startTimestampTicks, visit.contextTimestampTicks,
             visit.startActiveMs, visit.contextActiveMs);
    visit.productionLatencyMs =
        span(visit.contextTimestampTicks, visit.firstProductionTimestampTicks,
             visit.contextActiveMs, visit.firstProductionActiveMs);
    visit.executionDurationMs =
        span(visit.startTimestampTicks, visit.firstProductionTimestampTicks,
             visit.startActiveMs, visit.firstProductionActiveMs);
    visit.productionBurstSpanMs =
        span(visit.firstProductionTimestampTicks, visit.endTimestampTicks,
             visit.firstProductionActiveMs, visit.endActiveMs);
    visit.durationMs = span(visit.startTimestampTicks, visit.endTimestampTicks,
                            visit.startActiveMs, visit.endActiveMs);
}

void annotateProductionAccessTelemetry(std::vector<ProductionVisit>& visits,
                                       const AnalysisResult& result) {
    std::vector<CameraEpisode> episodes;
    episodes.reserve(result.navigationEvents.size() + result.recenters.size());
    for (const auto& event : result.navigationEvents) {
        CameraEpisode episode;
        episode.anchorId = event.id;
        episode.anchorTimestampTicks = event.timestampTicks;
        switch (event.type) {
        case CameraNavigationType::ControlGroupJump:
            episode.access = ProductionCameraAccess::ControlGroupDoubleTap;
            episode.anchorKind = ProductionCameraAnchorKind::ControlGroup;
            break;
        case CameraNavigationType::LocationHotkey:
            episode.access = ProductionCameraAccess::LocationHotkey;
            episode.anchorKind = ProductionCameraAnchorKind::LocationHotkey;
            break;
        case CameraNavigationType::MinimapJump:
            episode.access = ProductionCameraAccess::Minimap;
            episode.anchorKind = ProductionCameraAnchorKind::Minimap;
            break;
        case CameraNavigationType::EdgeScroll:
            episode.access = ProductionCameraAccess::EdgeScroll;
            episode.anchorKind = ProductionCameraAnchorKind::EdgeScroll;
            break;
        }
        episodes.push_back(episode);
    }
    for (const auto& event : result.recenters) {
        CameraEpisode episode;
        episode.anchorId = event.id;
        episode.anchorTimestampTicks = event.timestampTicks;
        if (event.type == CameraRecenterType::ControlGroup) {
            episode.access = ProductionCameraAccess::ControlGroupDoubleTap;
            episode.anchorKind = ProductionCameraAnchorKind::ControlGroup;
        } else {
            episode.access = ProductionCameraAccess::LocationHotkey;
            episode.anchorKind = ProductionCameraAnchorKind::LocationHotkey;
        }
        episodes.push_back(episode);
    }
    for (const auto& event : result.mechanicalEvents) {
        if (event.type != MechanicalInputType::LocationRecall || event.value < 0)
            continue;
        episodes.push_back({0, ProductionCameraAccess::LocationHotkey,
                            ProductionCameraAnchorKind::LocationHotkey, event.value,
                            event.timestampTicks});
    }
    std::stable_sort(episodes.begin(), episodes.end(),
                     [](const CameraEpisode& first, const CameraEpisode& second) {
                         return first.anchorTimestampTicks < second.anchorTimestampTicks;
                     });
    episodes.erase(std::unique(episodes.begin(), episodes.end(),
                               [](const CameraEpisode& first, const CameraEpisode& second) {
                                   return first.anchorTimestampTicks == second.anchorTimestampTicks &&
                                          first.access == second.access &&
                                          first.anchorKind == second.anchorKind &&
                                          first.anchorId == second.anchorId;
                               }),
                   episodes.end());
    for (std::size_t index = 0; index < episodes.size(); ++index)
        episodes[index].id = index + 1;

    for (auto& visit : visits) {
        visit.cameraAccess = ProductionCameraAccess::None;
        visit.cameraAnchorKind = ProductionCameraAnchorKind::None;
        visit.cameraEpisodeId = 0;
        visit.cameraAnchorId = -1;
        visit.cameraAnchorTimestampTicks = 0;

        const auto afterVisit = std::upper_bound(
            episodes.begin(), episodes.end(), visit.contextTimestampTicks,
            [](std::uint64_t timestampTicks, const CameraEpisode& episode) {
                return timestampTicks < episode.anchorTimestampTicks;
            });
        if (afterVisit == episodes.begin())
            continue;
        const auto& episode = *std::prev(afterVisit);
        const bool clickSelection =
            visit.selectionAccess == ProductionSelectionAccess::DirectClick ||
            visit.selectionAccess == ProductionSelectionAccess::BoxSelect;
        const bool matchingControlGroupDoubleTap =
            visit.selectionAccess == ProductionSelectionAccess::ControlGroup &&
            episode.access == ProductionCameraAccess::ControlGroupDoubleTap &&
            episode.anchorId == visit.controlGroup &&
            episode.anchorTimestampTicks == visit.contextTimestampTicks;
        if (!clickSelection && !matchingControlGroupDoubleTap)
            continue;
        visit.cameraAccess = episode.access;
        visit.cameraAnchorKind = episode.anchorKind;
        visit.cameraEpisodeId = episode.id;
        visit.cameraAnchorId = episode.anchorId;
        visit.cameraAnchorTimestampTicks = episode.anchorTimestampTicks;
    }
}

MacroAccessStyleStatistics
summarizeMacroAccessStyleDurations(std::vector<double> durationsMs) {
    MacroAccessStyleStatistics statistics;
    statistics.cycleCount = durationsMs.size();
    if (durationsMs.empty())
        return statistics;
    std::sort(durationsMs.begin(), durationsMs.end());
    const auto percentile = [&](double probability) {
        const double position = probability * static_cast<double>(durationsMs.size() - 1);
        const auto lower = static_cast<std::size_t>(position);
        const auto upper = std::min(lower + 1, durationsMs.size() - 1);
        const double fraction = position - static_cast<double>(lower);
        return durationsMs[lower] + (durationsMs[upper] - durationsMs[lower]) * fraction;
    };
    statistics.averageDurationMs =
        std::accumulate(durationsMs.begin(), durationsMs.end(), 0.0) /
        static_cast<double>(durationsMs.size());
    statistics.medianDurationMs = percentile(0.50);
    statistics.bestDurationMs = durationsMs.front();
    statistics.p25DurationMs = percentile(0.25);
    statistics.p75DurationMs = percentile(0.75);
    statistics.p90DurationMs = percentile(0.90);
    return statistics;
}

double macroAccessStylePercentage(const ProductMacroCycleAnalysis& analysis,
                                  MacroAccessStyle style) noexcept {
    if (!analysis.available || analysis.cycles.empty())
        return 0.0;
    const auto index = macroAccessStyleIndex(style);
    if (index >= analysis.accessStyleStatistics.size())
        return 0.0;
    return static_cast<double>(analysis.accessStyleStatistics[index].cycleCount) * 100.0 /
           static_cast<double>(analysis.cycles.size());
}

std::vector<std::string> MacroHotkeyProfile::compatibleProductionCommands(std::uint16_t key) const {
    std::vector<std::string> commands;
    for (const auto& hotkey : productionCommands) {
        if (hotkey.virtualKey == key)
            commands.push_back(hotkey.command);
    }
    return commands;
}

bool isOrdinaryProductionCommandIdentifier(std::string_view command) {
    constexpr std::array<std::string_view, 3> prefixes{"STR_MAKE_P_", "STR_MAKE_T_", "STR_MAKE_Z_"};
    std::string_view unit;
    for (const auto prefix : prefixes) {
        if (startsWith(command, prefix)) {
            unit = command.substr(prefix.size());
            break;
        }
    }
    if (unit.empty())
        return false;

    // Explicit unit allow-list: research, upgrades, commands, placement, spells, and Archon combination
    // actions are intentionally absent. Zerg combat-unit morphs remain ordinary physical production here.
    static const std::unordered_set<std::string> ordinaryUnits{
        "PROBE", "ZEALOT", "DRAGOON", "TEMPLAR", "HIGHTEMPLAR", "DTEMPLAR", "DARKTEMPLAR",
        "OBSERVER", "SHUTTLE", "REAVER", "CORSAIR", "SCOUT", "ARBITER", "CARRIER",
        "INTERCEPTOR", "SCARAB", "SCV", "MARINE", "FIREBAT", "GHOST", "MEDIC", "VULTURE",
        "TANK", "SIEGETANK", "GOLIATH", "WRAITH", "DROPSHIP", "VESSEL", "SCIENCEVESSEL",
        "BCRUISER", "BATTLECRUISER", "FRIGATE", "VALKYRIE", "DRONE", "ZERGLING", "OVERLORD",
        "HYDRALISK", "MUTALID", "MUTALISK", "AVENGER", "SCOURGE", "QUEEN", "DEFILER",
        "ULTRALISK", "LURKER", "INFESTED", "INFESTEDTERRAN"};
    return ordinaryUnits.contains(normalizedProductionName(unit));
}

MacroHotkeyProfile parseStarCraftHotkeyProfile(const std::string& settingsJson) noexcept {
    try {
        const std::string_view text =
            settingsJson.size() >= 3 && static_cast<unsigned char>(settingsJson[0]) == 0xEF &&
                    static_cast<unsigned char>(settingsJson[1]) == 0xBB &&
                    static_cast<unsigned char>(settingsJson[2]) == 0xBF
                ? std::string_view(settingsJson).substr(3)
                : std::string_view(settingsJson);
        const auto root = json::parse(std::string(text));
        const auto& customSetting = root["General settings"]["Starcraft-Game Custom Hotkeys"];
        if (!customSetting.isBool())
            return unavailableProfile(unreadableHotkeysReason);
        const bool customEnabled = customSetting.asBool();
        if (!customEnabled)
            return unavailableProfile("StarCraft custom hotkeys are disabled", false);

        const auto& encodedHotkeys = root["Hotkeys"];
        if (!encodedHotkeys.isString())
            return unavailableProfile(unreadableHotkeysReason, true);

        MacroHotkeyProfile profile;
        profile.customHotkeysEnabled = true;
        std::istringstream lines(encodedHotkeys.asString());
        std::string line;
        while (std::getline(lines, line)) {
            line = trim(std::move(line));
            if (line.empty())
                continue;
            const auto separator = line.find('=');
            if (separator == std::string::npos)
                continue;
            auto command = trim(line.substr(0, separator));
            auto boundKey = trim(line.substr(separator + 1));
            if (command.empty() || boundKey.empty())
                continue;
            StarCraftHotkey binding{std::move(command), std::move(boundKey), 0};
            binding.virtualKey = hotkeyVirtualKey(binding.boundKey);
            profile.parsedBindings.push_back(binding);
            if (binding.virtualKey != 0 && isOrdinaryProductionCommandIdentifier(binding.command))
                profile.productionCommands.push_back(std::move(binding));
        }
        if (profile.productionCommands.empty())
            return unavailableProfile("StarCraft hotkey configuration contains no recognized production bindings",
                                      true);
        profile.available = true;
        return profile;
    } catch (...) {
        return unavailableProfile(unreadableHotkeysReason);
    }
}

MacroHotkeyProfile loadStarCraftHotkeyProfile(const std::filesystem::path& settingsPath) noexcept {
    try {
        std::ifstream input(settingsPath, std::ios::binary);
        if (!input)
            return unavailableProfile(unreadableHotkeysReason);
        std::ostringstream text;
        text << input.rdbuf();
        if (!input.good() && !input.eof())
            return unavailableProfile(unreadableHotkeysReason);
        return parseStarCraftHotkeyProfile(text.str());
    } catch (...) {
        return unavailableProfile(unreadableHotkeysReason);
    }
}

MacroHotkeyProfile loadStarCraftHotkeyProfile() noexcept {
    PWSTR documentsRaw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documentsRaw)))
        return unavailableProfile(unreadableHotkeysReason);
    const std::filesystem::path documents(documentsRaw);
    CoTaskMemFree(documentsRaw);
    return loadStarCraftHotkeyProfile(documents / "Starcraft" / "CSettings.json");
}

std::vector<LikelyProductionGroup>
inferLikelyProductionGroups(const std::vector<MechanicalInputEvent>& events,
                            const MacroHotkeyProfile& hotkeys, std::uint64_t qpcFrequency) {
    if (!hotkeys.available || qpcFrequency == 0)
        return {};
    const auto visits = collectControlGroupVisits(events, hotkeys, qpcFrequency);
    std::array<GroupEvidence, 10> evidence{};
    for (const auto& visit : visits) {
        if (visit.productionPressIndices.empty())
            continue;
        auto& group = evidence[static_cast<std::size_t>(visit.group)];
        ++group.candidateVisits;
        if (visit.mouseContradiction) {
            ++group.mouseContradictions;
            continue;
        }
        ++group.cleanVisits;
        group.cleanPresses += static_cast<int>(visit.productionPressIndices.size());
        std::unordered_set<std::uint16_t> keysInVisit;
        for (const auto index : visit.productionPressIndices) {
            const auto key = events[index].virtualKey;
            ++group.keys[key].presses;
            keysInVisit.insert(key);
        }
        for (const auto key : keysInVisit)
            ++group.keys[key].visits;
    }

    std::vector<LikelyProductionGroup> likely;
    for (int number = 0; number <= 9; ++number) {
        const auto& group = evidence[static_cast<std::size_t>(number)];
        const bool mouseTargetingPredominates =
            group.candidateVisits > 0 && group.mouseContradictions * 2 >= group.candidateVisits;
        if (group.cleanVisits < minimumProductionVisits || group.cleanPresses < minimumProductionPresses ||
            mouseTargetingPredominates)
            continue;
        LikelyProductionGroup inferred;
        inferred.group = number;
        for (const auto& [key, keyEvidence] : group.keys) {
            if (keyEvidence.visits >= minimumProductionVisits ||
                keyEvidence.presses >= minimumProductionPresses)
                inferred.observedProductionKeys.push_back(key);
        }
        std::sort(inferred.observedProductionKeys.begin(), inferred.observedProductionKeys.end());
        likely.push_back(std::move(inferred));
    }
    return likely;
}

std::vector<ProductionVisit>
detectHeuristicProductionVisitsForLikelyGroups(const AnalysisResult& result,
                                               const MacroHotkeyProfile& hotkeys,
                                               std::uint64_t qpcFrequency,
                                               const std::vector<LikelyProductionGroup>& likelyGroups) {
    if (!hotkeys.available || qpcFrequency == 0)
        return {};
    const auto learned = learnedKeyMap(likelyGroups);
    const auto candidates = detectControlGroupProductionCandidates(result, hotkeys, qpcFrequency);
    std::vector<ProductionVisit> visits;
    for (const auto& candidate : candidates) {
        if (candidate.mouseContradiction)
            continue;
        const auto found = learned.find(candidate.visit.controlGroup);
        if (found == learned.end())
            continue;
        ProductionVisit visit = candidate.visit;
        visit.physicalProductionKeys.clear();
        std::size_t firstQualifyingIndex = 0;
        std::size_t finalQualifyingIndex = 0;
        for (const auto eventIndex : candidate.productionMechanicalEventIndices) {
            const auto key = result.mechanicalEvents[eventIndex].virtualKey;
            if (found->second.empty() || found->second.contains(key)) {
                if (visit.physicalProductionKeys.empty())
                    firstQualifyingIndex = eventIndex;
                visit.physicalProductionKeys.push_back(key);
                finalQualifyingIndex = eventIndex;
            }
        }
        if (visit.physicalProductionKeys.empty())
            continue;
        const auto& firstPress = result.mechanicalEvents[firstQualifyingIndex];
        const auto& finalPress = result.mechanicalEvents[finalQualifyingIndex];
        visit.firstProductionActiveMs = firstPress.activeMs;
        visit.firstProductionTimestampTicks = firstPress.timestampTicks;
        visit.endActiveMs = finalPress.activeMs;
        visit.endTimestampTicks = finalPress.timestampTicks;
        refreshProductionVisitTiming(visit, qpcFrequency);
        visit.physicalProductionPresses = static_cast<int>(visit.physicalProductionKeys.size());
        visits.push_back(std::move(visit));
    }
    return visits;
}

std::vector<ControlGroupProductionCandidate>
detectControlGroupProductionCandidates(const AnalysisResult& result,
                                       const MacroHotkeyProfile& hotkeys,
                                       std::uint64_t qpcFrequency) {
    if (!hotkeys.available || qpcFrequency == 0)
        return {};
    const auto candidates = collectControlGroupVisits(result.mechanicalEvents, hotkeys, qpcFrequency);
    std::vector<ControlGroupProductionCandidate> visits;
    visits.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (candidate.productionPressIndices.empty())
            continue;
        const auto& firstPress = result.mechanicalEvents[candidate.productionPressIndices.front()];
        const auto& finalPress = result.mechanicalEvents[candidate.productionPressIndices.back()];
        ProductionVisit visit;
        visit.accessMethod = ProductionAccessMethod::ControlGroup;
        visit.selectionAccess = ProductionSelectionAccess::ControlGroup;
        visit.startActiveMs = candidate.selectActiveMs;
        visit.endActiveMs = finalPress.activeMs;
        visit.startTimestampTicks = candidate.selectTimestampTicks;
        visit.endTimestampTicks = finalPress.timestampTicks;
        visit.contextActiveMs = candidate.selectActiveMs;
        visit.contextTimestampTicks = candidate.selectTimestampTicks;
        visit.firstProductionActiveMs = firstPress.activeMs;
        visit.firstProductionTimestampTicks = firstPress.timestampTicks;
        refreshProductionVisitTiming(visit, qpcFrequency);
        visit.controlGroup = candidate.group;
        visit.productionContext = makeControlGroupProductionContext(
            candidate.group, candidate.assignmentGeneration);
        visit.physicalProductionPresses =
            static_cast<int>(candidate.productionPressIndices.size());
        visit.physicalProductionKeys.reserve(candidate.productionPressIndices.size());
        for (const auto index : candidate.productionPressIndices)
            visit.physicalProductionKeys.push_back(result.mechanicalEvents[index].virtualKey);
        visits.push_back(
            {std::move(visit), candidate.selectEventIndex, candidate.productionPressIndices,
             candidate.mouseContradiction});
    }
    return visits;
}

ProductMacroCycleAnalysis
summarizeProductMacroCycles(MacroProductType productType, std::vector<MacroCycle> cycles,
                            const std::vector<ProductionVisit>& visits) {
    ProductMacroCycleAnalysis analysis;
    analysis.available = true;
    analysis.productType = productType;
    analysis.cycles = std::move(cycles);
    for (const auto& visit : visits) {
        if (visit.productType != productType)
            continue;
        ++analysis.productionVisitCount;
        ++analysis.accessMethodCounts[accessMethodIndex(visit.accessMethod)];
    }
    std::array<std::vector<double>, macroAccessStyleCount> styleDurations;
    for (auto& cycle : analysis.cycles) {
        cycle.macroAccessStyle = deriveMacroAccessStyle(cycle, visits);
        styleDurations[macroAccessStyleIndex(cycle.macroAccessStyle)].push_back(
            cycle.durationMs);
    }
    for (std::size_t index = 0; index < styleDurations.size(); ++index)
        analysis.accessStyleStatistics[index] =
            summarizeMacroAccessStyleDurations(std::move(styleDurations[index]));
    if (analysis.cycles.empty())
        return analysis;
    double total = 0.0;
    double best = analysis.cycles.front().durationMs;
    double slowest = analysis.cycles.front().durationMs;
    for (const auto& cycle : analysis.cycles) {
        total += cycle.durationMs;
        best = std::min(best, cycle.durationMs);
        slowest = std::max(slowest, cycle.durationMs);
    }
    analysis.averageDurationMs = total / static_cast<double>(analysis.cycles.size());
    analysis.bestDurationMs = best;
    analysis.slowestDurationMs = slowest;
    return analysis;
}

ProductMacroCycleAnalysis groupProductionVisits(const std::vector<ProductionVisit>& visits,
                                                MacroProductType productType,
                                                const std::vector<MechanicalInputEvent>& mechanicalEvents,
                                                std::uint64_t qpcFrequency) {
    const double mergeGap = productType == MacroProductType::Worker ? workerMacroMergeGapMs
                                                                    : armyMacroMergeGapMs;
    const double maximumDuration = productType == MacroProductType::Worker
                                       ? workerMacroMaximumDurationMs
                                       : armyMacroMaximumDurationMs;
    std::vector<MacroCycle> cycles;
    std::vector<std::size_t> repeatedContextSplitVisitIndices;
    std::vector<AssignmentInterruptionSplit> assignmentInterruptionSplitDetails;
    bool oppositeSinceCurrent = false;
    for (std::size_t index = 0; index < visits.size(); ++index) {
        const auto& visit = visits[index];
        if (visit.productType != productType) {
            if (visit.productType != MacroProductType::Unknown)
                oppositeSinceCurrent = true;
            continue;
        }

        bool mergeWithoutContextIdentity = false;
        bool repeatedKnownContext = false;
        const MechanicalInputEvent* assignmentInterruption = nullptr;
        if (!cycles.empty() && !oppositeSinceCurrent) {
            auto& cycle = cycles.back();
            const auto& previousVisit = visits[cycle.visitIndices.back()];
            const double activeGap =
                visit.contextActiveMs - previousVisit.firstProductionActiveMs;
            const auto realGap =
                qpcElapsedMs(previousVisit.firstProductionTimestampTicks,
                             visit.contextTimestampTicks, qpcFrequency);
            const auto executionTotalDuration =
                qpcElapsedMs(cycle.startTimestampTicks,
                             visit.firstProductionTimestampTicks, qpcFrequency);
            mergeWithoutContextIdentity =
                realGap && executionTotalDuration && activeGap >= 0.0 && *realGap <= mergeGap &&
                *realGap - activeGap <= qpcActivePauseToleranceMs &&
                *executionTotalDuration <= maximumDuration;
            if (mergeWithoutContextIdentity && knownProductionContext(visit.productionContext)) {
                repeatedKnownContext = std::any_of(
                    cycle.visitIndices.begin(), cycle.visitIndices.end(),
                    [&](std::size_t previousVisitIndex) {
                        return sameProductionContext(
                            visits[previousVisitIndex].productionContext,
                            visit.productionContext);
                    });
            }
            if (mergeWithoutContextIdentity) {
                const auto previousVisitIndex = cycle.visitIndices.back();
                assignmentInterruption = firstAssignmentInterruption(
                    mechanicalEvents, visits[previousVisitIndex].endTimestampTicks,
                    visit.contextTimestampTicks);
            }
        }
        const bool merge = mergeWithoutContextIdentity && !repeatedKnownContext &&
                           assignmentInterruption == nullptr;

        if (!merge) {
            if (repeatedKnownContext)
                repeatedContextSplitVisitIndices.push_back(index);
            if (assignmentInterruption) {
                assignmentInterruptionSplitDetails.push_back(
                    {cycles.back().visitIndices.back(), index, assignmentInterruption->type,
                     assignmentInterruption->activeMs,
                     assignmentInterruption->timestampTicks});
            }
            MacroCycle cycle;
            cycle.productType = productType;
            cycle.startActiveMs = visit.startActiveMs;
            cycle.endActiveMs = visit.endActiveMs;
            cycle.executionEndActiveMs = visit.firstProductionActiveMs;
            cycle.startTimestampTicks = visit.startTimestampTicks;
            cycle.endTimestampTicks = visit.endTimestampTicks;
            cycle.executionEndTimestampTicks = visit.firstProductionTimestampTicks;
            cycle.durationMs = visit.executionDurationMs;
            cycle.fullSpanMs = visit.durationMs;
            cycle.visitIndices.push_back(index);
            cycles.push_back(std::move(cycle));
        } else {
            auto& cycle = cycles.back();
            cycle.endActiveMs = visit.endActiveMs;
            cycle.endTimestampTicks = visit.endTimestampTicks;
            cycle.executionEndActiveMs = visit.firstProductionActiveMs;
            cycle.executionEndTimestampTicks = visit.firstProductionTimestampTicks;
            cycle.durationMs =
                qpcElapsedMs(cycle.startTimestampTicks, cycle.executionEndTimestampTicks,
                             qpcFrequency)
                    .value_or(std::max(
                        0.0, cycle.executionEndActiveMs - cycle.startActiveMs));
            cycle.fullSpanMs =
                qpcElapsedMs(cycle.startTimestampTicks, cycle.endTimestampTicks,
                             qpcFrequency)
                    .value_or(std::max(0.0,
                                       cycle.endActiveMs - cycle.startActiveMs));
            cycle.visitIndices.push_back(index);
        }
        oppositeSinceCurrent = false;
    }
    auto analysis = summarizeProductMacroCycles(productType, std::move(cycles), visits);
    analysis.repeatedContextSplits = repeatedContextSplitVisitIndices.size();
    analysis.repeatedContextSplitVisitIndices = std::move(repeatedContextSplitVisitIndices);
    analysis.assignmentInterruptionSplits = assignmentInterruptionSplitDetails.size();
    analysis.assignmentInterruptionSplitDetails =
        std::move(assignmentInterruptionSplitDetails);
    return analysis;
}

ProductMacroCycleAnalysis groupProductionVisits(const std::vector<ProductionVisit>& visits,
                                                MacroProductType productType,
                                                std::uint64_t qpcFrequency) {
    static const std::vector<MechanicalInputEvent> noMechanicalEvents;
    return groupProductionVisits(visits, productType, noMechanicalEvents, qpcFrequency);
}

ProductionAnalysis analyzeProductionVisits(const AnalysisResult& result,
                                            const MacroHotkeyProfile& hotkeys,
                                            std::uint64_t qpcFrequency) {
    ProductionAnalysis analysis;
    analysis.armyControlGroupManagement =
        detectArmyControlGroupManagement(result, qpcFrequency);
    if (!hotkeys.available) {
        analysis.visitsUnavailableReason = hotkeys.unavailableReason;
    } else if (qpcFrequency == 0) {
        analysis.visitsUnavailableReason = "QPC frequency is unavailable";
    } else {
        analysis.visitsAvailable = true;
        analysis.likelyProductionGroups =
            inferLikelyProductionGroups(result.mechanicalEvents, hotkeys, qpcFrequency);
        analysis.productionVisits = detectHeuristicProductionVisitsForLikelyGroups(
            result, hotkeys, qpcFrequency, analysis.likelyProductionGroups);
        annotateProductionAccessTelemetry(analysis.productionVisits, result);
    }
    const std::string replayReason = "Replay correlation was not performed";
    markUnavailable(analysis.workerMacroCycles, MacroProductType::Worker, replayReason);
    markUnavailable(analysis.armyMacroCycles, MacroProductType::Army, replayReason);
    analysis.armyCommandActivity.unavailableReason = replayReason;
    analysis.abilityActivity.unavailableReason = replayReason;
    analysis.replayCorrelation.unavailableReason = replayReason;
    analysis.replayCorrelation.unmatchedProductionVisits = analysis.productionVisits.size();
    return analysis;
}

} // namespace smp
