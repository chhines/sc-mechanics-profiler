#include "analysis/analyzer.h"

#include "analysis/capacity.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <windows.h>

namespace scm {
namespace {

constexpr std::array<double, 5> productiveThresholds{150.0, 250.0, 500.0, 750.0, 1000.0};

bool isModifier(std::uint16_t key) {
    return key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT || key == VK_CONTROL || key == VK_LCONTROL ||
           key == VK_RCONTROL || key == VK_MENU || key == VK_LMENU || key == VK_RMENU;
}

bool down(const std::array<bool, 256>& state, int key) {
    return key >= 0 && key < static_cast<int>(state.size()) && state[static_cast<std::size_t>(key)];
}

double distance(int x1, int y1, int x2, int y2) {
    return std::hypot(static_cast<double>(x2 - x1), static_cast<double>(y2 - y1));
}

int edgeDirection(const Rect& viewport, int thickness, int x, int y) {
    if (!viewport.valid() || !viewport.contains(x, y))
        return 0;
    const bool left = x <= viewport.left + thickness;
    const bool right = x >= viewport.right - thickness;
    const bool top = y <= viewport.top + thickness;
    const bool bottom = y >= viewport.bottom - thickness;
    if (top && left)
        return 5;
    if (top && right)
        return 6;
    if (bottom && left)
        return 7;
    if (bottom && right)
        return 8;
    if (top)
        return 1;
    if (bottom)
        return 2;
    if (left)
        return 3;
    if (right)
        return 4;
    return 0;
}

std::string boxDirection(int dx, int dy) {
    if (dx >= 0 && dy >= 0)
        return "TL_TO_BR";
    if (dx < 0 && dy < 0)
        return "BR_TO_TL";
    if (dx < 0 && dy >= 0)
        return "TR_TO_BL";
    return "BL_TO_TR";
}

std::vector<double> valuesOf(const std::vector<TimedMeasurement>& measurements) {
    std::vector<double> result;
    result.reserve(measurements.size());
    for (const auto& item : measurements)
        result.push_back(item.valueMs);
    return result;
}

std::optional<double> lateChange(const std::vector<TimedMeasurement>& values, double durationMs) {
    std::vector<double> first;
    std::vector<double> last;
    for (const auto& value : values) {
        if (value.activeTimeMs < durationMs * 0.25)
            first.push_back(value.valueMs);
        else if (value.activeTimeMs >= durationMs * 0.75)
            last.push_back(value.valueMs);
    }
    const auto firstMedian = median(std::move(first));
    const auto lastMedian = median(std::move(last));
    if (!firstMedian || !lastMedian)
        return std::nullopt;
    return percentageChange(*firstMedian, *lastMedian);
}

void addLapse(std::map<std::string, double>& output, const std::string& name, const std::vector<double>& values,
              double activeMinutes) {
    if (values.size() < 20 || activeMinutes <= 0.0)
        return;
    const auto center = median(values);
    const auto deviation = mad(values);
    if (!center || !deviation)
        return;
    const double threshold = *center + 3.0 * 1.4826 * *deviation;
    const auto count = std::count_if(values.begin(), values.end(), [&](double value) { return value > threshold; });
    output[name] = static_cast<double>(count) / activeMinutes;
}

} // namespace

std::string_view logicalEventName(LogicalEventType type) noexcept {
    switch (type) {
    case LogicalEventType::KeyAction:
        return "KEY_ACTION";
    case LogicalEventType::ControlGroupSelect:
        return "CONTROL_GROUP_SELECT";
    case LogicalEventType::ControlGroupAssign:
        return "CONTROL_GROUP_ASSIGN";
    case LogicalEventType::ControlGroupDoubleTap:
        return "CONTROL_GROUP_DOUBLE_TAP";
    case LogicalEventType::LocationHotkey:
        return "LOCATION_HOTKEY";
    case LogicalEventType::LocationHotkeyAssign:
        return "LOCATION_HOTKEY_ASSIGN";
    case LogicalEventType::AttackCommandStart:
        return "ATTACK_COMMAND_START";
    case LogicalEventType::MoveCommandStart:
        return "MOVE_COMMAND_START";
    case LogicalEventType::PatrolCommandStart:
        return "PATROL_COMMAND_START";
    case LogicalEventType::StopCommand:
        return "STOP_COMMAND";
    case LogicalEventType::HoldCommand:
        return "HOLD_COMMAND";
    case LogicalEventType::TargetClick:
        return "TARGET_CLICK";
    case LogicalEventType::RightClickCommand:
        return "RIGHT_CLICK_COMMAND";
    case LogicalEventType::BoxSelectStart:
        return "BOX_SELECT_START";
    case LogicalEventType::BoxSelectEnd:
        return "BOX_SELECT_END";
    case LogicalEventType::MinimapClick:
        return "MINIMAP_CLICK";
    case LogicalEventType::ViewportClick:
        return "VIEWPORT_CLICK";
    case LogicalEventType::CommandCardClick:
        return "COMMAND_CARD_CLICK";
    case LogicalEventType::OtherUiClick:
        return "OTHER_UI_CLICK";
    case LogicalEventType::EdgeScrollStart:
        return "EDGE_SCROLL_START";
    case LogicalEventType::EdgeScrollEnd:
        return "EDGE_SCROLL_END";
    case LogicalEventType::MacroWorkerAttempt:
        return "MACRO_WORKER_ATTEMPT";
    case LogicalEventType::MacroArmyAttempt:
        return "MACRO_ARMY_ATTEMPT";
    case LogicalEventType::MacroEpisodeStart:
        return "MACRO_EPISODE_START";
    case LogicalEventType::MacroEpisodeEnd:
        return "MACRO_EPISODE_END";
    case LogicalEventType::MicroBurstStart:
        return "MICRO_BURST_START";
    case LogicalEventType::MicroBurstEnd:
        return "MICRO_BURST_END";
    case LogicalEventType::InferredContextSwitch:
        return "INFERRED_CONTEXT_SWITCH";
    case LogicalEventType::InferredPacStart:
        return "INFERRED_PAC_START";
    case LogicalEventType::InferredPacFirstAction:
        return "INFERRED_PAC_FIRST_ACTION";
    case LogicalEventType::InferredPacEnd:
        return "INFERRED_PAC_END";
    }
    return "UNKNOWN";
}

std::string_view navigationMethodName(NavigationMethod method) noexcept {
    switch (method) {
    case NavigationMethod::ControlGroupJump:
        return "CONTROL_GROUP_JUMP";
    case NavigationMethod::LocationHotkey:
        return "LOCATION_HOTKEY";
    case NavigationMethod::MinimapJump:
        return "MINIMAP_JUMP";
    case NavigationMethod::EdgeScroll:
        return "EDGE_SCROLL";
    default:
        return "NONE";
    }
}

Analyzer::Analyzer(Config config, std::uint64_t ticksPerSecond)
    : config_(std::move(config)), frequency_(ticksPerSecond == 0 ? 1 : ticksPerSecond) {
    lastGroupTapAbsoluteMs_.fill(-1.0);
}

double Analyzer::ticksToMs(std::uint64_t ticks) const {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency_);
}

double Analyzer::activeTimeAt(double absoluteMs) const {
    return accumulatedActiveMs + (active_ ? std::max(0.0, absoluteMs - activeSegmentStartAbsoluteMs) : 0.0);
}

