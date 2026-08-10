#include "platform/minimap_viewport_detector.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace smp {
namespace {

struct BrightRun {
    int fixed{};
    int start{};
    int end{};
};

bool isBright(const BgraImageView& image, int x, int y) noexcept {
    const std::size_t offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(image.stride) +
                               static_cast<std::size_t>(x) * 4U;
    const auto blue = image.pixels[offset];
    const auto green = image.pixels[offset + 1U];
    const auto red = image.pixels[offset + 2U];
    return red >= 240U && green >= 240U && blue >= 240U;
}

std::vector<BrightRun> horizontalRuns(const BgraImageView& image, int minimumLength) {
    std::vector<BrightRun> runs;
    for (int y = 0; y < image.height; ++y) {
        int x = 0;
        while (x < image.width) {
            while (x < image.width && !isBright(image, x, y))
                ++x;
            if (x >= image.width)
                break;
            const int start = x;
            int lastBright = x;
            int gap = 0;
            for (++x; x < image.width; ++x) {
                if (isBright(image, x, y)) {
                    lastBright = x;
                    gap = 0;
                } else if (++gap > 1) {
                    break;
                }
            }
            if (lastBright - start + 1 >= minimumLength)
                runs.push_back({y, start, lastBright});
        }
    }
    return runs;
}

std::vector<BrightRun> verticalRuns(const BgraImageView& image, int minimumLength) {
    std::vector<BrightRun> runs;
    for (int x = 0; x < image.width; ++x) {
        int y = 0;
        while (y < image.height) {
            while (y < image.height && !isBright(image, x, y))
                ++y;
            if (y >= image.height)
                break;
            const int start = y;
            int lastBright = y;
            int gap = 0;
            for (++y; y < image.height; ++y) {
                if (isBright(image, x, y)) {
                    lastBright = y;
                    gap = 0;
                } else if (++gap > 1) {
                    break;
                }
            }
            if (lastBright - start + 1 >= minimumLength)
                runs.push_back({x, start, lastBright});
        }
    }
    return runs;
}

bool hasSupportingRun(const std::vector<BrightRun>& runs, int fixed, int start, int end, int tolerance) {
    return std::any_of(runs.begin(), runs.end(), [&](const BrightRun& run) {
        return std::abs(run.fixed - fixed) <= tolerance && run.start <= start + tolerance &&
               run.end >= end - tolerance;
    });
}

bool rectangleFromHorizontalPair(const std::vector<BrightRun>& horizontal,
                                 const std::vector<BrightRun>& vertical, int width,
                                 int minimumWidth, int minimumHeight, int tolerance) {
    const int boundaryTolerance = tolerance + 1;
    for (std::size_t firstIndex = 0; firstIndex < horizontal.size(); ++firstIndex) {
        const auto& first = horizontal[firstIndex];
        for (std::size_t secondIndex = firstIndex + 1; secondIndex < horizontal.size(); ++secondIndex) {
            const auto& second = horizontal[secondIndex];
            const int top = std::min(first.fixed, second.fixed);
            const int bottom = std::max(first.fixed, second.fixed);
            if (bottom - top + 1 < minimumHeight)
                continue;
            if (std::abs(first.start - second.start) > tolerance ||
                std::abs(first.end - second.end) > tolerance)
                continue;
            const int left = (first.start + second.start) / 2;
            const int right = (first.end + second.end) / 2;
            if (right - left + 1 < minimumWidth)
                continue;

            const bool leftSide = hasSupportingRun(vertical, left, top, bottom, tolerance);
            const bool rightSide = hasSupportingRun(vertical, right, top, bottom, tolerance);
            if ((leftSide && rightSide) || (!leftSide && rightSide && left <= boundaryTolerance) ||
                (leftSide && !rightSide && right >= width - 1 - boundaryTolerance))
                return true;
        }
    }
    return false;
}

bool rectangleFromVerticalPair(const std::vector<BrightRun>& horizontal,
                               const std::vector<BrightRun>& vertical, int height,
                               int minimumWidth, int minimumHeight, int tolerance) {
    const int boundaryTolerance = tolerance + 1;
    for (std::size_t firstIndex = 0; firstIndex < vertical.size(); ++firstIndex) {
        const auto& first = vertical[firstIndex];
        for (std::size_t secondIndex = firstIndex + 1; secondIndex < vertical.size(); ++secondIndex) {
            const auto& second = vertical[secondIndex];
            const int left = std::min(first.fixed, second.fixed);
            const int right = std::max(first.fixed, second.fixed);
            if (right - left + 1 < minimumWidth)
                continue;
            if (std::abs(first.start - second.start) > tolerance ||
                std::abs(first.end - second.end) > tolerance)
                continue;
            const int top = (first.start + second.start) / 2;
            const int bottom = (first.end + second.end) / 2;
            if (bottom - top + 1 < minimumHeight)
                continue;

            const bool topSide = hasSupportingRun(horizontal, top, left, right, tolerance);
            const bool bottomSide = hasSupportingRun(horizontal, bottom, left, right, tolerance);
            if ((topSide && bottomSide) || (!topSide && bottomSide && top <= boundaryTolerance) ||
                (topSide && !bottomSide && bottom >= height - 1 - boundaryTolerance))
                return true;
        }
    }
    return false;
}

} // namespace

bool containsMinimapViewportOutline(const BgraImageView& image) noexcept {
    if (!image.valid())
        return false;
    try {
        const int minimumWidth = std::max(12, image.width / 32);
        const int minimumHeight = std::max(10, image.height / 36);
        const int tolerance = std::max(2, std::min(image.width, image.height) / 128);
        const auto horizontal = horizontalRuns(image, minimumWidth);
        const auto vertical = verticalRuns(image, minimumHeight);
        if (horizontal.size() < 1U || vertical.size() < 1U)
            return false;
        return rectangleFromHorizontalPair(horizontal, vertical, image.width, minimumWidth, minimumHeight,
                                           tolerance) ||
               rectangleFromVerticalPair(horizontal, vertical, image.height, minimumWidth, minimumHeight,
                                         tolerance);
    } catch (...) {
        return false;
    }
}

MinimapConfirmationResult MinimapStartConfirmation::observe(bool viewportPresent) noexcept {
    if (state_ == MinimapDetectorState::WaitForAppearance) {
        consecutiveAbsent_ = 0;
        if (!viewportPresent) {
            consecutivePresent_ = 0;
            return {};
        }
        if (++consecutivePresent_ < 2)
            return {};
        consecutivePresent_ = 0;
        state_ = MinimapDetectorState::WaitForAbsence;
        return {.startDetected = true};
    }

    consecutivePresent_ = 0;
    if (viewportPresent) {
        consecutiveAbsent_ = 0;
        return {};
    }
    if (++consecutiveAbsent_ < 2)
        return {};
    consecutiveAbsent_ = 0;
    state_ = MinimapDetectorState::WaitForAppearance;
    return {.rearmed = true};
}

} // namespace smp
