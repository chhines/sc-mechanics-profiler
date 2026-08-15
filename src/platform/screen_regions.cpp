#include "platform/screen_regions.h"

#include "platform/foreground.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace smp {
namespace {

ScreenRect fromWin32Rect(const RECT& value) {
    return {value.left, value.top, value.right - 1, value.bottom - 1};
}

int reconstructedCoordinate(int origin, int size, double normalized) {
    return origin + static_cast<int>(std::lround(normalized * static_cast<double>(size)));
}

} // namespace

ScreenRect derive4x3GameArea(const ScreenRect& clientArea) {
    if (!clientArea.valid())
        throw std::invalid_argument("StarCraft client area is empty");

    constexpr double targetAspect = 4.0 / 3.0;
    const double clientAspect = static_cast<double>(clientArea.width()) / clientArea.height();
    if (clientAspect > targetAspect) {
        const int width = static_cast<int>(std::lround(clientArea.height() * targetAspect));
        const int xOffset = (clientArea.width() - width) / 2;
        return {clientArea.left + xOffset, clientArea.top, clientArea.left + xOffset + width - 1,
                clientArea.bottom};
    }
    if (clientAspect < targetAspect) {
        const int height = static_cast<int>(std::lround(clientArea.width() / targetAspect));
        const int yOffset = (clientArea.height() - height) / 2;
        return {clientArea.left, clientArea.top + yOffset, clientArea.right,
                clientArea.top + yOffset + height - 1};
    }
    return clientArea;
}

ScreenRegions calculateStarcraftScreenRegions(
    const ScreenRect& clientArea, StarcraftDisplayMode displayMode) {
    ScreenRegions regions;
    regions.clientArea = clientArea;
    regions.displayMode = displayMode;
    if (displayMode == StarcraftDisplayMode::Widescreen) {
        regions.gameArea = clientArea;
    } else {
        // Unknown deliberately retains the explicit original-aspect fallback.
        regions.gameArea = derive4x3GameArea(clientArea);
    }
    regions.viewport = regions.gameArea;
    return regions;
}

std::optional<ScreenRegions> detectScreenRegionsForWindow(
    HWND window, StarcraftDisplayMode displayMode) {
    if (!window)
        return std::nullopt;

    RECT client{};
    if (!GetClientRect(window, &client))
        return std::nullopt;
    POINT corners[2]{{client.left, client.top}, {client.right, client.bottom}};
    SetLastError(ERROR_SUCCESS);
    if (MapWindowPoints(window, nullptr, corners, 2) == 0 && GetLastError() != ERROR_SUCCESS)
        return std::nullopt;

    const ScreenRect clientArea{corners[0].x, corners[0].y, corners[1].x - 1, corners[1].y - 1};
    if (!clientArea.valid())
        return std::nullopt;
    return calculateStarcraftScreenRegions(clientArea, displayMode);
}

std::optional<ScreenRegions> detectForegroundStarcraftScreenRegions(
    const std::wstring& expectedExecutable, StarcraftDisplayMode displayMode) {
    ForegroundMatcher matcher(expectedExecutable);
    if (!matcher.matchesForeground())
        return std::nullopt;
    return detectScreenRegionsForWindow(GetForegroundWindow(), displayMode);
}

ScreenRect displayBoundsFor(const ScreenRect& gameArea) noexcept {
    POINT center{gameArea.left + gameArea.width() / 2, gameArea.top + gameArea.height() / 2};
    const HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info))
        return fromWin32Rect(info.rcMonitor);
    return {0, 0, GetSystemMetrics(SM_CXSCREEN) - 1, GetSystemMetrics(SM_CYSCREEN) - 1};
}

