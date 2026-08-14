#pragma once

#include "app/game_analysis_visualization_model.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <windows.h>

namespace smp {

class AnalysisWindow {
  public:
    AnalysisWindow() = default;
    ~AnalysisWindow();
    AnalysisWindow(const AnalysisWindow&) = delete;
    AnalysisWindow& operator=(const AnalysisWindow&) = delete;

    void open(HWND owner, GameAnalysisVisualizationModel model);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept;

  private:
    void run(HWND owner, GameAnalysisVisualizationModel model) noexcept;

    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<HWND> window_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<bool> closeRequested_{false};
};

} // namespace smp
