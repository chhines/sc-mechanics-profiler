#pragma once

#include "config/config.h"

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
ScreenRegions calculateStarcraftScreenRegions(const ScreenRect& clientArea);
std::optional<ScreenRegions> detectScreenRegionsForWindow(HWND window);
std::optional<ScreenRegions> detectForegroundStarcraftScreenRegions(const std::wstring& expectedExecutable);
ScreenRect displayBoundsFor(const ScreenRect& gameArea) noexcept;

NormalizedScreenRect normalizeScreenRect(const ScreenRect& rect, const ScreenRect& gameArea);
ScreenRect reconstructScreenRect(const NormalizedScreenRect& normalized, const ScreenRect& gameArea) noexcept;
bool isReasonableMinimapRect(const ScreenRect& minimap, const ScreenRect& gameArea) noexcept;
NormalizedScreenRect automaticMinimapNormalizedRect() noexcept;
ScreenRect deriveAutomaticMinimapRect(const ScreenRect& gameArea) noexcept;
ResolvedMinimapRegion resolveMinimapRegion(
    const ScreenRect& gameArea, MinimapMode mode,
    const std::optional<NormalizedScreenRect>& calibratedOverride) noexcept;
const char* minimapRegionSourceName(MinimapRegionSource source) noexcept;

ScreenRegion classifyScreenRegion(const ScreenRegions& regions, ScreenPoint point) noexcept;
const char* screenRegionName(ScreenRegion region) noexcept;
EdgeDirection edgeDirectionAt(const ScreenRect& gameArea, int marginPx, ScreenPoint point) noexcept;
const char* edgeDirectionName(EdgeDirection direction) noexcept;

} // namespace smp
