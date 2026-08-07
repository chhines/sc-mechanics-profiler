#pragma once

#include <cstdint>
#include <type_traits>

namespace scm {

enum class RawEventType : std::uint8_t {
    KeyDown,
    KeyUp,
    MouseMove,
    MouseLeftDown,
    MouseLeftUp,
    MouseRightDown,
    MouseRightUp,
    MouseMiddleDown,
    MouseMiddleUp,
    MouseWheel,
    ForegroundGained,
    ForegroundLost
};

struct RawInputEvent {
    std::uint64_t sequence{};
    std::uint64_t timestampTicks{};
    RawEventType type{};
    std::uint8_t reserved{};
    std::uint16_t scanCode{};
    std::uint16_t virtualKey{};
    std::int32_t mouseDx{};
    std::int32_t mouseDy{};
    std::int32_t cursorX{};
    std::int32_t cursorY{};
    std::int16_t wheelDelta{};
    std::uint16_t flags{};
};

constexpr std::uint16_t RawEventFlagPolledCursor = 0x8000;

static_assert(std::is_trivially_copyable_v<RawInputEvent>);

} // namespace scm
