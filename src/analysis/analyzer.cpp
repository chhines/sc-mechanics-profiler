#include "analysis/analyzer.h"

#include <algorithm>
#include <utility>
#include <windows.h>

namespace smp {
namespace {

bool keyDown(const std::array<bool, 256>& keys, std::uint16_t key) {
    return key < keys.size() && keys[key];
}

bool controlDown(const std::array<bool, 256>& keys) {
    return keyDown(keys, VK_CONTROL) || keyDown(keys, VK_LCONTROL) || keyDown(keys, VK_RCONTROL);
}

bool shiftDown(const std::array<bool, 256>& keys) {
    return keyDown(keys, VK_SHIFT) || keyDown(keys, VK_LSHIFT) || keyDown(keys, VK_RSHIFT);
}

bool altDown(const std::array<bool, 256>& keys) {
    return keyDown(keys, VK_MENU) || keyDown(keys, VK_LMENU) || keyDown(keys, VK_RMENU);
}

int edgeMask(EdgeDirection direction) {
    constexpr int left = 1;
    constexpr int right = 2;
    constexpr int top = 4;
    constexpr int bottom = 8;
    switch (direction) {
    case EdgeDirection::Left:
        return left;
    case EdgeDirection::Right:
        return right;
    case EdgeDirection::Top:
        return top;
    case EdgeDirection::Bottom:
        return bottom;
    case EdgeDirection::TopLeft:
        return top | left;
    case EdgeDirection::TopRight:
        return top | right;
    case EdgeDirection::BottomLeft:
        return bottom | left;
    case EdgeDirection::BottomRight:
        return bottom | right;
    default:
        return 0;
    }
}

bool compatibleEdges(EdgeDirection first, EdgeDirection second) {
    return first == second || (edgeMask(first) & edgeMask(second)) != 0;
}

} // namespace

const char* cameraNavigationTypeName(CameraNavigationType type) noexcept {
    switch (type) {
    case CameraNavigationType::ControlGroupJump:
        return "CONTROL_GROUP_JUMP";
    case CameraNavigationType::LocationHotkey:
        return "LOCATION_HOTKEY";
    case CameraNavigationType::MinimapJump:
        return "MINIMAP_JUMP";
    case CameraNavigationType::EdgeScroll:
        return "EDGE_SCROLL";
    default:
        return "UNKNOWN";
    }
}

const char* cameraRecenterTypeName(CameraRecenterType type) noexcept {
    return type == CameraRecenterType::ControlGroup ? "CONTROL_GROUP_RECENTER" : "LOCATION_RECENTER";
}

Analyzer::Analyzer(Config config, std::uint64_t ticksPerSecond)
    : config_(std::move(config)), frequency_(ticksPerSecond == 0 ? 1 : ticksPerSecond) {}

double Analyzer::ticksToMs(std::uint64_t ticks) const noexcept {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency_);
}

double Analyzer::activeTimeAt(double absoluteMs) const noexcept {
    return accumulatedActiveMs_ + (active_ ? std::max(0.0, absoluteMs - activeSegmentStartAbsoluteMs_) : 0.0);
}

void Analyzer::setScreenRegions(const ScreenRegions& regions) noexcept {
    config_.gameArea = regions.gameArea;
    config_.viewport = regions.viewport;
    config_.minimap = regions.minimap;
    config_.commandCard = regions.commandCard;
    clearEdgeState();
}

ScreenRegions Analyzer::screenRegions() const noexcept {
    return {{}, config_.gameArea, config_.viewport, config_.minimap, config_.commandCard};
}

void Analyzer::emitNavigation(const CameraNavigationEvent& event) {
    result_.navigationEvents.push_back(event);
    emittedNavigation_.push_back(event);
}

void Analyzer::emitRecenter(const CameraRecenterEvent& event) {
    result_.recenters.push_back(event);
    emittedRecenters_.push_back(event);
}

void Analyzer::emitMechanical(const MechanicalInputEvent& event) {
    result_.mechanicalEvents.push_back(event);
}

