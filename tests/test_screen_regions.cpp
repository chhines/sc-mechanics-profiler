#include "test_framework.h"

#include "platform/screen_regions.h"

#include <cmath>

namespace {

void requireRectNear(const smp::ScreenRect& actual, const smp::ScreenRect& expected, int tolerance = 1) {
    REQUIRE(std::abs(actual.left - expected.left) <= tolerance);
    REQUIRE(std::abs(actual.top - expected.top) <= tolerance);
    REQUIRE(std::abs(actual.right - expected.right) <= tolerance);
    REQUIRE(std::abs(actual.bottom - expected.bottom) <= tolerance);
}

} // namespace

TEST_CASE("1920 by 1080 client deterministically derives the centered 4 by 3 game area") {
    const smp::ScreenRect client{0, 0, 1919, 1079};
    const auto game = smp::derive4x3GameArea(client);
    REQUIRE((game == smp::ScreenRect{240, 0, 1679, 1079}));
    REQUIRE(game.width() == 1440);
    REQUIRE(game.height() == 1080);
}

TEST_CASE("identical client geometry reconstructs identical minimap geometry after focus regain") {
    const smp::ScreenRect client{0, 0, 1919, 1079};
    const smp::ScreenRect calibrated{277, 799, 519, 1040};
    const auto game = smp::derive4x3GameArea(client);
    const auto normalized = smp::normalizeScreenRect(calibrated, game);
    const auto first = smp::withCalibratedMinimap(smp::calculateStarcraftScreenRegions(client), normalized);
    const auto regained = smp::withCalibratedMinimap(smp::calculateStarcraftScreenRegions(client), normalized);
    REQUIRE(first.clientArea == regained.clientArea);
    REQUIRE(first.gameArea == regained.gameArea);
    REQUIRE(first.minimap == regained.minimap);
}

TEST_CASE("normalized minimap calibration round trips within inclusive-coordinate rounding tolerance") {
    const smp::ScreenRect game{240, 0, 1679, 1079};
    const smp::ScreenRect minimap{277, 799, 519, 1040};
    const auto normalized = smp::normalizeScreenRect(minimap, game);
    requireRectNear(smp::reconstructScreenRect(normalized, game), minimap);
}

TEST_CASE("moving a same-sized client moves the normalized minimap by the same desktop offset") {
    const smp::ScreenRect originalClient{0, 0, 1919, 1079};
    const smp::ScreenRect originalMinimap{277, 799, 519, 1040};
    const auto normalized = smp::normalizeScreenRect(originalMinimap, smp::derive4x3GameArea(originalClient));

    const smp::ScreenRect movedClient{100, 200, 2019, 1279};
    const auto moved = smp::withCalibratedMinimap(smp::calculateStarcraftScreenRegions(movedClient), normalized);
    requireRectNear(moved.minimap, {377, 999, 619, 1240});
}

TEST_CASE("normalized minimap position is preserved in a resized proportional game area") {
    const smp::ScreenRect originalGame{240, 0, 1679, 1079};
    const smp::ScreenRect originalMinimap{277, 799, 519, 1040};
    const auto normalized = smp::normalizeScreenRect(originalMinimap, originalGame);
    const smp::ScreenRect resizedGame{0, 0, 799, 599};
    const auto resizedMinimap = smp::reconstructScreenRect(normalized, resizedGame);
    const auto resizedNormalized = smp::normalizeScreenRect(resizedMinimap, resizedGame);
    REQUIRE_NEAR(resizedNormalized.left, normalized.left, 0.002);
    REQUIRE_NEAR(resizedNormalized.top, normalized.top, 0.002);
    REQUIRE_NEAR(resizedNormalized.right, normalized.right, 0.002);
    REQUIRE_NEAR(resizedNormalized.bottom, normalized.bottom, 0.002);
}

TEST_CASE("calibrated minimap hit testing includes its boundaries and excludes adjacent pixels") {
    smp::ScreenRegions regions;
    regions.clientArea = {0, 0, 1919, 1079};
    regions.gameArea = {240, 0, 1679, 1079};
    regions.viewport = regions.gameArea;
    regions.minimap = {277, 799, 519, 1040};

    REQUIRE(smp::classifyScreenRegion(regions, {338, 869}) == smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {277, 799}) == smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {519, 1040}) == smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {276, 900}) != smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {520, 900}) != smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {400, 798}) != smp::ScreenRegion::Minimap);
    REQUIRE(smp::classifyScreenRegion(regions, {400, 1041}) != smp::ScreenRegion::Minimap);
}
