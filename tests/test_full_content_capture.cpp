#include "test_framework.h"

#include "app/full_content_capture.h"

#include <limits>

TEST_CASE("full-content capture dimensions and byte counts are validated") {
    REQUIRE(smp::validCaptureDimensions(1920, 1080));
    REQUIRE(!smp::validCaptureDimensions(0, 1080));
    REQUIRE(!smp::validCaptureDimensions(1920, 0));
    REQUIRE(!smp::validCaptureDimensions(-1, 1080));

    const auto bytes = smp::fullContentCaptureByteSize(1920, 1080);
    REQUIRE(bytes.has_value());
    REQUIRE(*bytes == 1920U * 1080U * 4U);
    REQUIRE(!smp::fullContentCaptureByteSize(0, 1080));
    REQUIRE(!smp::fullContentCaptureByteSize(
        std::numeric_limits<int>::max(),
        std::numeric_limits<int>::max(),
        std::numeric_limits<int>::max()));
}

TEST_CASE("full-content capture plans one tile when content fits") {
    const auto tiles = smp::fullContentCaptureTilePlan(1000, 4096);
    REQUIRE(tiles.size() == 1);
    REQUIRE(tiles[0] == (smp::FullContentCaptureTile{0, 1000}));
}

TEST_CASE("full-content capture uses the texture limit instead of 4096 pixels") {
    const auto shortCapture =
        smp::fullContentCaptureTilePlanForTextureLimit(3000, 16384);
    REQUIRE(shortCapture.size() == 1);
    REQUIRE(shortCapture[0] == (smp::FullContentCaptureTile{0, 3000}));

    const auto captureAroundFiveThousand =
        smp::fullContentCaptureTilePlanForTextureLimit(5000, 16384);
    REQUIRE(captureAroundFiveThousand.size() == 1);
    REQUIRE(captureAroundFiveThousand[0] ==
            (smp::FullContentCaptureTile{0, 5000}));
}

TEST_CASE("full-content capture tiles only at the texture-height limit") {
    const auto tiles =
        smp::fullContentCaptureTilePlanForTextureLimit(20000, 16384);
    REQUIRE(tiles.size() == 2);
    REQUIRE(tiles[0] == (smp::FullContentCaptureTile{0, 16384}));
    REQUIRE(tiles[1] == (smp::FullContentCaptureTile{16384, 3616}));
    REQUIRE(tiles[1].offsetY == tiles[0].offsetY + tiles[0].height);
    REQUIRE(tiles.back().offsetY + tiles.back().height == 20000);
}

TEST_CASE("full-content capture tile plan is contiguous with a short final tile") {
    const auto tiles = smp::fullContentCaptureTilePlan(9000, 4096);
    REQUIRE(tiles.size() == 3);
    REQUIRE(tiles[0] == (smp::FullContentCaptureTile{0, 4096}));
    REQUIRE(tiles[1] == (smp::FullContentCaptureTile{4096, 4096}));
    REQUIRE(tiles[2] == (smp::FullContentCaptureTile{8192, 808}));

    for (std::size_t index = 1; index < tiles.size(); ++index) {
        REQUIRE(tiles[index].offsetY ==
                tiles[index - 1].offsetY + tiles[index - 1].height);
    }
    REQUIRE(tiles.back().offsetY + tiles.back().height == 9000);
}

TEST_CASE("full-content capture rejects invalid tile plans") {
    REQUIRE(smp::fullContentCaptureTilePlan(0, 4096).empty());
    REQUIRE(smp::fullContentCaptureTilePlan(1000, 0).empty());
    REQUIRE(smp::fullContentCaptureTilePlan(-1, 4096).empty());
    REQUIRE(smp::fullContentCaptureTilePlanForTextureLimit(1000, 0).empty());
}
