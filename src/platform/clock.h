#pragma once

#include <cstdint>
#include <optional>

namespace smp {

struct QpcWallClockAnchor {
    std::uint64_t qpcTicks{};
    std::int64_t unixNanoseconds{};
};

std::optional<std::int64_t> qpcDeltaNanoseconds(std::uint64_t anchorTicks,
                                                std::uint64_t timestampTicks,
                                                std::uint64_t frequency) noexcept;

class QpcClock {
  public:
    QpcClock();
    [[nodiscard]] std::uint64_t now() const noexcept;
    [[nodiscard]] QpcWallClockAnchor wallClockAnchorAt(std::uint64_t qpcTicks) const noexcept;
    [[nodiscard]] std::uint64_t frequency() const noexcept {
        return frequency_;
    }

  private:
    std::uint64_t frequency_{};
};

} // namespace smp
