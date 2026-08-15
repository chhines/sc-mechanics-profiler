#pragma once

#include "platform/screen_regions.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <windows.h>

namespace smp {

inline constexpr wchar_t screenRegionOverlayClassName[] =
    L"StarcraftMechanicsProfilerScreenRegionOverlay";

struct ScreenRegionOverlayModel {
    bool visible{};
    ScreenRect desktopBounds{};
    ScreenRect clientRect{};
    ScreenRect gameViewportRect{};
    ScreenRect minimapRect{};
    ScreenRect commandCardRect{};
    ScreenRect automaticCandidateRect{};
    std::array<ScreenRect, 4> edgeMarginRects{};
    MinimapRegionSource minimapSource{MinimapRegionSource::Unavailable};
    StarcraftDisplayMode displayMode{StarcraftDisplayMode::Unknown};

    bool operator==(const ScreenRegionOverlayModel&) const noexcept = default;
};

ScreenRect overlayLocalRect(const ScreenRect& desktopRect,
                            const ScreenRect& desktopBounds) noexcept;
std::array<ScreenRect, 4> overlayEdgeMarginRects(
    const ScreenRect& localGameArea, int edgeMarginPx) noexcept;
ScreenRegionOverlayModel makeScreenRegionOverlayModel(
    const ScreenRegions& regions, const ResolvedMinimapRegion& resolvedMinimap,
    int edgeMarginPx, bool starcraftForeground) noexcept;

enum class OverlayCapturePolicy : std::uint8_t {
    Capturable,
    ExcludeFromCapture,
};

class ScreenRegionDebugOverlay {
  public:
    ScreenRegionDebugOverlay() = default;
    ~ScreenRegionDebugOverlay();
    ScreenRegionDebugOverlay(const ScreenRegionDebugOverlay&) = delete;
    ScreenRegionDebugOverlay& operator=(const ScreenRegionDebugOverlay&) = delete;

    [[nodiscard]] bool start(OverlayCapturePolicy capturePolicy);
    void update(const ScreenRegionOverlayModel& model) noexcept;
    void hide() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool captureExclusionApplied() const noexcept {
        return captureExclusionApplied_.load(std::memory_order_acquire);
    }

  private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message,
                                            WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam,
                          LPARAM lParam);
    void run();
    void applyPendingModel(HWND window);
    void paint(HWND window);

    std::thread thread_;
    std::atomic<HWND> window_{nullptr};
    std::atomic<bool> captureExclusionApplied_{false};
    OverlayCapturePolicy capturePolicy_{OverlayCapturePolicy::Capturable};
    std::mutex startupMutex_;
    std::condition_variable startupReady_;
    bool startupComplete_{};
    bool startupSucceeded_{};
    std::mutex modelMutex_;
    ScreenRegionOverlayModel pendingModel_{};
    ScreenRegionOverlayModel currentModel_{};
};

} // namespace smp
