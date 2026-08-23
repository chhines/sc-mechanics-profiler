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
}
