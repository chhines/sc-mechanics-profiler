#pragma once

#include "capture/raw_event.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace smp {

bool registerRawInput(HWND target);
void unregisterRawInput();
std::size_t decodeRawInput(LPARAM rawInputHandle, std::uint64_t timestamp, std::array<RawInputEvent, 8>& output);

} // namespace smp