double Analyzer::currentLoadEapm(double activeMs) const {
    const double windowMs = static_cast<double>(config_.loadWindowSeconds) * 1000.0;
    const auto& actions = result_.effectiveActionTimesMs;
    const auto first = std::lower_bound(actions.begin(), actions.end(), activeMs - windowMs);
    const auto last = std::upper_bound(actions.begin(), actions.end(), activeMs);
    return static_cast<double>(std::distance(first, last)) * 60.0 / static_cast<double>(config_.loadWindowSeconds);
}

void Analyzer::emit(LogicalEventType type, const RawInputEvent& source, Confidence confidence, int data1, int data2,
                    double value1, double value2) {
    logicalEvents_.push_back(
        {source.timestampTicks, source.sequence, type, confidence, 0, data1, data2, value1, value2});
    const double absoluteMs = ticksToMs(source.timestampTicks);
    observeSequence(logicalEvents_.back(), activeTimeAt(absoluteMs), absoluteMs);
}

void Analyzer::emitAt(LogicalEventType type, double absoluteMs, std::uint64_t sourceSequence, Confidence confidence,
                      int data1, int data2, double value1, double value2) {
    const auto ticks = static_cast<std::uint64_t>(std::max(0.0, absoluteMs) * static_cast<double>(frequency_) / 1000.0);
    logicalEvents_.push_back({ticks, sourceSequence, type, confidence, 0, data1, data2, value1, value2});
    observeSequence(logicalEvents_.back(), activeTimeAt(absoluteMs), absoluteMs);
}

void Analyzer::process(const RawInputEvent& event) {
    if (finalized_)
        return;
    const double absoluteMs = ticksToMs(event.timestampTicks);

    if (event.type == RawEventType::ForegroundGained) {
        if (!seenSession_) {
            seenSession_ = true;
            sessionStartAbsoluteMs = absoluteMs;
        } else if (!active_) {
            accumulatedPausedMs += std::max(0.0, absoluteMs - pauseStartAbsoluteMs);
        }
        active_ = true;
        activeSegmentStartAbsoluteMs = absoluteMs;
        keysDown_.fill(false);
        return;
    }

    if (event.type == RawEventType::ForegroundLost) {
        if (!active_)
            return;
        const double activeMs = activeTimeAt(absoluteMs);
        advanceMicro(activeMs, absoluteMs, event.sequence);
        if (microActive_) {
            emitAt(LogicalEventType::MicroBurstEnd, absoluteMs, event.sequence, Confidence::HeuristicInference, 0, 0,
                   activeMs, 0.0);
            microActive_ = false;
            pendingMicroEndActiveMs_ = activeMs;
        }
        if (activeEdge_ != 0) {
            emit(LogicalEventType::EdgeScrollEnd, event, Confidence::HighConfidenceInference, activeEdge_, 0,
                 absoluteMs - activeEdgeStartAbsoluteMs_, 0.0);
            startNavigation(NavigationMethod::EdgeScroll, event, absoluteMs, activeMs,
                            absoluteMs - activeEdgeStartAbsoluteMs_, event.cursorX, event.cursorY);
            activeEdge_ = 0;
        }
        closePac(activeMs, absoluteMs, event.sequence);
        finalizeSelection(activeMs);
        accumulatedActiveMs += std::max(0.0, absoluteMs - activeSegmentStartAbsoluteMs);
        active_ = false;
        pauseStartAbsoluteMs = absoluteMs;
        keysDown_.fill(false);
        pendingCommand_.reset();
        pendingSwitch_.reset();
        pendingSwitchCompletedStartActiveMs_.reset();
        pendingNavigationIndex_.reset();
        sequenceWindow_.clear();
        drag_.reset();
        return;
    }

    if (!active_) {
        // Synthetic replay streams may omit foreground markers; live capture never does.
        if (!seenSession_) {
            seenSession_ = true;
            sessionStartAbsoluteMs = absoluteMs;
            activeSegmentStartAbsoluteMs = absoluteMs;
            active_ = true;
        } else {
            return;
        }
    }

    const double activeMs = activeTimeAt(absoluteMs);
    advanceMicro(activeMs, absoluteMs, event.sequence);

    switch (event.type) {
    case RawEventType::KeyDown: {
        if (event.virtualKey < keysDown_.size() && keysDown_[event.virtualKey])
            return;
        if (event.virtualKey < keysDown_.size())
            keysDown_[event.virtualKey] = true;
        ++result_.rawInputCount;
        if (currentPac_)
            ++currentPac_->rawActionCount;
        handleKeyDown(event, absoluteMs, activeMs);
        break;
    }
    case RawEventType::KeyUp:
        if (event.virtualKey < keysDown_.size())
            keysDown_[event.virtualKey] = false;
        break;
    case RawEventType::MouseMove:
        handleMouseMove(event, absoluteMs, activeMs);
        break;
    case RawEventType::MouseLeftDown:
        ++result_.rawInputCount;
        if (currentPac_)
            ++currentPac_->rawActionCount;
        handleLeftDown(event, absoluteMs, activeMs);
        break;
    case RawEventType::MouseLeftUp:
        handleLeftUp(event, absoluteMs, activeMs);
        break;
    case RawEventType::MouseRightDown:
        ++result_.rawInputCount;
        if (currentPac_)
            ++currentPac_->rawActionCount;
        handleRightDown(event, absoluteMs, activeMs);
        break;
    case RawEventType::MouseMiddleDown:
    case RawEventType::MouseWheel:
        ++result_.rawInputCount;
        if (currentPac_)
            ++currentPac_->rawActionCount;
        break;
    default:
        break;
    }
}

