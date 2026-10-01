#pragma once

#include "capture/raw_event.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace smp {

bool registerRawInput(HWND target);
void unregisterRawInput();
std::size_t decodeRawInput(LPARAM rawInputHandle, std::uint64_t timestamp, POINT messageCursor, std::array<RawInputEvent, 8>& output);

// Pure packet decoding: screen position is supplied by the queued message.
std::size_t decodeRawInputPacket(const RAWINPUT& input, std::uint64_t timestamp, POINT messageCursor,
                               std::array<RawInputEvent, 8>& output);

} // namespace smp
