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
    auto regions = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::OriginalAspect);
    auto resolved = smp::resolveMinimapRegion(
        regions, smp::MinimapMode::Automatic, smp::MinimapMode::Automatic,
        std::nullopt, std::nullopt);
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
    REQUIRE((regions.minimap == smp::ScreenRect{254, 783, 541, 1070}));
    REQUIRE(regions.minimap.width() == 288);
    REQUIRE(regions.minimap.height() == 288);
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

TEST_CASE("same client selects distinct original-aspect and widescreen profiles") {
    const smp::ScreenRect client{0, 0, 1919, 1079};
    const auto original = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::OriginalAspect);
    const auto widescreen = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::Widescreen);

    REQUIRE(original.displayMode == smp::StarcraftDisplayMode::OriginalAspect);
    REQUIRE((original.gameArea == smp::ScreenRect{240, 0, 1679, 1079}));
    REQUIRE(original.viewport == original.gameArea);
    REQUIRE(widescreen.displayMode == smp::StarcraftDisplayMode::Widescreen);
    REQUIRE(widescreen.gameArea == client);
    REQUIRE(widescreen.viewport == client);
    REQUIRE(original.gameArea != widescreen.gameArea);

    const auto widescreenMinimap = smp::resolveMinimapRegion(
        widescreen, smp::MinimapMode::Automatic, smp::MinimapMode::Automatic,
        std::nullopt, std::nullopt);
    REQUIRE(widescreenMinimap.source == smp::MinimapRegionSource::Automatic);
    REQUIRE((widescreenMinimap.rect == smp::ScreenRect{13, 783, 300, 1070}));
    REQUIRE(widescreenMinimap.rect.width() == 288);
    REQUIRE(widescreenMinimap.rect.height() == 288);
}

TEST_CASE("original-aspect and widescreen minimap profiles remain isolated") {
    const smp::ScreenRect client{0, 0, 1919, 1079};
    const auto original = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::OriginalAspect);
    const auto widescreen = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::Widescreen);
    const auto originalCalibration = smp::normalizeScreenRect(
        {300, 810, 530, 1045}, original.gameArea);
    const smp::ScreenRect measuredWidescreenMinimap{20, 780, 310, 1065};
    const auto widescreenCalibration = smp::normalizeScreenRect(
        measuredWidescreenMinimap, widescreen.gameArea);

    const auto originalAutomatic = smp::resolveMinimapRegion(
        original, smp::MinimapMode::Automatic, smp::MinimapMode::Automatic,
        std::nullopt,
        std::nullopt);
    REQUIRE(originalAutomatic.source == smp::MinimapRegionSource::Automatic);
    REQUIRE((originalAutomatic.rect == smp::ScreenRect{254, 783, 541, 1070}));

    const auto widescreenAutomatic = smp::resolveMinimapRegion(
        widescreen, smp::MinimapMode::Automatic, smp::MinimapMode::Automatic,
        std::nullopt,
        widescreenCalibration);
    REQUIRE(widescreenAutomatic.source ==
            smp::MinimapRegionSource::Automatic);
    REQUIRE((widescreenAutomatic.rect ==
             smp::ScreenRect{13, 783, 300, 1070}));

    const auto calibratedWidescreen = smp::resolveMinimapRegion(
        widescreen, smp::MinimapMode::Automatic,
        smp::MinimapMode::CalibratedOverride, std::nullopt,
        widescreenCalibration);
    REQUIRE(calibratedWidescreen.source ==
            smp::MinimapRegionSource::CalibratedOverride);
    requireRectNear(calibratedWidescreen.rect, measuredWidescreenMinimap);
    REQUIRE((calibratedWidescreen.automaticCandidate ==
             smp::ScreenRect{13, 783, 300, 1070}));

    const auto missingWidescreenOverride = smp::resolveMinimapRegion(
        widescreen, smp::MinimapMode::Automatic,
        smp::MinimapMode::CalibratedOverride, std::nullopt,
        std::nullopt);
    const auto invalidWidescreenOverride = smp::resolveMinimapRegion(
        widescreen, smp::MinimapMode::Automatic,
        smp::MinimapMode::CalibratedOverride, std::nullopt,
        smp::NormalizedScreenRect{});
    REQUIRE(missingWidescreenOverride.source ==
            smp::MinimapRegionSource::Automatic);
    REQUIRE(invalidWidescreenOverride.source ==
            smp::MinimapRegionSource::Automatic);
    REQUIRE(missingWidescreenOverride.rect == widescreenAutomatic.rect);
    REQUIRE(invalidWidescreenOverride.rect == widescreenAutomatic.rect);

    const auto originalCalibrationInWidescreen = smp::resolveMinimapRegion(
        widescreen, smp::MinimapMode::CalibratedOverride,
        smp::MinimapMode::CalibratedOverride, originalCalibration,
        std::nullopt);
    REQUIRE(originalCalibrationInWidescreen.source ==
            smp::MinimapRegionSource::Automatic);
    REQUIRE(originalCalibrationInWidescreen.rect == widescreenAutomatic.rect);

    const auto widescreenCalibrationInOriginal = smp::resolveMinimapRegion(
        original, smp::MinimapMode::CalibratedOverride,
        smp::MinimapMode::CalibratedOverride, std::nullopt,
        widescreenCalibration);
    REQUIRE(widescreenCalibrationInOriginal.source ==
            smp::MinimapRegionSource::Automatic);
    REQUIRE(widescreenCalibrationInOriginal.rect == originalAutomatic.rect);
}