NormalizedScreenRect normalizeScreenRect(const ScreenRect& rect, const ScreenRect& gameArea) {
    if (!isReasonableMinimapRect(rect, gameArea))
        throw std::invalid_argument("Minimap rectangle is invalid or outside the StarCraft game area");
    const int gameWidth = gameArea.width();
    const int gameHeight = gameArea.height();
    if (gameWidth <= 0 || gameHeight <= 0)
        throw std::invalid_argument("StarCraft game area is empty");
    return {static_cast<double>(rect.left - gameArea.left) / gameWidth,
            static_cast<double>(rect.top - gameArea.top) / gameHeight,
            static_cast<double>(rect.right - gameArea.left) / gameWidth,
            static_cast<double>(rect.bottom - gameArea.top) / gameHeight};
}

ScreenRect reconstructScreenRect(const NormalizedScreenRect& normalized, const ScreenRect& gameArea) noexcept {
    if (!normalized.valid() || !gameArea.valid())
        return {};
    ScreenRect result{reconstructedCoordinate(gameArea.left, gameArea.width(), normalized.left),
                      reconstructedCoordinate(gameArea.top, gameArea.height(), normalized.top),
                      reconstructedCoordinate(gameArea.left, gameArea.width(), normalized.right),
                      reconstructedCoordinate(gameArea.top, gameArea.height(), normalized.bottom)};
    result.left = std::clamp(result.left, gameArea.left, gameArea.right);
    result.right = std::clamp(result.right, gameArea.left, gameArea.right);
    result.top = std::clamp(result.top, gameArea.top, gameArea.bottom);
    result.bottom = std::clamp(result.bottom, gameArea.top, gameArea.bottom);
    return result;
}

bool isReasonableMinimapRect(const ScreenRect& minimap, const ScreenRect& gameArea) noexcept {
    return minimap.valid() && gameArea.valid() && minimap.right > minimap.left && minimap.bottom > minimap.top &&
           minimap.width() >= 16 && minimap.height() >= 16 && gameArea.contains({minimap.left, minimap.top}) &&
           gameArea.contains({minimap.right, minimap.bottom});
}

NormalizedScreenRect automaticMinimapNormalizedRect() noexcept {
    // Canonical full minimap geometry measured against a 1440x1080 4:3 game
    // area. Rational constants preserve the measured inclusive-pixel
    // fixture without tying the result to desktop coordinates.
    return {14.0 / 1440.0, 783.0 / 1080.0,
            301.0 / 1440.0, 1070.0 / 1080.0};
}

NormalizedScreenRect originalAspectAutomaticMinimapNormalizedRect() noexcept {
    return automaticMinimapNormalizedRect();
}

NormalizedScreenRect widescreenAutomaticMinimapNormalizedRect() noexcept {
    // Canonical full minimap geometry measured against a 1920x1080
    // widescreen game area.
    return {13.0 / 1920.0, 783.0 / 1080.0,
            300.0 / 1920.0, 1070.0 / 1080.0};
}

ScreenRect deriveOriginalAspectAutomaticMinimapRect(
    const ScreenRect& gameArea) noexcept {
    if (!gameArea.valid())
        return {};
    const auto minimap = reconstructScreenRect(
        originalAspectAutomaticMinimapNormalizedRect(), gameArea);
    return isReasonableMinimapRect(minimap, gameArea) ? minimap : ScreenRect{};
}

ScreenRect deriveWidescreenAutomaticMinimapRect(
    const ScreenRect& gameArea) noexcept {
    if (!gameArea.valid())
        return {};
    const auto minimap = reconstructScreenRect(
        widescreenAutomaticMinimapNormalizedRect(), gameArea);
    return isReasonableMinimapRect(minimap, gameArea) ? minimap : ScreenRect{};
}