void Analyzer::handleKeyDown(const RawInputEvent& event, double absoluteMs, double activeMs) {
    const auto key = event.virtualKey;
    if (isModifier(key))
        return;

    const bool ctrl = down(keysDown_, VK_CONTROL) || down(keysDown_, VK_LCONTROL) || down(keysDown_, VK_RCONTROL);
    const bool shift = down(keysDown_, VK_SHIFT) || down(keysDown_, VK_LSHIFT) || down(keysDown_, VK_RSHIFT);

    if (key >= '0' && key <= '9') {
        const int group = static_cast<int>(key - '0');
        if (ctrl) {
            emit(LogicalEventType::ControlGroupAssign, event, Confidence::Observed, group);
            return;
        }

        finalizeSelection(activeMs);
        pendingSwitch_.reset();
        pendingSwitchCompletedStartActiveMs_.reset();
        const bool isDoubleTap =
            lastGroupTapAbsoluteMs_[static_cast<std::size_t>(group)] >= 0.0 &&
            absoluteMs - lastGroupTapAbsoluteMs_[static_cast<std::size_t>(group)] <= config_.controlGroupDoubleTapMs;
        emit(LogicalEventType::ControlGroupSelect, event, Confidence::Observed, group, isDoubleTap ? 1 : 0);
        ++result_.totalSelections;

        if (currentGroup_ >= 0 && currentGroup_ != group) {
            ++result_.controlGroupSwitchCount;
            pendingSwitch_ = PendingSwitch{activeMs};
            pendingSwitchCompletedStartActiveMs_ = activeMs;
        }

        if (groupHistory_.size() >= 2 && groupHistory_[groupHistory_.size() - 2].first == group &&
            groupHistory_.back().first != group) {
            result_.returnLatenciesMs.push_back(activeMs - groupHistory_.back().second);
            pendingReturnActiveMs_ = activeMs;
        }
        if (groupHistory_.empty() || groupHistory_.back().first != group) {
            groupHistory_.emplace_back(group, activeMs);
            if (groupHistory_.size() > 3)
                groupHistory_.pop_front();
        }

        currentGroup_ = group;
        currentGroupSelectedActiveMs_ = activeMs;
        pendingSelection_ = PendingSelection{activeMs, false, std::nullopt};
        lastGroupTapAbsoluteMs_[static_cast<std::size_t>(group)] = absoluteMs;
        effectiveAction(activeMs);
        microActivity(activeMs, absoluteMs, event.sequence);

        if (isDoubleTap) {
            emit(LogicalEventType::ControlGroupDoubleTap, event, Confidence::HighConfidenceInference, group);
            startNavigation(NavigationMethod::ControlGroupJump, event, absoluteMs, activeMs, 0.0, event.cursorX,
                            event.cursorY, false);
        }
        return;
    }

    if (std::find(config_.locationHotkeys.begin(), config_.locationHotkeys.end(), key) !=
        config_.locationHotkeys.end()) {
        const int location = static_cast<int>(key - VK_F1 + 1);
        if (shift) {
            emit(LogicalEventType::LocationHotkeyAssign, event, Confidence::Observed, location);
        } else {
            emit(LogicalEventType::LocationHotkey, event, Confidence::Observed, location);
            effectiveAction(activeMs);
            startNavigation(NavigationMethod::LocationHotkey, event, absoluteMs, activeMs, 0.0, event.cursorX,
                            event.cursorY);
        }
        return;
    }

    if (recognizeMacro(key, activeMs, absoluteMs, event))
        return;

    LogicalEventType command = LogicalEventType::KeyAction;
    bool targeted = false;
    bool substantive = false;
    if (key == config_.attackKey) {
        command = LogicalEventType::AttackCommandStart;
        targeted = substantive = true;
    } else if (key == config_.moveKey) {
        command = LogicalEventType::MoveCommandStart;
        targeted = substantive = true;
    } else if (key == config_.patrolKey) {
        command = LogicalEventType::PatrolCommandStart;
        targeted = substantive = true;
    } else if (key == config_.stopKey) {
        command = LogicalEventType::StopCommand;
        substantive = true;
    } else if (key == config_.holdKey) {
        command = LogicalEventType::HoldCommand;
        substantive = true;
    }

    emit(command, event, Confidence::Observed, static_cast<int>(key));
    if (substantive) {
        if (targeted)
            pendingCommand_ = PendingCommand{command, activeMs};
        else
            pendingCommand_.reset();
        effectiveAction(activeMs);
        microActivity(activeMs, absoluteMs, event.sequence);
        qualifyingAction(activeMs, absoluteMs, event.sequence, false, false, true);
    }
}

void Analyzer::handleMouseMove(const RawInputEvent& event, double absoluteMs, double activeMs) {
    if (drag_) {
        drag_->pathLength += distance(drag_->lastX, drag_->lastY, event.cursorX, event.cursorY);
        drag_->lastX = event.cursorX;
        drag_->lastY = event.cursorY;
        if (!drag_->dragging &&
            distance(drag_->startX, drag_->startY, event.cursorX, event.cursorY) >= config_.dragThresholdPx) {
            drag_->dragging = true;
            if (currentPac_ && !currentPac_->firstActionMs) {
                drag_->contextStartLatencyMs = drag_->downActiveMs - currentPac_->startActiveMs;
            }
            emitAt(LogicalEventType::BoxSelectStart, drag_->downAbsoluteMs, drag_->sourceSequence, Confidence::Observed,
                   drag_->startX, drag_->startY);
            qualifyingAction(drag_->downActiveMs, drag_->downAbsoluteMs, drag_->sourceSequence, true, false, false);
        }
    }

    const int direction = edgeDirection(config_.viewport, config_.edgeThicknessPx, event.cursorX, event.cursorY);
    if (activeEdge_ != 0 && direction != activeEdge_) {
        emit(LogicalEventType::EdgeScrollEnd, event, Confidence::HighConfidenceInference, activeEdge_, 0,
             absoluteMs - activeEdgeStartAbsoluteMs_, 0.0);
        effectiveAction(activeMs);
        startNavigation(NavigationMethod::EdgeScroll, event, absoluteMs, activeMs,
                        absoluteMs - activeEdgeStartAbsoluteMs_, event.cursorX, event.cursorY);
        activeEdge_ = 0;
        candidateEdge_ = 0;
    }
    if (direction == 0) {
        candidateEdge_ = 0;
        return;
    }
    if (activeEdge_ == direction)
        return;
    if (candidateEdge_ != direction) {
        candidateEdge_ = direction;
        candidateEdgeStartAbsoluteMs_ = absoluteMs;
        return;
    }
    if (absoluteMs - candidateEdgeStartAbsoluteMs_ >= config_.edgeDwellMs) {
        activeEdge_ = direction;
        activeEdgeStartAbsoluteMs_ = candidateEdgeStartAbsoluteMs_;
        emitAt(LogicalEventType::EdgeScrollStart, candidateEdgeStartAbsoluteMs_, event.sequence,
               Confidence::HighConfidenceInference, direction);
    }
}

void Analyzer::handleLeftDown(const RawInputEvent& event, double absoluteMs, double activeMs) {
    if (config_.viewport.contains(event.cursorX, event.cursorY)) {
        drag_ = DragState{absoluteMs,    activeMs, event.sequence, event.cursorX, event.cursorY, event.cursorX,
                          event.cursorY, 0.0,      false,          std::nullopt};
    } else {
        drag_.reset();
    }
}

