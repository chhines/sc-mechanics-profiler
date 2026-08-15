#pragma once

#include "config/config.h"
#include "platform/starcraft_display_mode.h"

#include <optional>
#include <string>
#include <windows.h>

namespace smp {

struct ScreenRegions {
    ScreenRect clientArea{};
    ScreenRect gameArea{};
    ScreenRect viewport{};
    ScreenRect minimap{};
    ScreenRect commandCard{};
    StarcraftDisplayMode displayMode{StarcraftDisplayMode::Unknown};
};

enum class ScreenRegion {
    Outside,
    Viewport,
    Minimap,
    CommandCard,
    OtherUi
};

enum class EdgeDirection {
    None,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};

enum class MinimapRegionSource {
    Unavailable,
    Automatic,
    CalibratedOverride,
};

struct ResolvedMinimapRegion {
    ScreenRect rect{};
    ScreenRect automaticCandidate{};
    MinimapRegionSource source{MinimapRegionSource::Unavailable};
};

ScreenRect derive4x3GameArea(const ScreenRect& clientArea);
ScreenRegions calculateStarcraftScreenRegions(
    const ScreenRect& clientArea, StarcraftDisplayMode displayMode);
std::optional<ScreenRegions> detectScreenRegionsForWindow(
    HWND window, StarcraftDisplayMode displayMode);
std::optional<ScreenRegions> detectForegroundStarcraftScreenRegions(
    const std::wstring& expectedExecutable, StarcraftDisplayMode displayMode);
ScreenRect displayBoundsFor(const ScreenRect& gameArea) noexcept;

NormalizedScreenRect normalizeScreenRect(const ScreenRect& rect, const ScreenRect& gameArea);
ScreenRect reconstructScreenRect(const NormalizedScreenRect& normalized, const ScreenRect& gameArea) noexcept;
bool isReasonableMinimapRect(const ScreenRect& minimap, const ScreenRect& gameArea) noexcept;
// Retained as the Original Aspect profile for compatibility with existing callers.
NormalizedScreenRect automaticMinimapNormalizedRect() noexcept;
NormalizedScreenRect originalAspectAutomaticMinimapNormalizedRect() noexcept;
NormalizedScreenRect widescreenAutomaticMinimapNormalizedRect() noexcept;
ScreenRect deriveOriginalAspectAutomaticMinimapRect(
    const ScreenRect& gameArea) noexcept;
ScreenRect deriveWidescreenAutomaticMinimapRect(
    const ScreenRect& gameArea) noexcept;
ResolvedMinimapRegion resolveMinimapRegion(
    const ScreenRegions& regions, MinimapMode mode,
    const std::optional<NormalizedScreenRect>& calibratedOverride,
    const std::optional<NormalizedScreenRect>& widescreenCalibratedOverride) noexcept;
const char* minimapRegionSourceName(MinimapRegionSource source) noexcept;

ScreenRegion classifyScreenRegion(const ScreenRegions& regions, ScreenPoint point) noexcept;
const char* screenRegionName(ScreenRegion region) noexcept;
EdgeDirection edgeDirectionAt(const ScreenRect& gameArea, int marginPx, ScreenPoint point) noexcept;
const char* edgeDirectionName(EdgeDirection direction) noexcept;

} // namespace smp
