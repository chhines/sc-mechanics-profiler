#pragma once

#include <cstdint>

namespace scm {

class QpcClock {
  public:
    QpcClock();
    [[nodiscard]] std::uint64_t now() const noexcept;
    [[nodiscard]] std::uint64_t frequency() const noexcept {
        return frequency_;
    }

  private:
    std::uint64_t frequency_{};
};

} // namespace scm