void Analyzer::handleLeftUp(const RawInputEvent& event, double absoluteMs, double activeMs) {
    if (drag_ && drag_->dragging) {
        drag_->pathLength += distance(drag_->lastX, drag_->lastY, event.cursorX, event.cursorY);
        BoxRecord box;
        box.startActiveMs = drag_->downActiveMs;
        box.endActiveMs = activeMs;
        box.startX = drag_->startX;
        box.startY = drag_->startY;
        box.endX = event.cursorX;
        box.endY = event.cursorY;
        box.width = std::abs(box.endX - box.startX);
        box.height = std::abs(box.endY - box.startY);
        box.area = static_cast<double>(box.width) * box.height;
        box.diagonal = distance(box.startX, box.startY, box.endX, box.endY);
        box.pathLength = drag_->pathLength;
        box.pathEfficiency = box.pathLength <= 0.0 ? 0.0 : std::clamp(box.diagonal / box.pathLength, 0.0, 1.0);
        box.direction = boxDirection(box.endX - box.startX, box.endY - box.startY);
        box.contextStartLatencyMs = drag_->contextStartLatencyMs;
        if (drag_->contextStartLatencyMs)
            box.contextCompleteLatencyMs = *drag_->contextStartLatencyMs + (activeMs - drag_->downActiveMs);

        if (!result_.boxes.empty()) {
            const auto& previous = result_.boxes.back();
            const double gap = box.startActiveMs - previous.endActiveMs;
            const double iou =
                rectangleIou(std::min(previous.startX, previous.endX), std::min(previous.startY, previous.endY),
                             std::max(previous.startX, previous.endX), std::max(previous.startY, previous.endY),
                             std::min(box.startX, box.endX), std::min(box.startY, box.endY),
                             std::max(box.startX, box.endX), std::max(box.startY, box.endY));
            box.reselectionGapMs = gap;
            box.reselectionIou = iou;
            box.probableReselection = gap <= config_.reselectionIntervalMs && iou >= config_.reselectionIou;
        }

        result_.boxes.push_back(std::move(box));
        pendingBoxIndex_ = result_.boxes.size() - 1;
        emit(LogicalEventType::BoxSelectEnd, event, Confidence::Observed, result_.boxes.back().width,
             result_.boxes.back().height, activeMs - drag_->downActiveMs, result_.boxes.back().pathEfficiency);
        effectiveAction(activeMs);
        microActivity(activeMs, absoluteMs, event.sequence);
        viewportInteraction(event.cursorX, event.cursorY, absoluteMs);
        drag_.reset();
        return;
    }
    drag_.reset();

    if (config_.minimap.contains(event.cursorX, event.cursorY)) {
        emit(LogicalEventType::MinimapClick, event, Confidence::Observed, event.cursorX, event.cursorY);
        const double navigationDuration = lastViewportInteractionAbsoluteMs_ > 0.0
                                              ? std::max(0.0, absoluteMs - lastViewportInteractionAbsoluteMs_)
                                              : 0.0;
        effectiveAction(activeMs);
        startNavigation(NavigationMethod::MinimapJump, event, absoluteMs, activeMs, navigationDuration, event.cursorX,
                        event.cursorY);
        return;
    }

    if (config_.viewport.contains(event.cursorX, event.cursorY)) {
        viewportInteraction(event.cursorX, event.cursorY, absoluteMs);
        if (pendingCommand_) {
            const auto command = *pendingCommand_;
            emit(LogicalEventType::TargetClick, event, Confidence::Observed, static_cast<int>(command.type), 0,
                 activeMs - command.startActiveMs, 0.0);
            result_.commandTargets.push_back(
                {command.type, activeMs, activeMs - command.startActiveMs, currentLoadEapm(activeMs)});
            pendingCommand_.reset();
            effectiveAction(activeMs);
            microActivity(activeMs, absoluteMs, event.sequence);
            qualifyingAction(activeMs, absoluteMs, event.sequence, false, true, true);
        } else {
            emit(LogicalEventType::ViewportClick, event, Confidence::Observed, event.cursorX, event.cursorY);
            effectiveAction(activeMs);
            qualifyingAction(activeMs, absoluteMs, event.sequence, true, false, false);
        }
    } else if (config_.commandCard.contains(event.cursorX, event.cursorY)) {
        emit(LogicalEventType::CommandCardClick, event, Confidence::Observed, event.cursorX, event.cursorY);
    } else {
        emit(LogicalEventType::OtherUiClick, event, Confidence::Observed, event.cursorX, event.cursorY);
    }
}

void Analyzer::handleRightDown(const RawInputEvent& event, double absoluteMs, double activeMs) {
    emit(LogicalEventType::RightClickCommand, event, Confidence::Observed, event.cursorX, event.cursorY);
    pendingCommand_.reset();
    effectiveAction(activeMs);
    microActivity(activeMs, absoluteMs, event.sequence);
    qualifyingAction(activeMs, absoluteMs, event.sequence, false, true, true);
    if (config_.viewport.contains(event.cursorX, event.cursorY))
        viewportInteraction(event.cursorX, event.cursorY, absoluteMs);
}

void Analyzer::startNavigation(NavigationMethod method, const RawInputEvent& event, double absoluteMs, double activeMs,
                               double durationMs, int cursorX, int cursorY, bool closeSelection) {
    closePac(activeMs, absoluteMs, event.sequence);
    if (closeSelection)
        finalizeSelection(activeMs);
    pendingSwitch_.reset();
    pendingSwitchCompletedStartActiveMs_.reset();
    pendingCommand_.reset();
    result_.navigation.push_back(
        {method, activeMs, durationMs, std::nullopt, std::nullopt, std::nullopt, cursorX, cursorY});
    pendingNavigationIndex_ = result_.navigation.size() - 1;
    emit(LogicalEventType::InferredContextSwitch, event, Confidence::HighConfidenceInference, static_cast<int>(method));
    emit(LogicalEventType::InferredPacStart, event, Confidence::HighConfidenceInference, static_cast<int>(method));
    currentPac_ = PacRecord{activeMs, activeMs, method, std::nullopt, std::nullopt, activeMs, 0, 0, true};
}

void Analyzer::closePac(double activeMs, double absoluteMs, std::uint64_t sequence) {
    if (!currentPac_)
        return;
    currentPac_->endActiveMs = std::max(currentPac_->startActiveMs, activeMs);
    emitAt(LogicalEventType::InferredPacEnd, absoluteMs, sequence, Confidence::HighConfidenceInference,
           static_cast<int>(currentPac_->transition), currentPac_->actionless ? 1 : 0,
           currentPac_->endActiveMs - currentPac_->startActiveMs,
           static_cast<double>(currentPac_->qualifyingActionCount));
    result_.pacs.push_back(*currentPac_);
    currentPac_.reset();
}

void Analyzer::qualifyingAction(double activeMs, double absoluteMs, std::uint64_t sequence, bool selection,
                                bool completedCommand, bool substantiveCommand) {
    if (currentPac_) {
        ++currentPac_->qualifyingActionCount;
        currentPac_->lastActionActiveMs = activeMs;
        currentPac_->actionless = false;
        if (!currentPac_->firstActionMs) {
            currentPac_->firstActionMs = activeMs - currentPac_->startActiveMs;
            result_.pacFirstActionLatencies.push_back(
                {activeMs, *currentPac_->firstActionMs, currentLoadEapm(activeMs)});
            emitAt(LogicalEventType::InferredPacFirstAction, absoluteMs, sequence, Confidence::HighConfidenceInference,
                   static_cast<int>(currentPac_->transition), 0, *currentPac_->firstActionMs, 0.0);
        }
        if (completedCommand && !currentPac_->firstCompletedCommandMs) {
            currentPac_->firstCompletedCommandMs = activeMs - currentPac_->startActiveMs;
        }
    }

    if (pendingNavigationIndex_) {
        auto& navigation = result_.navigation[*pendingNavigationIndex_];
        if (!navigation.firstActionLatencyMs)
            navigation.firstActionLatencyMs = activeMs - navigation.completionActiveMs;
        if (selection && !navigation.firstSelectionLatencyMs)
            navigation.firstSelectionLatencyMs = activeMs - navigation.completionActiveMs;
    }

    if (pendingSwitch_) {
        const double latency = activeMs - pendingSwitch_->startActiveMs;
        result_.controlGroupSwitchLatencies.push_back({activeMs, latency, currentLoadEapm(activeMs)});
        pendingSwitch_.reset();
    }
    if (completedCommand && pendingSwitchCompletedStartActiveMs_) {
        result_.controlGroupCompletedCommandLatenciesMs.push_back(activeMs - *pendingSwitchCompletedStartActiveMs_);
        pendingSwitchCompletedStartActiveMs_.reset();
    }
    if (pendingSelection_ && !pendingSelection_->productive) {
        pendingSelection_->productive = true;
        pendingSelection_->firstActionLatencyMs = activeMs - pendingSelection_->startActiveMs;
    }
    if (pendingReturnActiveMs_) {
        result_.returnToActionLatenciesMs.push_back(activeMs - *pendingReturnActiveMs_);
        pendingReturnActiveMs_.reset();
    }
    if (substantiveCommand && pendingBoxIndex_ && *pendingBoxIndex_ < result_.boxes.size()) {
        auto& box = result_.boxes[*pendingBoxIndex_];
        if (!box.commandLatencyMs)
            box.commandLatencyMs = activeMs - box.endActiveMs;
        pendingBoxIndex_.reset();
    }
}

