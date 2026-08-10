#include "test_framework.h"

#include "platform/minimap_viewport_detector.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

class SyntheticImage {
  public:
    SyntheticImage(int width = 120, int height = 100)
        : width_(width), height_(height), pixels_(static_cast<std::size_t>(width * height * 4), 0) {}

    void point(int x, int y, std::uint8_t value = 255) {
        if (x < 0 || y < 0 || x >= width_ || y >= height_)
            return;
        const auto offset = static_cast<std::size_t>((y * width_ + x) * 4);
        pixels_[offset] = value;
        pixels_[offset + 1] = value;
        pixels_[offset + 2] = value;
    }

    void horizontal(int y, int left, int right, std::uint8_t value = 255) {
        for (int x = left; x <= right; ++x)
            point(x, y, value);
    }

    void vertical(int x, int top, int bottom, std::uint8_t value = 255) {
        for (int y = top; y <= bottom; ++y)
            point(x, y, value);
    }

    void rectangle(int left, int top, int right, int bottom, std::uint8_t value = 255) {
        horizontal(top, left, right, value);
        horizontal(top + 1, left, right, value);
        horizontal(bottom - 1, left, right, value);
        horizontal(bottom, left, right, value);
        vertical(left, top, bottom, value);
        vertical(left + 1, top, bottom, value);
        vertical(right - 1, top, bottom, value);
        vertical(right, top, bottom, value);
    }

    [[nodiscard]] smp::BgraImageView view() const {
        return {width_, height_, width_ * 4, pixels_};
    }

  private:
    int width_{};
    int height_{};
    std::vector<std::uint8_t> pixels_;
};

} // namespace

TEST_CASE("black minimap has no viewport outline") {
    const SyntheticImage image;
    REQUIRE(!smp::containsMinimapViewportOutline(image.view()));
}

TEST_CASE("isolated white dots do not form a viewport outline") {
    SyntheticImage image;
    for (int i = 0; i < 30; ++i)
        image.point((i * 17) % 120, (i * 29) % 100);
    REQUIRE(!smp::containsMinimapViewportOutline(image.view()));
}

TEST_CASE("random short white lines do not form a viewport outline") {
    SyntheticImage image;
    image.horizontal(10, 3, 8);
    image.horizontal(42, 60, 66);
    image.vertical(90, 70, 76);
    image.vertical(25, 20, 25);
    REQUIRE(!smp::containsMinimapViewportOutline(image.view()));
}

TEST_CASE("complete viewport rectangle is detected") {
    SyntheticImage image;
    image.rectangle(20, 25, 72, 60);
    REQUIRE(smp::containsMinimapViewportOutline(image.view()));
}

TEST_CASE("viewport rectangle clipped against left boundary is detected") {
    SyntheticImage image;
    image.horizontal(25, 0, 46);
    image.horizontal(26, 0, 46);
    image.horizontal(54, 0, 46);
    image.horizontal(55, 0, 46);
    image.vertical(45, 25, 55);
    image.vertical(46, 25, 55);
    REQUIRE(smp::containsMinimapViewportOutline(image.view()));
}

TEST_CASE("viewport rectangle clipped against right boundary is detected") {
    SyntheticImage image;
    image.horizontal(25, 73, 119);
    image.horizontal(26, 73, 119);
    image.horizontal(54, 73, 119);
    image.horizontal(55, 73, 119);
    image.vertical(73, 25, 55);
    image.vertical(74, 25, 55);
    REQUIRE(smp::containsMinimapViewportOutline(image.view()));
}

TEST_CASE("viewport rectangle clipped against top boundary is detected") {
    SyntheticImage image;
    image.vertical(20, 0, 31);
    image.vertical(21, 0, 31);
    image.vertical(68, 0, 31);
    image.vertical(69, 0, 31);
    image.horizontal(30, 20, 69);
    image.horizontal(31, 20, 69);
    REQUIRE(smp::containsMinimapViewportOutline(image.view()));
}

TEST_CASE("viewport rectangle clipped against bottom boundary is detected") {
    SyntheticImage image;
    image.vertical(20, 68, 99);
    image.vertical(21, 68, 99);
    image.vertical(68, 68, 99);
    image.vertical(69, 68, 99);
    image.horizontal(68, 20, 69);
    image.horizontal(69, 20, 69);
    REQUIRE(smp::containsMinimapViewportOutline(image.view()));
}

TEST_CASE("tiny bright rectangle does not qualify as viewport outline") {
    SyntheticImage image;
    image.rectangle(20, 20, 27, 27);
    REQUIRE(!smp::containsMinimapViewportOutline(image.view()));
}

TEST_CASE("near-white viewport outline is detected") {
    SyntheticImage image;
    image.rectangle(20, 25, 72, 60, 242);
    REQUIRE(smp::containsMinimapViewportOutline(image.view()));
}

TEST_CASE("one plausible horizontal line is not a viewport outline") {
    SyntheticImage image;
    image.horizontal(40, 20, 85);
    image.horizontal(41, 20, 85);
    REQUIRE(!smp::containsMinimapViewportOutline(image.view()));
}

TEST_CASE("viewport start requires two consecutive detected samples") {
    smp::MinimapStartConfirmation confirmation;
    REQUIRE(!confirmation.observe(false).startDetected);
    REQUIRE(!confirmation.observe(true).startDetected);
    REQUIRE(!confirmation.observe(false).startDetected);
    REQUIRE(!confirmation.observe(true).startDetected);
    REQUIRE(confirmation.observe(true).startDetected);
    REQUIRE(!confirmation.observe(true).startDetected);
}

TEST_CASE("post-game detector waits for absence before rearming") {
    smp::MinimapStartConfirmation confirmation;
    REQUIRE(!confirmation.observe(true).startDetected);
    REQUIRE(confirmation.observe(true).startDetected);
    REQUIRE(confirmation.state() == smp::MinimapDetectorState::WaitForAbsence);

    REQUIRE(!confirmation.observe(true).startDetected);
    REQUIRE(!confirmation.observe(true).startDetected);
    REQUIRE(!confirmation.observe(false).rearmed);
    REQUIRE(confirmation.observe(false).rearmed);
    REQUIRE(confirmation.state() == smp::MinimapDetectorState::WaitForAppearance);

    REQUIRE(!confirmation.observe(true).startDetected);
    REQUIRE(confirmation.observe(true).startDetected);
}