TEST_CASE("minimap modes resolve independently for all preference combinations") {
    const smp::ScreenRect client{0, 0, 1919, 1079};
    const auto original = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::OriginalAspect);
    const auto widescreen = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::Widescreen);
    const auto originalCalibration = smp::normalizeScreenRect(
        {300, 810, 530, 1045}, original.gameArea);
    const auto widescreenCalibration = smp::normalizeScreenRect(
        {20, 780, 310, 1065}, widescreen.gameArea);

    const auto requireModes = [&](smp::MinimapMode originalMode,
                                  smp::MinimapMode widescreenMode,
                                  smp::MinimapRegionSource originalSource,
                                  smp::MinimapRegionSource widescreenSource) {
        const auto originalResult = smp::resolveMinimapRegion(
            original, originalMode, widescreenMode, originalCalibration,
            widescreenCalibration);
        const auto widescreenResult = smp::resolveMinimapRegion(
            widescreen, originalMode, widescreenMode, originalCalibration,
            widescreenCalibration);
        REQUIRE(originalResult.source == originalSource);
        REQUIRE(widescreenResult.source == widescreenSource);
        if (originalSource == smp::MinimapRegionSource::Automatic)
            REQUIRE((originalResult.rect == smp::ScreenRect{254, 783, 541, 1070}));
        else
            requireRectNear(originalResult.rect, {300, 810, 530, 1045});
        if (widescreenSource == smp::MinimapRegionSource::Automatic)
            REQUIRE((widescreenResult.rect == smp::ScreenRect{13, 783, 300, 1070}));
        else
            requireRectNear(widescreenResult.rect, {20, 780, 310, 1065});
    };

    requireModes(smp::MinimapMode::Automatic, smp::MinimapMode::Automatic,
                 smp::MinimapRegionSource::Automatic,
                 smp::MinimapRegionSource::Automatic);
    requireModes(smp::MinimapMode::CalibratedOverride,
                 smp::MinimapMode::Automatic,
                 smp::MinimapRegionSource::CalibratedOverride,
                 smp::MinimapRegionSource::Automatic);
    requireModes(smp::MinimapMode::Automatic,
                 smp::MinimapMode::CalibratedOverride,
                 smp::MinimapRegionSource::Automatic,
                 smp::MinimapRegionSource::CalibratedOverride);
    requireModes(smp::MinimapMode::CalibratedOverride,
                 smp::MinimapMode::CalibratedOverride,
                 smp::MinimapRegionSource::CalibratedOverride,
                 smp::MinimapRegionSource::CalibratedOverride);
}

