#include "platform/clock.h"

#include <stdexcept>
#include <windows.h>

namespace smp {

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

} // namespace smp