void Analyzer::effectiveAction(double activeMs) {
    if (!result_.effectiveActionTimesMs.empty()) {
        const double interval = activeMs - result_.effectiveActionTimesMs.back();
        if (interval >= 0.0)
            result_.interActionLatenciesMs.push_back(interval);
    }
    result_.effectiveActionTimesMs.push_back(activeMs);
    ++result_.effectiveActionCount;
}

void Analyzer::viewportInteraction(int x, int y, double absoluteMs) {
    if (pendingNavigationIndex_ && *pendingNavigationIndex_ < result_.navigation.size()) {
        auto& navigation = result_.navigation[*pendingNavigationIndex_];
        if ((navigation.method == NavigationMethod::MinimapJump || navigation.method == NavigationMethod::EdgeScroll) &&
            !navigation.cursorRecoveryDistance) {
            navigation.cursorRecoveryDistance = distance(navigation.cursorX, navigation.cursorY, x, y);
        }
    }
    lastViewportInteractionAbsoluteMs_ = absoluteMs;
}

bool Analyzer::recognizeMacro(std::uint16_t key, double activeMs, double absoluteMs, const RawInputEvent& event) {
    if (currentGroup_ < 0 || currentGroupSelectedActiveMs_ < 0.0 ||
        activeMs - currentGroupSelectedActiveMs_ > config_.macroRecognitionIntervalMs)
        return false;
    const auto matches = [&](const ProductionRule& rule) {
        return rule.group == currentGroup_ &&
               std::find(rule.trainKeys.begin(), rule.trainKeys.end(), key) != rule.trainKeys.end();
    };
    if (std::any_of(config_.workerRules.begin(), config_.workerRules.end(), matches)) {
        addMacroAttempt(true, currentGroup_, key, activeMs, absoluteMs, event);
        return true;
    }
    if (std::any_of(config_.armyRules.begin(), config_.armyRules.end(), matches)) {
        addMacroAttempt(false, currentGroup_, key, activeMs, absoluteMs, event);
        return true;
    }
    return false;
}

void Analyzer::addMacroAttempt(bool worker, int group, std::uint16_t key, double activeMs, double absoluteMs,
                               const RawInputEvent& event) {
    if (workingEpisode_ && activeMs - workingEpisode_->lastAttemptActiveMs > config_.macroEpisodeGapMs) {
        finishMacroEpisode(absoluteMs, event.sequence);
    }
    if (!workingEpisode_) {
        MacroEpisode episode;
        episode.startActiveMs = activeMs;
        episode.endActiveMs = activeMs;
        episode.loadEapm = currentLoadEapm(activeMs);
        workingEpisode_ = WorkingEpisode{std::move(episode), activeMs};
        emit(LogicalEventType::MacroEpisodeStart, event, Confidence::HeuristicInference);
    }
    auto& episode = workingEpisode_->episode;
    episode.endActiveMs = activeMs;
    episode.productionGroups.insert(group);
    if (worker) {
        ++episode.workerAttempts;
        if (!episode.firstWorkerActiveMs)
            episode.firstWorkerActiveMs = activeMs;
    } else {
        ++episode.armyAttempts;
        if (!episode.firstArmyActiveMs)
            episode.firstArmyActiveMs = activeMs;
    }
    workingEpisode_->lastAttemptActiveMs = activeMs;

    result_.macroAttempts.push_back({worker, group, key, activeMs, currentLoadEapm(activeMs)});
    emit(worker ? LogicalEventType::MacroWorkerAttempt : LogicalEventType::MacroArmyAttempt, event,
         Confidence::HighConfidenceInference, group, static_cast<int>(key));
    if (pendingMicroEndActiveMs_) {
        result_.microMacroReturnMs.push_back(activeMs - *pendingMicroEndActiveMs_);
        pendingMicroEndActiveMs_.reset();
    }
    effectiveAction(activeMs);
    qualifyingAction(activeMs, absoluteMs, event.sequence, false, false, true);
}

void Analyzer::finishMacroEpisode(double absoluteMs, std::uint64_t sequence) {
    if (!workingEpisode_)
        return;
    result_.macroEpisodes.push_back(workingEpisode_->episode);
    emitAt(LogicalEventType::MacroEpisodeEnd, absoluteMs, sequence, Confidence::HeuristicInference,
           workingEpisode_->episode.workerAttempts, workingEpisode_->episode.armyAttempts,
           workingEpisode_->episode.endActiveMs - workingEpisode_->episode.startActiveMs, 0.0);
    workingEpisode_.reset();
}

void Analyzer::microActivity(double activeMs, double absoluteMs, std::uint64_t sequence) {
    microEventsActiveMs_.push_back(activeMs);
    while (!microEventsActiveMs_.empty() && activeMs - microEventsActiveMs_.front() > config_.microWindowMs) {
        microEventsActiveMs_.pop_front();
    }
    microLastActivityActiveMs_ = activeMs;
    if (!microActive_ && microEventsActiveMs_.size() >= static_cast<std::size_t>(config_.microMinimumEvents)) {
        microActive_ = true;
        pendingMicroEndActiveMs_.reset();
        emitAt(LogicalEventType::MicroBurstStart, absoluteMs, sequence, Confidence::HeuristicInference,
               static_cast<int>(microEventsActiveMs_.size()));
    }
}

void Analyzer::advanceMicro(double activeMs, double absoluteMs, std::uint64_t sequence) {
    if (!microActive_ || activeMs - microLastActivityActiveMs_ < config_.microEndQuietMs)
        return;
    const double endActiveMs = microLastActivityActiveMs_ + config_.microEndQuietMs;
    const double endAbsoluteMs = absoluteMs - (activeMs - endActiveMs);
    emitAt(LogicalEventType::MicroBurstEnd, endAbsoluteMs, sequence, Confidence::HeuristicInference, 0, 0, endActiveMs,
           0.0);
    microActive_ = false;
    pendingMicroEndActiveMs_ = endActiveMs;
    microEventsActiveMs_.clear();
}

void Analyzer::finalizeSelection(double) {
    if (!pendingSelection_)
        return;
    if (pendingSelection_->productive) {
        ++result_.productiveSelections;
        if (pendingSelection_->firstActionLatencyMs) {
            for (std::size_t i = 0; i < productiveThresholds.size(); ++i) {
                if (*pendingSelection_->firstActionLatencyMs <= productiveThresholds[i])
                    ++result_.productiveWithin[i];
            }
        }
    }
    pendingSelection_.reset();
}