TEST_CASE("unknown display mode keeps the original-aspect fallback explicit") {
    const auto unknown = smp::calculateStarcraftScreenRegions(
        {0, 0, 1919, 1079}, smp::StarcraftDisplayMode::Unknown);
    REQUIRE(unknown.displayMode == smp::StarcraftDisplayMode::Unknown);
    REQUIRE((unknown.gameArea == smp::ScreenRect{240, 0, 1679, 1079}));
    REQUIRE(unknown.viewport == unknown.gameArea);
    const auto minimap = smp::resolveMinimapRegion(
        unknown, smp::MinimapMode::Automatic, smp::MinimapMode::Automatic,
        std::nullopt, std::nullopt);
    REQUIRE(minimap.source == smp::MinimapRegionSource::Automatic);
}

TEST_CASE("normalized minimap calibration round trips within inclusive-coordinate rounding tolerance") {
    const smp::ScreenRect game{240, 0, 1679, 1079};
    const smp::ScreenRect minimap{254, 783, 541, 1070};
    const auto normalized = smp::normalizeScreenRect(minimap, game);
    requireRectNear(smp::reconstructScreenRect(normalized, game), minimap);
}

TEST_CASE("widescreen minimap calibration normalizes against the full client") {
    const smp::ScreenRect client{0, 0, 1919, 1079};
    const auto regions = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::Widescreen);
    const smp::ScreenRect minimap{20, 780, 310, 1065};
    const auto normalized = smp::normalizeScreenRect(minimap, regions.gameArea);
    REQUIRE(regions.gameArea == client);
    requireRectNear(smp::reconstructScreenRect(normalized, regions.gameArea),
                    minimap);
}

TEST_CASE("automatic minimap normalization matches the measured calibration") {
    const auto normalized = smp::originalAspectAutomaticMinimapNormalizedRect();
    REQUIRE(normalized.left == 14.0 / 1440.0);
    REQUIRE(normalized.top == 783.0 / 1080.0);
    REQUIRE(normalized.right == 301.0 / 1440.0);
    REQUIRE(normalized.bottom == 1070.0 / 1080.0);
}

TEST_CASE("widescreen automatic minimap normalization matches the measured calibration") {
    const auto normalized = smp::widescreenAutomaticMinimapNormalizedRect();
    REQUIRE(normalized.left == 13.0 / 1920.0);
    REQUIRE(normalized.top == 783.0 / 1080.0);
    REQUIRE(normalized.right == 300.0 / 1920.0);
    REQUIRE(normalized.bottom == 1070.0 / 1080.0);
}

TEST_CASE("moving a same-sized client moves the automatic minimap by the same desktop offset") {
    const smp::ScreenRect movedClient{100, 200, 2019, 1279};
    const auto [moved, resolved] = automaticRegions(movedClient);
    REQUIRE((moved.minimap == smp::ScreenRect{354, 983, 641, 1270}));
    REQUIRE(resolved.source == smp::MinimapRegionSource::Automatic);
}

TEST_CASE("automatic minimap position is preserved in a resized proportional game area") {
    const auto normalized = smp::originalAspectAutomaticMinimapNormalizedRect();
    const smp::ScreenRect resizedGame{0, 0, 799, 599};
    const auto resizedMinimap =
        smp::deriveOriginalAspectAutomaticMinimapRect(resizedGame);
    const auto resizedNormalized = smp::normalizeScreenRect(resizedMinimap, resizedGame);
    REQUIRE_NEAR(resizedNormalized.left, normalized.left, 0.002);
    REQUIRE_NEAR(resizedNormalized.top, normalized.top, 0.002);
    REQUIRE_NEAR(resizedNormalized.right, normalized.right, 0.002);
    REQUIRE_NEAR(resizedNormalized.bottom, normalized.bottom, 0.002);
}

TEST_CASE("moving a widescreen client moves its automatic minimap by the same desktop offset") {
    auto regions = smp::calculateStarcraftScreenRegions(
        {100, 200, 2019, 1279}, smp::StarcraftDisplayMode::Widescreen);
    const auto resolved = smp::resolveMinimapRegion(
        regions, smp::MinimapMode::Automatic, smp::MinimapMode::Automatic,
        std::nullopt, std::nullopt);
    REQUIRE((resolved.rect == smp::ScreenRect{113, 983, 400, 1270}));
    REQUIRE(resolved.source == smp::MinimapRegionSource::Automatic);
}