std::uint16_t Analyzer::mechanicalModifiers() const noexcept {
    std::uint16_t modifiers = ModifierNone;
    if (controlDown(keysDown_))
        modifiers |= ModifierCtrl;
    if (shiftDown(keysDown_))
        modifiers |= ModifierShift;
    if (altDown(keysDown_))
        modifiers |= ModifierAlt;
    return modifiers;
}

void Analyzer::handleControlGroupSelect(const RawInputEvent& event, int group, double activeMs) {
    auto& previous = lastControlGroupSelect_[static_cast<std::size_t>(group)];
    if (!previous || ticksToMs(event.timestampTicks - *previous) > config_.controlGroupDoubleTapMs) {
        previous = event.timestampTicks;
        return;
    }

    previous.reset();
    if (cameraContext_.type == CameraContextType::ControlGroup && cameraContext_.id == group) {
        emitRecenter({event.timestampTicks, activeMs, CameraRecenterType::ControlGroup, group, event.cursorX,
                      event.cursorY});
        return;
    }

    emitNavigation({event.timestampTicks, activeMs, CameraNavigationType::ControlGroupJump, group, event.cursorX,
                    event.cursorY, 0.0, EdgeDirection::None, event.cursorX, event.cursorY});
    cameraContext_ = {CameraContextType::ControlGroup, group};
}

void Analyzer::handleLocationRecall(const RawInputEvent& event, int location, double activeMs) {
    ++result_.locationRecallCount;
    if (cameraContext_.type == CameraContextType::LocationHotkey && cameraContext_.id == location) {
        emitRecenter({event.timestampTicks, activeMs, CameraRecenterType::LocationHotkey, location, event.cursorX,
                      event.cursorY});
        return;
    }

    emitNavigation({event.timestampTicks, activeMs, CameraNavigationType::LocationHotkey, location, event.cursorX,
                    event.cursorY, 0.0, EdgeDirection::None, event.cursorX, event.cursorY});
    cameraContext_ = {CameraContextType::LocationHotkey, location};
}

void Analyzer::handleKeyDown(const RawInputEvent& event, double activeMs) {
    const auto key = event.virtualKey;
    if (key >= '0' && key <= '9') {
        const int group = key - '0';
        if (controlDown(keysDown_)) {
            emitMechanical({event.timestampTicks, activeMs, MechanicalInputType::ControlGroupAssign,
                            event.virtualKey, event.scanCode, mechanicalModifiers(), group,
                            event.cursorX, event.cursorY});
            lastControlGroupSelect_[static_cast<std::size_t>(group)].reset();
            return; // CONTROL_GROUP_ASSIGN
        }
        if (shiftDown(keysDown_)) {
            emitMechanical({event.timestampTicks, activeMs, MechanicalInputType::ControlGroupAdd,
                            event.virtualKey, event.scanCode, mechanicalModifiers(), group,
                            event.cursorX, event.cursorY});
            lastControlGroupSelect_[static_cast<std::size_t>(group)].reset();
            return; // CONTROL_GROUP_ADD
        }
        emitMechanical({event.timestampTicks, activeMs, MechanicalInputType::ControlGroupSelect,
                        event.virtualKey, event.scanCode, mechanicalModifiers(), group,
                        event.cursorX, event.cursorY});
        handleControlGroupSelect(event, group, activeMs); // CONTROL_GROUP_SELECT, possibly a jump
        return;
    }

    if (std::find(config_.locationHotkeys.begin(), config_.locationHotkeys.end(), key) ==
        config_.locationHotkeys.end()) {
        emitMechanical({event.timestampTicks, activeMs, MechanicalInputType::KeyPress,
                        event.virtualKey, event.scanCode, mechanicalModifiers(), -1,
                        event.cursorX, event.cursorY});
        return;
    }
    const int location = static_cast<int>(key - VK_F1 + 1);
    if (shiftDown(keysDown_)) {
        emitMechanical({event.timestampTicks, activeMs, MechanicalInputType::LocationAssign,
                        event.virtualKey, event.scanCode, mechanicalModifiers(), location,
                        event.cursorX, event.cursorY});
        return; // LOCATION_HOTKEY_ASSIGN
    }
    emitMechanical({event.timestampTicks, activeMs, MechanicalInputType::LocationRecall,
                    event.virtualKey, event.scanCode, mechanicalModifiers(), location,
                    event.cursorX, event.cursorY});
    handleLocationRecall(event, location, activeMs);
}