std::vector<LogicalEvent> Analyzer::takeEmittedEvents() {
    std::vector<LogicalEvent> result = std::move(logicalEvents_);
    logicalEvents_.clear();
    emittedCursor_ = 0;
    return result;
}

void Analyzer::finalize(std::uint64_t endingTicks, std::uint64_t droppedEventCount) {
    if (finalized_)
        return;
    const double endingAbsoluteMs = ticksToMs(endingTicks);
    double endingActiveMs = accumulatedActiveMs;
    if (active_) {
        endingActiveMs = activeTimeAt(endingAbsoluteMs);
        advanceMicro(endingActiveMs, endingAbsoluteMs, 0);
        if (microActive_) {
            emitAt(LogicalEventType::MicroBurstEnd, endingAbsoluteMs, 0, Confidence::HeuristicInference, 0, 0,
                   endingActiveMs, 0.0);
            pendingMicroEndActiveMs_ = endingActiveMs;
            microActive_ = false;
        }
        accumulatedActiveMs = endingActiveMs;
    } else if (seenSession_) {
        accumulatedPausedMs += std::max(0.0, endingAbsoluteMs - pauseStartAbsoluteMs);
    }
    closePac(endingActiveMs, endingAbsoluteMs, 0);
    finalizeSelection(endingActiveMs);
    finishMacroEpisode(endingAbsoluteMs, 0);
    result_.activeDurationSeconds = accumulatedActiveMs / 1000.0;
    result_.pausedDurationSeconds = accumulatedPausedMs / 1000.0;
    result_.droppedEventCount = droppedEventCount;
    computeDerivedMetrics();
    finalized_ = true;
}

void Analyzer::computeDerivedMetrics() {
    const double activeMinutes = result_.activeDurationSeconds / 60.0;
    if (activeMinutes > 0.0) {
        result_.rawApm = static_cast<double>(result_.rawInputCount) / activeMinutes;
        result_.effectiveApm = static_cast<double>(result_.effectiveActionCount) / activeMinutes;
        result_.pacRate = static_cast<double>(result_.pacs.size()) / activeMinutes;
        result_.switchesPerMinute = static_cast<double>(result_.controlGroupSwitchCount) / activeMinutes;
    }

    result_.interAction = describe(result_.interActionLatenciesMs);
    result_.pacFirstAction = describe(valuesOf(result_.pacFirstActionLatencies));
    std::vector<double> pacCompletedCommands;
    std::vector<double> pacDurations;
    std::vector<double> pacActions;
    for (const auto& pac : result_.pacs) {
        if (pac.firstCompletedCommandMs)
            pacCompletedCommands.push_back(*pac.firstCompletedCommandMs);
        pacDurations.push_back(pac.endActiveMs - pac.startActiveMs);
        pacActions.push_back(static_cast<double>(pac.qualifyingActionCount));
    }
    result_.pacCompletedCommand = describe(pacCompletedCommands);
    result_.pacDuration = describe(pacDurations);
    result_.pacActions = describe(pacActions);
    result_.controlGroupSwitch = describe(valuesOf(result_.controlGroupSwitchLatencies));
    std::vector<double> commandValues;
    for (const auto& item : result_.commandTargets)
        commandValues.push_back(item.latencyMs);
    result_.commandTarget = describe(commandValues);

    std::vector<double> boxDurations;
    std::vector<double> boxCommands;
    std::vector<double> efficiencies;
    std::size_t reselections = 0;
    for (const auto& box : result_.boxes) {
        boxDurations.push_back(box.endActiveMs - box.startActiveMs);
        efficiencies.push_back(box.pathEfficiency);
        if (box.commandLatencyMs)
            boxCommands.push_back(*box.commandLatencyMs);
        if (box.probableReselection)
            ++reselections;
    }
    result_.boxDuration = describe(boxDurations);
    result_.boxCommand = describe(boxCommands);
    if (result_.boxes.size() >= 5) {
        result_.boxReselectionRate =
            ratio(static_cast<double>(reselections), static_cast<double>(result_.boxes.size()));
    }
    result_.meanBoxPathEfficiency = mean(efficiencies);

    for (std::size_t i = 1; i < result_.boxes.size(); ++i) {
        if (result_.boxes[i - 1].commandLatencyMs && result_.boxes[i].commandLatencyMs) {
            result_.boxCycleDurationsMs.push_back(result_.boxes[i].startActiveMs - result_.boxes[i - 1].startActiveMs);
        }
    }
    result_.boxCycle = describe(result_.boxCycleDurationsMs);

    std::vector<double> workerIntervals;
    double previousWorker = -1.0;
    for (const auto& attempt : result_.macroAttempts) {
        if (!attempt.worker)
            continue;
        if (previousWorker >= 0.0)
            workerIntervals.push_back(attempt.activeTimeMs - previousWorker);
        previousWorker = attempt.activeTimeMs;
    }
    result_.workerInterval = describe(workerIntervals);

    std::vector<double> armyRevisits;
    std::vector<double> armyEpisodeDurations;
    std::vector<double> episodeDurations;
    std::vector<double> coverages;
    std::vector<double> offsets;
    double previousArmyEpisode = -1.0;
    std::set<int> configuredGroups;
    std::set<int> configuredArmyGroups;
    for (const auto& rule : config_.workerRules)
        configuredGroups.insert(rule.group);
    for (const auto& rule : config_.armyRules) {
        configuredGroups.insert(rule.group);
        configuredArmyGroups.insert(rule.group);
    }
    std::vector<double> armyCoverages;
    std::size_t combined = 0;
    for (const auto& episode : result_.macroEpisodes) {
        episodeDurations.push_back(episode.endActiveMs - episode.startActiveMs);
        if (episode.armyAttempts > 0) {
            if (previousArmyEpisode >= 0.0)
                armyRevisits.push_back(episode.startActiveMs - previousArmyEpisode);
            previousArmyEpisode = episode.startActiveMs;
            armyEpisodeDurations.push_back(episode.endActiveMs - episode.startActiveMs);
            if (!configuredArmyGroups.empty()) {
                std::size_t armyGroupCount = 0;
                for (const int group : episode.productionGroups)
                    if (configuredArmyGroups.contains(group))
                        ++armyGroupCount;
                armyCoverages.push_back(static_cast<double>(armyGroupCount) /
                                        static_cast<double>(configuredArmyGroups.size()));
            }
        }
        if (!configuredGroups.empty()) {
            std::size_t count = 0;
            for (const int group : episode.productionGroups)
                if (configuredGroups.contains(group))
                    ++count;
            coverages.push_back(static_cast<double>(count) / static_cast<double>(configuredGroups.size()));
        }
        if (episode.workerAttempts > 0 && episode.armyAttempts > 0) {
            ++combined;
            if (episode.firstWorkerActiveMs && episode.firstArmyActiveMs) {
                offsets.push_back(std::abs(*episode.firstWorkerActiveMs - *episode.firstArmyActiveMs));
            }
        }
    }
    result_.armyRevisit = describe(armyRevisits);
    result_.armyEpisodeDuration = describe(armyEpisodeDurations);
    result_.macroEpisodeDuration = describe(episodeDurations);
    result_.productionGroupCoverage = mean(coverages);
    result_.armyProductionGroupCoverage = mean(armyCoverages);
    if (result_.macroEpisodes.size() >= 5) {
        result_.combinedMacroBurstRatio =
            ratio(static_cast<double>(combined), static_cast<double>(result_.macroEpisodes.size()));
    }
    result_.workerArmyOffsetMedianMs = median(offsets);
    result_.microMacroReturn = describe(result_.microMacroReturnMs);
    if (result_.totalSelections >= 5) {
        result_.productiveSelectionRatio =
            ratio(static_cast<double>(result_.productiveSelections), static_cast<double>(result_.totalSelections));
    }

    computeSequences();
    computeLoadMetrics();
    computeConsistency();
}