TEST_CASE("widescreen automatic minimap preserves normalized position when resized") {
    const auto normalized = smp::widescreenAutomaticMinimapNormalizedRect();
    const smp::ScreenRect resizedGame{0, 0, 1279, 719};
    const auto resizedMinimap =
        smp::deriveWidescreenAutomaticMinimapRect(resizedGame);
    const auto resizedNormalized =
        smp::normalizeScreenRect(resizedMinimap, resizedGame);
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

    REQUIRE(smp::classifyScreenRegion(regions, {397, 926}) == smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {254, 783}) == smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {541, 1070}) == smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {253, 926}) != smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {542, 926}) != smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {397, 782}) != smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {397, 1071}) != smp::ScreenRegion::Minimap);
}

TEST_CASE("widescreen automatic minimap hit testing uses the measured boundaries") {
    auto regions = smp::calculateStarcraftScreenRegions(
        {0, 0, 1919, 1079}, smp::StarcraftDisplayMode::Widescreen);
    const auto resolved = smp::resolveMinimapRegion(
        regions, smp::MinimapMode::Automatic, smp::MinimapMode::Automatic,
        std::nullopt, std::nullopt);
    regions.minimap = resolved.rect;
    REQUIRE((regions.minimap == smp::ScreenRect{13, 783, 300, 1070}));
    REQUIRE(smp::classifyScreenRegion(regions, {13, 783}) ==
            smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {300, 1070}) ==
            smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {156, 926}) ==
            smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {294, 800}) ==
            smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {12, 926}) !=
            smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {301, 926}) !=
            smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {156, 782}) !=
            smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {156, 1071}) !=
            smp::ScreenRegion::Minimap);
}

TEST_CASE("invalid game area makes automatic minimap unavailable") {
    smp::ScreenRegions regions;
    regions.displayMode = smp::StarcraftDisplayMode::OriginalAspect;
    const auto resolved = smp::resolveMinimapRegion(
        regions, smp::MinimapMode::Automatic, smp::MinimapMode::Automatic,
        std::nullopt, std::nullopt);
    REQUIRE(!resolved.rect.valid());
    REQUIRE(!resolved.automaticCandidate.valid());
    REQUIRE(resolved.source == smp::MinimapRegionSource::Unavailable);
}

TEST_CASE("minimap resolver honors mode and safely falls back to automatic") {
    const smp::ScreenRect game{240, 0, 1679, 1079};
    smp::ScreenRegions regions;
    regions.gameArea = game;
    regions.displayMode = smp::StarcraftDisplayMode::OriginalAspect;
    const auto calibrated = smp::normalizeScreenRect(
        {300, 810, 530, 1045}, game);

    const auto automaticWithoutCalibration = smp::resolveMinimapRegion(
        regions, smp::MinimapMode::Automatic, smp::MinimapMode::Automatic,
        std::nullopt, std::nullopt);
    const auto automaticWithCalibration = smp::resolveMinimapRegion(
        regions, smp::MinimapMode::Automatic, smp::MinimapMode::Automatic,
        calibrated, std::nullopt);
    REQUIRE(automaticWithoutCalibration.source ==
            smp::MinimapRegionSource::Automatic);
    REQUIRE(automaticWithCalibration.source ==
            smp::MinimapRegionSource::Automatic);
    REQUIRE(automaticWithCalibration.rect == automaticWithoutCalibration.rect);

    const auto override = smp::resolveMinimapRegion(
        regions, smp::MinimapMode::CalibratedOverride,
        smp::MinimapMode::Automatic, calibrated, std::nullopt);
    REQUIRE(override.source == smp::MinimapRegionSource::CalibratedOverride);
    requireRectNear(override.rect, {300, 810, 530, 1045});

    const auto missingOverride = smp::resolveMinimapRegion(
        regions, smp::MinimapMode::CalibratedOverride,
        smp::MinimapMode::Automatic, std::nullopt, std::nullopt);
    const auto invalidOverride = smp::resolveMinimapRegion(
        regions, smp::MinimapMode::CalibratedOverride,
        smp::MinimapMode::Automatic, smp::NormalizedScreenRect{},
        std::nullopt);
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
    REQUIRE((model.minimapRect == smp::ScreenRect{254, 783, 541, 1070}));
    REQUIRE(model.displayMode == smp::StarcraftDisplayMode::OriginalAspect);
    REQUIRE((model.edgeMarginRects[0] == smp::ScreenRect{240, 0, 244, 1079}));
    REQUIRE((model.edgeMarginRects[1] == smp::ScreenRect{1675, 0, 1679, 1079}));
    REQUIRE((model.edgeMarginRects[2] == smp::ScreenRect{240, 0, 1679, 4}));
    REQUIRE((model.edgeMarginRects[3] == smp::ScreenRect{240, 1075, 1679, 1079}));
}

