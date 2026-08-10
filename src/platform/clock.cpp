#include "platform/clock.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <windows.h>

namespace smp {

std::optional<std::int64_t> qpcDeltaNanoseconds(std::uint64_t anchorTicks,
                                                std::uint64_t timestampTicks,
                                                std::uint64_t frequency) noexcept {
    if (frequency == 0)
        return std::nullopt;
    const bool negative = timestampTicks < anchorTicks;
    const std::uint64_t magnitudeTicks =
        negative ? anchorTicks - timestampTicks : timestampTicks - anchorTicks;
    const std::uint64_t wholeSeconds = magnitudeTicks / frequency;
    const std::uint64_t remainderTicks = magnitudeTicks % frequency;
    constexpr std::uint64_t nanosecondsPerSecond = 1'000'000'000ULL;
    if (wholeSeconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
                           nanosecondsPerSecond)
        return std::nullopt;
    const auto fractionalNanoseconds = static_cast<std::uint64_t>(std::llround(
        static_cast<long double>(remainderTicks) * static_cast<long double>(nanosecondsPerSecond) /
        static_cast<long double>(frequency)));
    const std::uint64_t magnitudeNanoseconds = wholeSeconds * nanosecondsPerSecond + fractionalNanoseconds;
    if (magnitudeNanoseconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    const auto signedMagnitude = static_cast<std::int64_t>(magnitudeNanoseconds);
    return negative ? -signedMagnitude : signedMagnitude;
}

QpcClock::QpcClock() {
    LARGE_INTEGER value{};
    if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0) {
        throw std::runtime_error("QueryPerformanceFrequency failed");
    }
    frequency_ = static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t QpcClock::now() const noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

QpcWallClockAnchor QpcClock::wallClockAnchorAt(std::uint64_t qpcTicks) const noexcept {
    constexpr std::uint64_t fileTimeUnixEpoch100ns = 116'444'736'000'000'000ULL;

    LARGE_INTEGER before{};
    LARGE_INTEGER after{};
    FILETIME wallTime{};
    QueryPerformanceCounter(&before);
    GetSystemTimePreciseAsFileTime(&wallTime);
    QueryPerformanceCounter(&after);

    ULARGE_INTEGER fileTime{};
    fileTime.LowPart = wallTime.dwLowDateTime;
    fileTime.HighPart = wallTime.dwHighDateTime;
    const auto unix100ns = fileTime.QuadPart >= fileTimeUnixEpoch100ns
                               ? fileTime.QuadPart - fileTimeUnixEpoch100ns
                               : 0;
    const std::uint64_t midpointTicks = static_cast<std::uint64_t>(before.QuadPart) +
                                        (static_cast<std::uint64_t>(after.QuadPart - before.QuadPart) / 2);
    const auto deltaNanoseconds = qpcDeltaNanoseconds(midpointTicks, qpcTicks, frequency_).value_or(0);
    const std::uint64_t sampledUnixNanoseconds = unix100ns * 100;
    std::int64_t unixNanoseconds{};
    if (sampledUnixNanoseconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        unixNanoseconds = std::numeric_limits<std::int64_t>::max();
    } else {
        const auto sampled = static_cast<std::int64_t>(sampledUnixNanoseconds);
        if (deltaNanoseconds > 0 && sampled > std::numeric_limits<std::int64_t>::max() - deltaNanoseconds)
            unixNanoseconds = std::numeric_limits<std::int64_t>::max();
        else if (deltaNanoseconds < 0 && sampled < std::numeric_limits<std::int64_t>::min() - deltaNanoseconds)
            unixNanoseconds = std::numeric_limits<std::int64_t>::min();
        else
            unixNanoseconds = sampled + deltaNanoseconds;
    }
    return {qpcTicks, unixNanoseconds};
}

} // namespace smp
