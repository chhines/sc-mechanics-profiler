#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace scm {

enum class Confidence : std::uint8_t {
    Observed,
    HighConfidenceInference,
    HeuristicInference
};

enum class LogicalEventType : std::uint8_t {
    KeyAction,
    ControlGroupSelect,
    ControlGroupAssign,
    ControlGroupDoubleTap,
    LocationHotkey,
    LocationHotkeyAssign,
    AttackCommandStart,
    MoveCommandStart,
    PatrolCommandStart,
    StopCommand,
    HoldCommand,
    TargetClick,
    RightClickCommand,
    BoxSelectStart,
    BoxSelectEnd,
    MinimapClick,
    ViewportClick,
    CommandCardClick,
    OtherUiClick,
    EdgeScrollStart,
    EdgeScrollEnd,
    MacroWorkerAttempt,
    MacroArmyAttempt,
    MacroEpisodeStart,
    MacroEpisodeEnd,
    MicroBurstStart,
    MicroBurstEnd,
    InferredContextSwitch,
    InferredPacStart,
    InferredPacFirstAction,
    InferredPacEnd
};

enum class NavigationMethod : std::int32_t {
    None = 0,
    ControlGroupJump = 1,
    LocationHotkey = 2,
    MinimapJump = 3,
    EdgeScroll = 4
};

struct LogicalEvent {
    std::uint64_t timestampTicks{};
    std::uint64_t sourceSequence{};
    LogicalEventType type{};
    Confidence confidence{Confidence::Observed};
    std::uint16_t flags{};
    std::int32_t data1{};
    std::int32_t data2{};
    double value1{};
    double value2{};
};

static_assert(std::is_trivially_copyable_v<LogicalEvent>);

std::string_view logicalEventName(LogicalEventType type) noexcept;
std::string_view navigationMethodName(NavigationMethod method) noexcept;

} // namespace scm
