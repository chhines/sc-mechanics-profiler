#include "test_framework.h"

#include "platform/screen_region_overlay.h"
#include "platform/screen_regions.h"

#include <cmath>

namespace {

void requireRectNear(const smp::ScreenRect& actual, const smp::ScreenRect& expected, int tolerance = 1) {
    REQUIRE(std::abs(actual.left - expected.left) <= tolerance);
    REQUIRE(std::abs(actual.top - expected.top) <= tolerance);
    REQUIRE(std::abs(actual.right - expected.right) <= tolerance);
    REQUIRE(std::abs(actual.bottom - expected.bottom) <= tolerance);
}

std::pair<smp::ScreenRegions, smp::ResolvedMinimapRegion> automaticRegions(
    const smp::ScreenRect& client) {
    auto regions = smp::calculateStarcraftScreenRegions(client);
    auto resolved = smp::resolveMinimapRegion(
        regions.gameArea, smp::MinimapMode::Automatic, std::nullopt);
    regions.minimap = resolved.rect;
    return {regions, resolved};
}

} // namespace

TEST_CASE("1920 by 1080 client deterministically derives the centered 4 by 3 game area") {
    const smp::ScreenRect client{0, 0, 1919, 1079};
    const auto game = smp::derive4x3GameArea(client);
    REQUIRE((game == smp::ScreenRect{240, 0, 1679, 1079}));
    REQUIRE(game.width() == 1440);
    REQUIRE(game.height() == 1080);
}

TEST_CASE("automatic minimap reproduces the known good 1920 by 1080 fixture") {
    const auto [regions, resolved] =
        automaticRegions({0, 0, 1919, 1079});
    REQUIRE((regions.gameArea == smp::ScreenRect{240, 0, 1679, 1079}));
    requireRectNear(regions.minimap, {277, 799, 519, 1040});
    REQUIRE(resolved.source == smp::MinimapRegionSource::Automatic);
}

TEST_CASE("identical client geometry resolves identical automatic minimap after focus regain") {
    const smp::ScreenRect client{0, 0, 1919, 1079};
    const auto [first, firstResolution] = automaticRegions(client);
    const auto [regained, regainedResolution] = automaticRegions(client);
    REQUIRE(first.clientArea == regained.clientArea);
    REQUIRE(first.gameArea == regained.gameArea);
    REQUIRE(first.minimap == regained.minimap);
    REQUIRE(firstResolution.source == regainedResolution.source);
}

TEST_CASE("normalized minimap calibration round trips within inclusive-coordinate rounding tolerance") {
    const smp::ScreenRect game{240, 0, 1679, 1079};
    const smp::ScreenRect minimap{277, 799, 519, 1040};
    const auto normalized = smp::normalizeScreenRect(minimap, game);
    requireRectNear(smp::reconstructScreenRect(normalized, game), minimap);
}

TEST_CASE("moving a same-sized client moves the automatic minimap by the same desktop offset") {
    const smp::ScreenRect movedClient{100, 200, 2019, 1279};
    const auto [moved, resolved] = automaticRegions(movedClient);
    requireRectNear(moved.minimap, {377, 999, 619, 1240});
    REQUIRE(resolved.source == smp::MinimapRegionSource::Automatic);
}

TEST_CASE("automatic minimap position is preserved in a resized proportional game area") {
    const auto normalized = smp::automaticMinimapNormalizedRect();
    const smp::ScreenRect resizedGame{0, 0, 799, 599};
    const auto resizedMinimap = smp::deriveAutomaticMinimapRect(resizedGame);
    const auto resizedNormalized = smp::normalizeScreenRect(resizedMinimap, resizedGame);
    REQUIRE_NEAR(resizedNormalized.left, normalized.left, 0.002);
    REQUIRE_NEAR(resizedNormalized.top, normalized.top, 0.002);
    REQUIRE_NEAR(resizedNormalized.right, normalized.right, 0.002);
    REQUIRE_NEAR(resizedNormalized.bottom, normalized.bottom, 0.002);
}

TEST_CASE("automatic minimap remains inside a 4 by 3 client") {
    const auto [regions, resolved] = automaticRegions({0, 0, 1439, 1079});
    REQUIRE(regions.gameArea == regions.clientArea);
    REQUIRE(smp::isReasonableMinimapRect(regions.minimap, regions.gameArea));
    REQUIRE(resolved.source == smp::MinimapRegionSource::Automatic);
}

TEST_CASE("automatic minimap hit testing includes boundaries and excludes adjacent pixels") {
    const auto [regions, resolved] = automaticRegions({0, 0, 1919, 1079});
    REQUIRE(resolved.source == smp::MinimapRegionSource::Automatic);

    REQUIRE(smp::classifyScreenRegion(regions, {338, 869}) == smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {277, 799}) == smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {519, 1040}) == smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {276, 900}) != smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {520, 900}) != smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {400, 798}) != smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {400, 1041}) != smp::ScreenRegion::Minimap);
}

TEST_CASE("invalid game area makes automatic minimap unavailable") {
    const auto resolved = smp::resolveMinimapRegion(
        {}, smp::MinimapMode::Automatic, std::nullopt);
    REQUIRE(!resolved.rect.valid());
    REQUIRE(!resolved.automaticCandidate.valid());
    REQUIRE(resolved.source == smp::MinimapRegionSource::Unavailable);
}

