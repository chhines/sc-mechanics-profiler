#pragma once

#include <cstdint>
#include <span>

namespace smp {

struct BgraImageView {
    int width{};
    int height{};
    int stride{};
    std::span<const std::uint8_t> pixels;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 && stride >= width * 4 &&
               pixels.size() >= static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    }
};

bool containsMinimapViewportOutline(const BgraImageView& image) noexcept;

enum class MinimapDetectorState {
    WaitForAppearance,
    WaitForAbsence,
};

struct MinimapConfirmationResult {
    bool startDetected{};
    bool rearmed{};
};

class MinimapStartConfirmation {
  public:
    explicit MinimapStartConfirmation(MinimapDetectorState initialState = MinimapDetectorState::WaitForAppearance)
        : state_(initialState) {}

    MinimapConfirmationResult observe(bool viewportPresent) noexcept;
    [[nodiscard]] MinimapDetectorState state() const noexcept {
        return state_;
    }

  private:
    MinimapDetectorState state_;
    int consecutivePresent_{};
    int consecutiveAbsent_{};
};

} // namespace smp