ResolvedMinimapRegion resolveMinimapRegion(
    const ScreenRegions& regions, MinimapMode originalAspectMode,
    MinimapMode widescreenMode,
    const std::optional<NormalizedScreenRect>& calibratedOverride,
    const std::optional<NormalizedScreenRect>& widescreenCalibratedOverride) noexcept {
    ResolvedMinimapRegion result;
    const bool widescreen =
        regions.displayMode == StarcraftDisplayMode::Widescreen;
    result.automaticCandidate = widescreen
                                    ? deriveWidescreenAutomaticMinimapRect(
                                          regions.gameArea)
                                    : deriveOriginalAspectAutomaticMinimapRect(
                                          regions.gameArea);
    const auto& selectedOverride = widescreen
                                       ? widescreenCalibratedOverride
                                       : calibratedOverride;
    const auto selectedMode = widescreen ? widescreenMode
                                         : originalAspectMode;
    if (selectedMode == MinimapMode::CalibratedOverride && selectedOverride &&
        selectedOverride->valid()) {
        const auto calibrated = reconstructScreenRect(
            *selectedOverride, regions.gameArea);
        if (isReasonableMinimapRect(calibrated, regions.gameArea)) {
            result.rect = calibrated;
            result.source = MinimapRegionSource::CalibratedOverride;
            return result;
        }
    }
    if (result.automaticCandidate.valid()) {
        result.rect = result.automaticCandidate;
        result.source = MinimapRegionSource::Automatic;
    }
    return result;
}

const char* minimapRegionSourceName(MinimapRegionSource source) noexcept {
    switch (source) {
    case MinimapRegionSource::Automatic:
        return "automatic";
    case MinimapRegionSource::CalibratedOverride:
        return "calibrated override";
    default:
        return "unavailable";
    }
}

ScreenRegion classifyScreenRegion(const ScreenRegions& regions, ScreenPoint point) noexcept {
    if (regions.minimap.contains(point))
        return ScreenRegion::Minimap;
    if (regions.commandCard.contains(point))
        return ScreenRegion::CommandCard;
    if (regions.viewport.contains(point))
        return ScreenRegion::Viewport;
    if (regions.gameArea.contains(point))
        return ScreenRegion::OtherUi;
    return ScreenRegion::Outside;
}

const char* screenRegionName(ScreenRegion region) noexcept {
    switch (region) {
    case ScreenRegion::Viewport:
        return "VIEWPORT";
    case ScreenRegion::Minimap:
        return "MINIMAP";
    case ScreenRegion::CommandCard:
        return "COMMAND_CARD";
    case ScreenRegion::OtherUi:
        return "OTHER_UI";
    default:
        return "OUTSIDE";
    }
}

EdgeDirection edgeDirectionAt(const ScreenRect& gameArea, int marginPx, ScreenPoint point) noexcept {
    if (!gameArea.contains(point) || marginPx <= 0)
        return EdgeDirection::None;
    const bool left = point.x < gameArea.left + marginPx;
    const bool right = point.x > gameArea.right - marginPx;
    const bool top = point.y < gameArea.top + marginPx;
    const bool bottom = point.y > gameArea.bottom - marginPx;
    if (top && left)
        return EdgeDirection::TopLeft;
    if (top && right)
        return EdgeDirection::TopRight;
    if (bottom && left)
        return EdgeDirection::BottomLeft;
    if (bottom && right)
        return EdgeDirection::BottomRight;
    if (left)
        return EdgeDirection::Left;
    if (right)
        return EdgeDirection::Right;
    if (top)
        return EdgeDirection::Top;
    if (bottom)
        return EdgeDirection::Bottom;
    return EdgeDirection::None;
}

const char* edgeDirectionName(EdgeDirection direction) noexcept {
    switch (direction) {
    case EdgeDirection::Left:
        return "LEFT";
    case EdgeDirection::Right:
        return "RIGHT";
    case EdgeDirection::Top:
        return "TOP";
    case EdgeDirection::Bottom:
        return "BOTTOM";
    case EdgeDirection::TopLeft:
        return "TOP_LEFT";
    case EdgeDirection::TopRight:
        return "TOP_RIGHT";
    case EdgeDirection::BottomLeft:
        return "BOTTOM_LEFT";
    case EdgeDirection::BottomRight:
        return "BOTTOM_RIGHT";
    default:
        return "NONE";
    }
}

} // namespace smp