TEST_CASE("minimap resolver honors mode and safely falls back to automatic") {
    const smp::ScreenRect game{240, 0, 1679, 1079};
    const auto calibrated = smp::normalizeScreenRect(
        {300, 810, 530, 1045}, game);

    const auto automaticWithoutCalibration = smp::resolveMinimapRegion(
        game, smp::MinimapMode::Automatic, std::nullopt);
    const auto automaticWithCalibration = smp::resolveMinimapRegion(
        game, smp::MinimapMode::Automatic, calibrated);
    REQUIRE(automaticWithoutCalibration.source ==
            smp::MinimapRegionSource::Automatic);
    REQUIRE(automaticWithCalibration.source ==
            smp::MinimapRegionSource::Automatic);
    REQUIRE(automaticWithCalibration.rect == automaticWithoutCalibration.rect);

    const auto override = smp::resolveMinimapRegion(
        game, smp::MinimapMode::CalibratedOverride, calibrated);
    REQUIRE(override.source == smp::MinimapRegionSource::CalibratedOverride);
    requireRectNear(override.rect, {300, 810, 530, 1045});

    const auto missingOverride = smp::resolveMinimapRegion(
        game, smp::MinimapMode::CalibratedOverride, std::nullopt);
    const auto invalidOverride = smp::resolveMinimapRegion(
        game, smp::MinimapMode::CalibratedOverride,
        smp::NormalizedScreenRect{});
    REQUIRE(missingOverride.source == smp::MinimapRegionSource::Automatic);
    REQUIRE(invalidOverride.source == smp::MinimapRegionSource::Automatic);
    REQUIRE(missingOverride.rect == automaticWithoutCalibration.rect);
    REQUIRE(invalidOverride.rect == automaticWithoutCalibration.rect);
}

TEST_CASE("overlay model converts desktop regions to stable local geometry") {
    const auto [regions, resolved] = automaticRegions({100, 200, 2019, 1279});
    const auto model = smp::makeScreenRegionOverlayModel(
        regions, resolved, 5, true);
    REQUIRE(model.visible);
    REQUIRE((model.desktopBounds == smp::ScreenRect{100, 200, 2019, 1279}));
    REQUIRE((model.clientRect == smp::ScreenRect{0, 0, 1919, 1079}));
    REQUIRE((model.gameViewportRect == smp::ScreenRect{240, 0, 1679, 1079}));
    REQUIRE((model.minimapRect == smp::ScreenRect{277, 799, 519, 1040}));
    REQUIRE((model.edgeMarginRects[0] == smp::ScreenRect{240, 0, 244, 1079}));
    REQUIRE((model.edgeMarginRects[1] == smp::ScreenRect{1675, 0, 1679, 1079}));
    REQUIRE((model.edgeMarginRects[2] == smp::ScreenRect{240, 0, 1679, 4}));
    REQUIRE((model.edgeMarginRects[3] == smp::ScreenRect{240, 1075, 1679, 1079}));
}

TEST_CASE("moving StarCraft changes overlay origin but preserves local geometry") {
    const auto [originalRegions, originalResolved] =
        automaticRegions({0, 0, 1919, 1079});
    const auto [movedRegions, movedResolved] =
        automaticRegions({100, 200, 2019, 1279});
    const auto original = smp::makeScreenRegionOverlayModel(
        originalRegions, originalResolved, 5, true);
    const auto moved = smp::makeScreenRegionOverlayModel(
        movedRegions, movedResolved, 5, true);
    REQUIRE(original.desktopBounds != moved.desktopBounds);
    REQUIRE(original.clientRect == moved.clientRect);
    REQUIRE(original.gameViewportRect == moved.gameViewportRect);
    REQUIRE(original.minimapRect == moved.minimapRect);
}

TEST_CASE("overlay omits invalid minimap and hides when StarCraft is not foreground") {
    auto regions = smp::calculateStarcraftScreenRegions({0, 0, 1919, 1079});
    smp::ResolvedMinimapRegion unavailable;
    const auto visible = smp::makeScreenRegionOverlayModel(
        regions, unavailable, 5, true);
    REQUIRE(visible.visible);
    REQUIRE(!visible.minimapRect.valid());
    const auto hidden = smp::makeScreenRegionOverlayModel(
        regions, unavailable, 5, false);
    REQUIRE(!hidden.visible);
    REQUIRE(!hidden.desktopBounds.valid());
}

TEST_CASE("native debug overlay window is click through and excluded from app switching") {
    smp::ScreenRegionDebugOverlay overlay;
    REQUIRE(overlay.start());
    const HWND window = FindWindowW(smp::screenRegionOverlayClassName, nullptr);
    REQUIRE(window != nullptr);
    const auto extendedStyle = static_cast<DWORD>(
        GetWindowLongPtrW(window, GWL_EXSTYLE));
    REQUIRE((extendedStyle & WS_EX_LAYERED) != 0);
    REQUIRE((extendedStyle & WS_EX_TRANSPARENT) != 0);
    REQUIRE((extendedStyle & WS_EX_NOACTIVATE) != 0);
    REQUIRE((extendedStyle & WS_EX_TOOLWINDOW) != 0);
    REQUIRE((extendedStyle & WS_EX_TOPMOST) != 0);
    REQUIRE(SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(1, 1)) ==
            HTTRANSPARENT);
    if (overlay.captureExclusionApplied()) {
        DWORD affinity = WDA_NONE;
        REQUIRE(GetWindowDisplayAffinity(window, &affinity));
        REQUIRE(affinity == 0x00000011);
    }
    overlay.stop();
}