void Analyzer::observeSequence(const LogicalEvent& event, double activeMs, double absoluteMs) {
    switch (event.type) {
    case LogicalEventType::KeyAction:
    case LogicalEventType::ControlGroupSelect:
    case LogicalEventType::LocationHotkey:
    case LogicalEventType::AttackCommandStart:
    case LogicalEventType::MoveCommandStart:
    case LogicalEventType::PatrolCommandStart:
    case LogicalEventType::StopCommand:
    case LogicalEventType::HoldCommand:
    case LogicalEventType::TargetClick:
    case LogicalEventType::RightClickCommand:
    case LogicalEventType::BoxSelectEnd:
    case LogicalEventType::MinimapClick:
    case LogicalEventType::EdgeScrollEnd:
    case LogicalEventType::MacroWorkerAttempt:
    case LogicalEventType::MacroArmyAttempt:
        break;
    default:
        return;
    }

    std::string name(logicalEventName(event.type));
    if (event.type == LogicalEventType::ControlGroupSelect)
        name = "CG:" + std::to_string(event.data1);
    else if (event.type == LogicalEventType::KeyAction)
        name = virtualKeyToName(static_cast<std::uint16_t>(event.data1));
    else if (event.type == LogicalEventType::MacroWorkerAttempt || event.type == LogicalEventType::MacroArmyAttempt) {
        name = std::to_string(event.data1) + ":" + virtualKeyToName(static_cast<std::uint16_t>(event.data2));
    }
    sequenceWindow_.push_back({std::move(name), absoluteMs, activeMs});
    if (sequenceWindow_.size() > 8)
        sequenceWindow_.pop_front();

    constexpr std::size_t maximumUniqueSequences = 100000;
    constexpr std::size_t maximumSamplesPerSequence = 20000;
    for (int length = 3; length <= static_cast<int>(sequenceWindow_.size()); ++length) {
        const auto start = sequenceWindow_.size() - static_cast<std::size_t>(length);
        std::ostringstream key;
        for (int offset = 0; offset < length; ++offset) {
            if (offset)
                key << " -> ";
            key << sequenceWindow_[start + static_cast<std::size_t>(offset)].name;
        }
        auto found = sequenceAggregates_.find(key.str());
        if (found == sequenceAggregates_.end()) {
            if (sequenceAggregates_.size() >= maximumUniqueSequences)
                continue;
            found = sequenceAggregates_.emplace(key.str(), SequenceAggregate{}).first;
            found->second.length = length;
            found->second.transitionSums.resize(static_cast<std::size_t>(length - 1));
        }
        auto& aggregate = found->second;
        ++aggregate.count;
        const double duration = sequenceWindow_.back().absoluteMs - sequenceWindow_[start].absoluteMs;
        if (aggregate.durationSamples.size() < maximumSamplesPerSequence) {
            aggregate.durationSamples.push_back(duration);
            aggregate.activeTimeSamples.push_back(activeMs);
        }
        for (int offset = 1; offset < length; ++offset) {
            aggregate.transitionSums[static_cast<std::size_t>(offset - 1)] +=
                sequenceWindow_[start + static_cast<std::size_t>(offset)].absoluteMs -
                sequenceWindow_[start + static_cast<std::size_t>(offset - 1)].absoluteMs;
        }
    }
}

void Analyzer::computeSequences() {
    for (auto& [sequence, aggregate] : sequenceAggregates_) {
        if (aggregate.count < 5)
            continue;
        std::vector<double> means = aggregate.transitionSums;
        for (auto& value : means)
            value /= static_cast<double>(aggregate.count);
        std::vector<TimedMeasurement> observations;
        observations.reserve(aggregate.durationSamples.size());
        for (std::size_t i = 0; i < aggregate.durationSamples.size(); ++i) {
            observations.push_back({aggregate.activeTimeSamples[i], aggregate.durationSamples[i],
                                    currentLoadEapm(aggregate.activeTimeSamples[i])});
        }
        auto duration = describe(aggregate.durationSamples);
        duration.count = aggregate.count;
        result_.sequences.push_back({sequence, aggregate.length, aggregate.count, std::move(duration), std::move(means),
                                     std::move(observations)});
    }
    std::sort(result_.sequences.begin(), result_.sequences.end(), [](const auto& left, const auto& right) {
        if (left.count != right.count)
            return left.count > right.count;
        if (left.length != right.length)
            return left.length > right.length;
        return left.sequence < right.sequence;
    });
}

