#pragma once

#include "capture/captured_event.h"
#include "capture/ring_buffer.h"
#include "platform/clock.h"
#include "platform/foreground.h"
#include "platform/screen_regions.h"
#include "platform/starcraft_display_mode.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <windows.h>

namespace smp {

using CapturedEventQueue = SpscRingBuffer<CapturedInputEvent, 65536>;

enum class CollectorState : std::uint8_t {
    Waiting,
    Recording,
    Paused,
    Failed,
    Stopped
};

enum class CollectorForegroundTransition : std::uint8_t {
    None,
    Gained,
    Lost,
};

struct CollectorForegroundDecision {
    CollectorForegroundTransition transition{CollectorForegroundTransition::None};
    bool refreshGeometry{};
};

[[nodiscard]] CollectorForegroundDecision collectorForegroundDecision(
    bool previouslyForeground, bool currentlyForeground,
    bool periodicGeometryRefresh) noexcept;
[[nodiscard]] RawInputEvent makeCollectorForegroundTransitionEvent(
    CollectorForegroundTransition transition,
    std::uint64_t observationTimestampTicks,
    int cursorX, int cursorY) noexcept;

// Collector-thread-only, single-use context; match the complete dispatch identity.
struct CollectorDispatchContext {
    MSG queued{};
    bool valid{};

    void begin(const MSG& message) noexcept { queued = message; valid = true; }
    void clear() noexcept { valid = false; }
    [[nodiscard]] std::optional<POINT> takeCursor(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
        if (!valid || queued.hwnd != window || queued.message != message ||
            queued.wParam != wParam || queued.lParam != lParam)
            return std::nullopt;
        valid = false;
        return queued.pt;
    }
};

class Collector {
  public:
    Collector(CapturedEventQueue& queue, std::wstring expectedProcess, const QpcClock& clock);
    ~Collector();
    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;

    bool start();
    void stop();
    [[nodiscard]] CollectorState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t droppedEvents() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::string error() const;
    [[nodiscard]] std::optional<ScreenRegions> screenRegions() const;

  private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void run();
    void updateForeground(bool periodicGeometryRefresh,
                          std::uint64_t observationTimestampTicks);
    void push(RawInputEvent event);

    CapturedEventQueue& queue_;
    ForegroundMatcher foreground_;
    const QpcClock& clock_;
    StarcraftDisplayModeWatcher displayModeWatcher_;
    std::thread thread_;
    std::atomic<CollectorState> state_{CollectorState::Stopped};
    std::atomic<std::uint64_t> dropped_{0};
    std::uint64_t nextSequence_{1};
    std::atomic<HWND> window_{nullptr};
    std::atomic<DWORD> threadId_{0};
    CollectorDispatchContext dispatchContext_;
    bool foregroundActive_{};
    bool everActive_{};
    mutable std::mutex screenRegionsMutex_;
    std::optional<ScreenRegions> screenRegions_;
    mutable std::mutex startupMutex_;
    std::condition_variable startupCv_;
    bool startupComplete_{};
    bool startupSuccess_{};
    std::string error_;
};

} // namespace smp