void Analyzer::clearEdgeState() noexcept {
    candidateEdge_ = EdgeDirection::None;
    candidateEdgeStartTicks_ = 0;
    candidateEdgeStartActiveMs_ = 0.0;
    candidateEdgeStartCursor_ = {};
    edgeActive_ = false;
    activeEdgeDirection_ = EdgeDirection::None;
}

void Analyzer::completeEdgeEpisode(const RawInputEvent& event) {
    if (candidateEdge_ == EdgeDirection::None)
        return;
    const double durationMs = ticksToMs(event.timestampTicks - candidateEdgeStartTicks_);
    if ((edgeActive_ || durationMs >= config_.edgeMinimumDwellMs) && durationMs >= config_.edgeMinimumDwellMs) {
        const auto direction = edgeActive_ ? activeEdgeDirection_ : candidateEdge_;
        emitNavigation({candidateEdgeStartTicks_, candidateEdgeStartActiveMs_, CameraNavigationType::EdgeScroll,
                        -1, event.cursorX, event.cursorY, durationMs, direction, candidateEdgeStartCursor_.x,
                        candidateEdgeStartCursor_.y});
        cameraContext_ = {CameraContextType::Manual, -1};
    }
    clearEdgeState();
}

void Analyzer::handleMouseMove(const RawInputEvent& event, double activeMs) {
    lastCursor_ = {event.cursorX, event.cursorY};
    const auto direction = edgeDirectionAt(config_.gameArea, config_.edgeMarginPx, lastCursor_);

    if (direction == EdgeDirection::None) {
        completeEdgeEpisode(event);
        return;
    }
    if (candidateEdge_ == EdgeDirection::None) {
        candidateEdge_ = direction;
        candidateEdgeStartTicks_ = event.timestampTicks;
        candidateEdgeStartActiveMs_ = activeMs;
        candidateEdgeStartCursor_ = lastCursor_;
        return;
    }
    if (!compatibleEdges(candidateEdge_, direction)) {
        completeEdgeEpisode(event);
        candidateEdge_ = direction;
        candidateEdgeStartTicks_ = event.timestampTicks;
        candidateEdgeStartActiveMs_ = activeMs;
        candidateEdgeStartCursor_ = lastCursor_;
        return;
    }
    if (!edgeActive_ && ticksToMs(event.timestampTicks - candidateEdgeStartTicks_) >= config_.edgeMinimumDwellMs) {
        edgeActive_ = true;
        activeEdgeDirection_ = candidateEdge_;
    }
}