TEST_CASE("automatic widescreen profile is visible in the debug overlay model") {
    auto regions = smp::calculateStarcraftScreenRegions(
        {0, 0, 1919, 1079}, smp::StarcraftDisplayMode::Widescreen);
    const auto resolved = smp::resolveMinimapRegion(
        regions, smp::MinimapMode::Automatic, smp::MinimapMode::Automatic,
        std::nullopt, std::nullopt);
    regions.minimap = resolved.rect;
    const auto model = smp::makeScreenRegionOverlayModel(
        regions, resolved, 5, true);
    REQUIRE(model.visible);
    REQUIRE(model.displayMode == smp::StarcraftDisplayMode::Widescreen);
    REQUIRE(model.clientRect.valid());
    REQUIRE((model.gameViewportRect == smp::ScreenRect{0, 0, 1919, 1079}));
    REQUIRE((model.minimapRect == smp::ScreenRect{13, 783, 300, 1070}));
    REQUIRE(model.minimapSource == smp::MinimapRegionSource::Automatic);
}

TEST_CASE("calibrated widescreen overlay uses the full client and widescreen minimap") {
    auto regions = smp::calculateStarcraftScreenRegions(
        {100, 200, 2019, 1279}, smp::StarcraftDisplayMode::Widescreen);
    const smp::ScreenRect minimap{120, 980, 410, 1265};
    const auto calibration = smp::normalizeScreenRect(minimap, regions.gameArea);
    const auto resolved = smp::resolveMinimapRegion(
        regions, smp::MinimapMode::Automatic,
        smp::MinimapMode::CalibratedOverride, std::nullopt, calibration);
    regions.minimap = resolved.rect;
    const auto model = smp::makeScreenRegionOverlayModel(
        regions, resolved, 5, true);
    REQUIRE((model.gameViewportRect == smp::ScreenRect{0, 0, 1919, 1079}));
    REQUIRE((model.minimapRect == smp::ScreenRect{20, 780, 310, 1065}));
    REQUIRE(model.minimapSource ==
            smp::MinimapRegionSource::CalibratedOverride);
    REQUIRE((model.automaticCandidateRect ==
             smp::ScreenRect{13, 783, 300, 1070}));
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
    auto regions = smp::calculateStarcraftScreenRegions(
        {0, 0, 1919, 1079}, smp::StarcraftDisplayMode::OriginalAspect);
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

TEST_CASE("native debug overlay window is click through capturable and excluded from app switching") {
    smp::ScreenRegionDebugOverlay overlay;
    REQUIRE(overlay.start(smp::OverlayCapturePolicy::Capturable));
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
    REQUIRE(!overlay.captureExclusionApplied());
    DWORD affinity = WDA_NONE;
    if (GetWindowDisplayAffinity(window, &affinity))
        REQUIRE(affinity == WDA_NONE);
    overlay.stop();
}

TEST_CASE("native overlay capture exclusion remains available as an explicit policy") {
    smp::ScreenRegionDebugOverlay overlay;
    REQUIRE(overlay.start(smp::OverlayCapturePolicy::ExcludeFromCapture));
    const HWND window = FindWindowW(smp::screenRegionOverlayClassName, nullptr);
    REQUIRE(window != nullptr);
    if (overlay.captureExclusionApplied()) {
        DWORD affinity = WDA_NONE;
        REQUIRE(GetWindowDisplayAffinity(window, &affinity));
        REQUIRE(affinity == 0x00000011);
    }
    overlay.stop();
}
