#pragma once

#include "config/config.h"
#include "platform/minimap_viewport_detector.h"

namespace smp {
ScreenRect replayUiProbeRect(const ScreenRect& gameArea) noexcept;
// Positive evidence only: a hidden replay panel cannot be distinguished here.
// Three strong lower-right transport rings suffice; no progress bar is required.
bool containsReplayTransportPanel(const BgraImageView& image) noexcept;
} // namespace smp
