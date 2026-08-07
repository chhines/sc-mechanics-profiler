#pragma once

#include "capture/raw_event.h"
#include "capture/ring_buffer.h"
#include "platform/clock.h"
#include "platform/foreground.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>

namespace scm {

using RawEventQueue = SpscRingBuffer<RawInputEvent, 65536>;

enum class CollectorState : std::uint8_t {
    Waiting,
    Recording,
    Paused,
    Failed,
    Stopped
};

class Collector {
  public:
    Collector(RawEventQueue& queue, std::wstring expectedProcess, const QpcClock& clock);
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

  private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void run();
    void updateForeground();
    void push(RawInputEvent event);

    RawEventQueue& queue_;
    ForegroundMatcher foreground_;
    const QpcClock& clock_;
    std::thread thread_;
    std::atomic<CollectorState> state_{CollectorState::Stopped};
    std::atomic<std::uint64_t> dropped_{0};
    std::uint64_t nextSequence_{1};
    std::atomic<HWND> window_{nullptr};
    std::atomic<DWORD> threadId_{0};
    bool foregroundActive_{};
    bool everActive_{};
    mutable std::mutex startupMutex_;
    std::condition_variable startupCv_;
    bool startupComplete_{};
    bool startupSuccess_{};
    std::string error_;
};

} // namespace scm