void Analyzer::process(const RawInputEvent& event) {
    if (finalized_)
        return;
    const double absoluteMs = ticksToMs(event.timestampTicks);

    if (event.type == RawEventType::ForegroundGained) {
        if (!seenSession_) {
            seenSession_ = true;
        } else if (!active_) {
            accumulatedPausedMs_ += std::max(0.0, absoluteMs - pauseStartAbsoluteMs_);
        }
        active_ = true;
        activeSegmentStartAbsoluteMs_ = absoluteMs;
        keysDown_.fill(false);
        for (auto& tap : lastControlGroupSelect_)
            tap.reset();
        clearEdgeState();
        lastCursor_ = {event.cursorX, event.cursorY};
        return;
    }

    if (event.type == RawEventType::ForegroundLost) {
        if (!active_)
            return;
        completeEdgeEpisode(event);
        accumulatedActiveMs_ += std::max(0.0, absoluteMs - activeSegmentStartAbsoluteMs_);
        active_ = false;
        pauseStartAbsoluteMs_ = absoluteMs;
        keysDown_.fill(false);
        for (auto& tap : lastControlGroupSelect_)
            tap.reset();
        return;
    }

    if (!active_) {
        // Deterministic replay streams may omit a foreground marker; live capture never does.
        if (!seenSession_) {
            seenSession_ = true;
            activeSegmentStartAbsoluteMs_ = absoluteMs;
            active_ = true;
        } else {
            return;
        }
    }

    const double activeMs = activeTimeAt(absoluteMs);
    if (event.type == RawEventType::KeyUp) {
        if (event.virtualKey < keysDown_.size())
            keysDown_[event.virtualKey] = false;
        return;
    }
    if (event.type == RawEventType::KeyDown) {
        if (event.virtualKey < keysDown_.size() && keysDown_[event.virtualKey])
            return; // OS autorepeat: no intervening key-up
        if (event.virtualKey < keysDown_.size())
            keysDown_[event.virtualKey] = true;
        handleKeyDown(event, activeMs);
        return;
    }
    std::optional<MechanicalInputType> mechanicalType;
    switch (event.type) {
    case RawEventType::MouseLeftDown:
        mechanicalType = MechanicalInputType::MouseLeftDown;
        break;
    case RawEventType::MouseLeftUp:
        mechanicalType = MechanicalInputType::MouseLeftUp;
        break;
    case RawEventType::MouseRightDown:
        mechanicalType = MechanicalInputType::MouseRightDown;
        break;
    case RawEventType::MouseRightUp:
        mechanicalType = MechanicalInputType::MouseRightUp;
        break;
    case RawEventType::MouseMiddleDown:
        mechanicalType = MechanicalInputType::MouseMiddleDown;
        break;
    case RawEventType::MouseMiddleUp:
        mechanicalType = MechanicalInputType::MouseMiddleUp;
        break;
    case RawEventType::MouseWheel:
        mechanicalType = MechanicalInputType::MouseWheel;
        break;
    default:
        break;
    }
    if (mechanicalType) {
        const int value = event.type == RawEventType::MouseWheel ? event.wheelDelta : -1;
        emitMechanical({event.timestampTicks, activeMs, *mechanicalType, 0, 0,
                        mechanicalModifiers(), value, event.cursorX, event.cursorY});
    }
    if (event.type == RawEventType::MouseLeftDown && config_.minimap.contains({event.cursorX, event.cursorY})) {
        emitNavigation({event.timestampTicks, activeMs, CameraNavigationType::MinimapJump, -1, event.cursorX,
                        event.cursorY, 0.0, EdgeDirection::None, event.cursorX, event.cursorY});
        cameraContext_ = {CameraContextType::Manual, -1};
        return;
    }
    if (event.type == RawEventType::MouseMove)
        handleMouseMove(event, activeMs);
}

void Analyzer::finalize(std::uint64_t endingTicks, std::uint64_t droppedEventCount) {
    if (finalized_)
        return;
    const double endingAbsoluteMs = ticksToMs(endingTicks);
    if (active_) {
        RawInputEvent ending{};
        ending.timestampTicks = endingTicks;
        ending.cursorX = lastCursor_.x;
        ending.cursorY = lastCursor_.y;
        completeEdgeEpisode(ending);
        accumulatedActiveMs_ += std::max(0.0, endingAbsoluteMs - activeSegmentStartAbsoluteMs_);
        active_ = false;
    } else if (seenSession_) {
        accumulatedPausedMs_ += std::max(0.0, endingAbsoluteMs - pauseStartAbsoluteMs_);
    }
    result_.activeDurationSeconds = accumulatedActiveMs_ / 1000.0;
    result_.pausedDurationSeconds = accumulatedPausedMs_ / 1000.0;
    result_.droppedEventCount = droppedEventCount;
    finalized_ = true;
}

std::vector<CameraNavigationEvent> Analyzer::takeEmittedNavigationEvents() {
    std::vector<CameraNavigationEvent> result;
    result.swap(emittedNavigation_);
    return result;
}

std::vector<CameraRecenterEvent> Analyzer::takeEmittedRecenters() {
    std::vector<CameraRecenterEvent> result;
    result.swap(emittedRecenters_);
    return result;
}

} // namespace smp