void Analyzer::computeLoadMetrics() {
    const double durationMs = result_.activeDurationSeconds * 1000.0;
    for (double second = 1000.0; second <= durationMs + 0.001; second += 1000.0) {
        result_.rollingEapm.push_back(currentLoadEapm(second));
    }

    for (auto& observation : result_.pacFirstActionLatencies)
        observation.loadEapm = currentLoadEapm(observation.activeTimeMs);
    for (auto& observation : result_.controlGroupSwitchLatencies)
        observation.loadEapm = currentLoadEapm(observation.activeTimeMs);
    for (auto& item : result_.commandTargets)
        item.loadEapm = currentLoadEapm(item.activeTimeMs);
    for (auto& item : result_.macroAttempts)
        item.loadEapm = currentLoadEapm(item.activeTimeMs);
    for (auto& item : result_.macroEpisodes)
        item.loadEapm = currentLoadEapm(item.startActiveMs);

    struct BinData {
        const char* label;
        double lower;
        double upper;
        std::vector<double> pac;
        std::vector<double> switches;
        std::vector<double> command;
        std::vector<double> worker;
        std::vector<double> army;
    };
    std::array<BinData, 6> bins{{{"0-100", 0, 100},
                                 {"100-150", 100, 150},
                                 {"150-200", 150, 200},
                                 {"200-250", 200, 250},
                                 {"250-300", 250, 300},
                                 {"300+", 300, std::numeric_limits<double>::infinity()}}};
    const auto binFor = [&](double load) -> BinData& {
        return bins[static_cast<std::size_t>(mechanicalLoadBin(load))];
    };
    for (const auto& value : result_.pacFirstActionLatencies)
        binFor(value.loadEapm).pac.push_back(value.valueMs);
    for (const auto& value : result_.controlGroupSwitchLatencies)
        binFor(value.loadEapm).switches.push_back(value.valueMs);
    for (const auto& value : result_.commandTargets)
        binFor(value.loadEapm).command.push_back(value.latencyMs);

    double previousWorker = -1.0;
    for (const auto& attempt : result_.macroAttempts) {
        if (!attempt.worker)
            continue;
        if (previousWorker >= 0.0)
            binFor(attempt.loadEapm).worker.push_back(attempt.activeTimeMs - previousWorker);
        previousWorker = attempt.activeTimeMs;
    }
    double previousArmy = -1.0;
    for (const auto& episode : result_.macroEpisodes) {
        if (episode.armyAttempts == 0)
            continue;
        if (previousArmy >= 0.0)
            binFor(episode.loadEapm).army.push_back(episode.startActiveMs - previousArmy);
        previousArmy = episode.startActiveMs;
    }

    for (const auto& bin : bins) {
        const auto observations =
            bin.pac.size() + bin.switches.size() + bin.command.size() + bin.worker.size() + bin.army.size();
        result_.loadBins.push_back({bin.label, bin.lower, observations,
                                    describe(bin.pac, static_cast<std::size_t>(config_.loadMinimumObservations)),
                                    describe(bin.switches, static_cast<std::size_t>(config_.loadMinimumObservations)),
                                    describe(bin.command, static_cast<std::size_t>(config_.loadMinimumObservations)),
                                    describe(bin.worker, static_cast<std::size_t>(config_.loadMinimumObservations)),
                                    describe(bin.army, static_cast<std::size_t>(config_.loadMinimumObservations))});
    }

    std::vector<CapacityBinInput> capacityInputs;
    for (const auto& bin : result_.loadBins) {
        capacityInputs.push_back(
            {bin.lowerEdge, {bin.pacLatency.median, bin.switchLatency.median, bin.commandTargetLatency.median}});
    }
    result_.capacityBreakpointEapm = estimateCapacityBreakpoint(capacityInputs);

    const auto lowThreshold = median(result_.rollingEapm);
    const auto highThreshold = percentile(result_.rollingEapm, 0.75);
    if (lowThreshold && highThreshold) {
        std::vector<double> workerLow, workerHigh, armyLow, armyHigh, durationLow, durationHigh;
        previousWorker = -1.0;
        for (const auto& attempt : result_.macroAttempts) {
            if (!attempt.worker)
                continue;
            if (previousWorker >= 0.0) {
                const double interval = attempt.activeTimeMs - previousWorker;
                if (attempt.loadEapm < *lowThreshold)
                    workerLow.push_back(interval);
                if (attempt.loadEapm >= *highThreshold)
                    workerHigh.push_back(interval);
            }
            previousWorker = attempt.activeTimeMs;
        }
        previousArmy = -1.0;
        for (const auto& episode : result_.macroEpisodes) {
            const double duration = episode.endActiveMs - episode.startActiveMs;
            if (episode.loadEapm < *lowThreshold)
                durationLow.push_back(duration);
            if (episode.loadEapm >= *highThreshold)
                durationHigh.push_back(duration);
            if (episode.armyAttempts > 0) {
                if (previousArmy >= 0.0) {
                    const double interval = episode.startActiveMs - previousArmy;
                    if (episode.loadEapm < *lowThreshold)
                        armyLow.push_back(interval);
                    if (episode.loadEapm >= *highThreshold)
                        armyHigh.push_back(interval);
                }
                previousArmy = episode.startActiveMs;
            }
        }
        const auto change = [](const std::vector<double>& low,
                               const std::vector<double>& high) -> std::optional<double> {
            const auto lowMedian = median(low);
            const auto highMedian = median(high);
            if (!lowMedian || !highMedian)
                return std::nullopt;
            return percentageChange(*lowMedian, *highMedian);
        };
        result_.workerHighLoadChangePct = change(workerLow, workerHigh);
        result_.armyHighLoadChangePct = change(armyLow, armyHigh);
        result_.macroDurationHighLoadChangePct = change(durationLow, durationHigh);
    }
}

void Analyzer::computeConsistency() {
    const double activeMinutes = result_.activeDurationSeconds / 60.0;
    const auto pac = valuesOf(result_.pacFirstActionLatencies);
    const auto switches = valuesOf(result_.controlGroupSwitchLatencies);
    std::vector<double> commands, boxDurations, boxCommands, workers, army, sequenceDurations;
    for (const auto& item : result_.commandTargets)
        commands.push_back(item.latencyMs);
    for (const auto& box : result_.boxes) {
        boxDurations.push_back(box.endActiveMs - box.startActiveMs);
        if (box.commandLatencyMs)
            boxCommands.push_back(*box.commandLatencyMs);
    }
    double previousWorker = -1.0;
    for (const auto& attempt : result_.macroAttempts)
        if (attempt.worker) {
            if (previousWorker >= 0.0)
                workers.push_back(attempt.activeTimeMs - previousWorker);
            previousWorker = attempt.activeTimeMs;
        }
    double previousArmy = -1.0;
    for (const auto& episode : result_.macroEpisodes)
        if (episode.armyAttempts > 0) {
            if (previousArmy >= 0.0)
                army.push_back(episode.startActiveMs - previousArmy);
            previousArmy = episode.startActiveMs;
        }
    if (!result_.sequences.empty()) {
        for (const auto& observation : result_.sequences.front().observations)
            sequenceDurations.push_back(observation.valueMs);
    }

    addLapse(result_.lapsesPerMinute, "pac_first_action", pac, activeMinutes);
    addLapse(result_.lapsesPerMinute, "control_group_switch", switches, activeMinutes);
    addLapse(result_.lapsesPerMinute, "command_target", commands, activeMinutes);
    addLapse(result_.lapsesPerMinute, "box_duration", boxDurations, activeMinutes);
    addLapse(result_.lapsesPerMinute, "box_command", boxCommands, activeMinutes);
    addLapse(result_.lapsesPerMinute, "worker_interval", workers, activeMinutes);
    addLapse(result_.lapsesPerMinute, "army_revisit", army, activeMinutes);
    addLapse(result_.lapsesPerMinute, "sequence_duration", sequenceDurations, activeMinutes);

    const double durationMs = result_.activeDurationSeconds * 1000.0;
    result_.lateSessionChangePct["pac_first_action"] = lateChange(result_.pacFirstActionLatencies, durationMs);
    result_.lateSessionChangePct["control_group_switch"] = lateChange(result_.controlGroupSwitchLatencies, durationMs);
    std::vector<TimedMeasurement> commandTimed;
    for (const auto& item : result_.commandTargets)
        commandTimed.push_back({item.activeTimeMs, item.latencyMs, item.loadEapm});
    result_.lateSessionChangePct["command_target"] = lateChange(commandTimed, durationMs);

    std::vector<TimedMeasurement> workerTimed, armyTimed;
    previousWorker = -1.0;
    for (const auto& attempt : result_.macroAttempts)
        if (attempt.worker) {
            if (previousWorker >= 0.0)
                workerTimed.push_back({attempt.activeTimeMs, attempt.activeTimeMs - previousWorker, attempt.loadEapm});
            previousWorker = attempt.activeTimeMs;
        }
    previousArmy = -1.0;
    for (const auto& episode : result_.macroEpisodes)
        if (episode.armyAttempts > 0) {
            if (previousArmy >= 0.0)
                armyTimed.push_back({episode.startActiveMs, episode.startActiveMs - previousArmy, episode.loadEapm});
            previousArmy = episode.startActiveMs;
        }
    result_.lateSessionChangePct["worker_interval"] = lateChange(workerTimed, durationMs);
    result_.lateSessionChangePct["army_revisit"] = lateChange(armyTimed, durationMs);
    result_.lateSessionChangePct["sequence_duration"] =
        result_.sequences.empty() ? std::nullopt : lateChange(result_.sequences.front().observations, durationMs);
}

} // namespace scm
