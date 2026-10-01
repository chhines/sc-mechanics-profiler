#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace smp::detail {

struct TimelineAnchor {
    std::int64_t replayFrame{};
    double liveActiveMs{};
};

// Matched sequence order is authoritative. Keep the first usable anchor when
// duplicates or contradictory coordinates occur; never pass those to interpolation.
inline bool appendTimelineAnchor(std::vector<TimelineAnchor>& anchors, TimelineAnchor candidate) {
    if (!std::isfinite(candidate.liveActiveMs) ||
        (!anchors.empty() && (candidate.replayFrame <= anchors.back().replayFrame ||
                              candidate.liveActiveMs <= anchors.back().liveActiveMs)))
        return false;
    anchors.push_back(candidate);
    return true;
}

// Subtract in unsigned arithmetic before conversion to avoid signed overflow,
// while retaining single-frame differences even near the int64 limits.
inline double frameOffset(std::int64_t frame, std::int64_t origin) noexcept {
    if (frame >= origin)
        return static_cast<double>(static_cast<std::uint64_t>(frame) - static_cast<std::uint64_t>(origin));
    return -static_cast<double>(static_cast<std::uint64_t>(origin) - static_cast<std::uint64_t>(frame));
}

inline double frameSlope(const TimelineAnchor& first, const TimelineAnchor& second) noexcept {
    return (second.liveActiveMs - first.liveActiveMs) / frameOffset(second.replayFrame, first.replayFrame);
}

inline double boundedFrameSlope(const TimelineAnchor& first, const TimelineAnchor& second) noexcept {
    return std::clamp(frameSlope(first, second), 5.0, 80.0);
}

// Anchors must have been accepted by appendTimelineAnchor. Interior slopes are
// measurements in foreground-active time, not a simulation/wall-clock rate.
// Out-of-range rates are diagnosed by the caller, not clamped away from their
// endpoints. Positive interpolation and boundary-attached extrapolation define
// one continuous, nondecreasing transformation, independent of query order.
inline double replayFrameToActiveMs(std::int64_t frame, const std::vector<TimelineAnchor>& anchors) noexcept {
    if (anchors.empty())
        return static_cast<double>(frame) * 42.0;
    if (anchors.size() == 1)
        return anchors.front().liveActiveMs + frameOffset(frame, anchors.front().replayFrame) * 42.0;
    if (frame <= anchors.front().replayFrame) {
        return anchors.front().liveActiveMs +
               frameOffset(frame, anchors.front().replayFrame) * boundedFrameSlope(anchors[0], anchors[1]);
    }
    if (frame >= anchors.back().replayFrame) {
        return anchors.back().liveActiveMs + frameOffset(frame, anchors.back().replayFrame) *
                                                 boundedFrameSlope(anchors[anchors.size() - 2], anchors.back());
    }
    const auto upper =
        std::upper_bound(anchors.begin(), anchors.end(), frame,
                         [](std::int64_t value, const TimelineAnchor& anchor) { return value < anchor.replayFrame; });
    const auto& second = *upper;
    const auto& first = *(upper - 1);
    const double fraction = frameOffset(frame, first.replayFrame) / frameOffset(second.replayFrame, first.replayFrame);
    // lerp guarantees monotonic interpolation and exact endpoints, including
    // when rounding a value extremely close to the second anchor.
    return std::lerp(first.liveActiveMs, second.liveActiveMs, fraction);
}

} // namespace smp::detail
