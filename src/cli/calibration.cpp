#include "cli/calibration.h"

#include "capture/collector.h"
#include "platform/clock.h"
#include "platform/foreground.h"
#include "platform/screen_regions.h"

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <windows.h>

namespace smp {
namespace {

void printRect(const char* label, const ScreenRect& rect) {
    std::cout << label << "(" << rect.left << ',' << rect.top << ") -> (" << rect.right << ',' << rect.bottom
              << ")  " << rect.width() << 'x' << rect.height() << '\n';
}

} // namespace

int runCalibration(Config& config, const std::filesystem::path& configPath,
                   const std::function<void(std::string)>& progress,
                   const std::atomic<bool>* requested) {
    const std::string captureKeyName = virtualKeyToName(config.calibrationCaptureKey);
    std::cout << "Starcraft Mechanics Profiler minimap calibration\n\n"
              << "Waiting for StarCraft to become foreground...\n\n"
              << "After switching to StarCraft, remain in the game for both steps:\n"
              << "  1. Move to the TOP-LEFT of the clickable minimap and press " << captureKeyName << ".\n"
              << "  2. Move to the BOTTOM-RIGHT of the clickable minimap and press " << captureKeyName << ".\n\n"
              << "No Alt+Tab or console Enter press is needed between points.\n"
              << std::flush;
    if (progress)
        progress("Waiting for StarCraft. Capture the minimap top-left and bottom-right with " +
                 captureKeyName + ".");

    QpcClock clock;
    RawEventQueue queue;
    Collector collector(queue, config.starcraftProcess, clock);
    if (!collector.start())
        throw std::runtime_error(collector.error());
    ForegroundMatcher foreground(config.starcraftProcess);

    bool starcraftActive = false;
    bool captureKeyHeld = false;
    bool announcedActive = false;
    std::optional<ScreenPoint> topLeft;
    std::optional<ScreenRegions> firstGeometry;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);

    while (std::chrono::steady_clock::now() < deadline &&
           (!requested || requested->load(std::memory_order_acquire))) {
        bool consumed = false;
        RawInputEvent event{};
        while (queue.tryPop(event)) {
            consumed = true;
            if (event.type == RawEventType::ForegroundGained) {
                starcraftActive = true;
                captureKeyHeld = false;
                if (!announcedActive) {
                    announcedActive = true;
                    std::cout << "\nCalibration active.\n\n"
                              << "Move cursor to TOP-LEFT of clickable minimap.\n"
                              << "Press " << captureKeyName << " to capture.\n"
                              << std::flush;
                    if (progress)
                        progress("StarCraft active. Move to the minimap top-left and press " +
                                 captureKeyName + ".");
                }
                continue;
            }
            if (event.type == RawEventType::ForegroundLost) {
                starcraftActive = false;
                captureKeyHeld = false;
                continue;
            }
            if (event.virtualKey != config.calibrationCaptureKey)
                continue;
            if (event.type == RawEventType::KeyUp) {
                captureKeyHeld = false;
                continue;
            }
            if (event.type != RawEventType::KeyDown || captureKeyHeld)
                continue;
            captureKeyHeld = true;

            // The collector already filters input by foreground process, and this
            // second check prevents accepting a queued key after focus changed.
            const HWND foregroundWindow = GetForegroundWindow();
            if (!starcraftActive || !foreground.matches(foregroundWindow))
                continue;
            const auto geometry = detectScreenRegionsForWindow(foregroundWindow);
            if (!geometry)
                continue;

            const ScreenPoint point{event.cursorX, event.cursorY};
            if (!geometry->gameArea.contains(point)) {
                std::cout << "\nCalibration point (" << point.x << ',' << point.y
                          << ") is outside the derived StarCraft game area.\n";
                if (topLeft) {
                    std::cout << "Restarting minimap calibration. Move to TOP-LEFT and press " << captureKeyName
                              << ".\n";
                    topLeft.reset();
                    firstGeometry.reset();
                } else {
                    std::cout << "Move to TOP-LEFT and try again.\n";
                }
                std::cout << std::flush;
                MessageBeep(MB_ICONERROR);
                if (progress)
                    progress("Invalid point. Restarting at the minimap top-left.");
                continue;
            }

            if (!topLeft) {
                topLeft = point;
                firstGeometry = geometry;
                std::cout << "\nTop-left captured: (" << point.x << ',' << point.y << ")\n\n"
                          << "Move cursor to BOTTOM-RIGHT of clickable minimap.\n"
                          << "Press " << captureKeyName << " to capture.\n"
                          << std::flush;
                MessageBeep(MB_OK);
                if (progress)
                    progress("Top-left captured. Move to the minimap bottom-right and press " +
                             captureKeyName + ".");
                continue;
            }

            const bool geometryChanged = !firstGeometry || firstGeometry->clientArea != geometry->clientArea;
            const ScreenRect minimap{topLeft->x, topLeft->y, point.x, point.y};
            if (geometryChanged || !isReasonableMinimapRect(minimap, geometry->gameArea)) {
                std::cout << "\nCalibration invalid: bottom-right must be below and to the right of top-left, both "
                             "points must be inside the game area, and the rectangle must have nonzero size.\n"
                          << "Restarting minimap calibration. Move to TOP-LEFT and press " << captureKeyName << ".\n"
                          << std::flush;
                MessageBeep(MB_ICONERROR);
                topLeft.reset();
                firstGeometry.reset();
                if (progress)
                    progress("Invalid rectangle. Restarting at the minimap top-left.");
                continue;
            }

            config.autoScreenRegions = true;
            config.gameArea = geometry->gameArea;
            config.viewport = geometry->viewport;
            config.minimap = minimap;
            config.commandCard = {};
            config.calibratedMinimap = normalizeScreenRect(minimap, geometry->gameArea);
            config.save(configPath);

            std::cout << "\nBottom-right captured: (" << point.x << ',' << point.y << ")\n\n"
                      << "Minimap calibration complete.\n";
            printRect("Client:   ", geometry->clientArea);
            printRect("Game:     ", geometry->gameArea);
            printRect("Minimap:  ", minimap);
            std::cout << "Configuration saved to " << configPath.string() << "\n" << std::flush;
            MessageBeep(MB_OK);
            if (progress)
                progress("Minimap calibration complete.");
            collector.stop();
            return 0;
        }
        if (!consumed)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    collector.stop();
    if (requested && !requested->load(std::memory_order_acquire)) {
        if (progress)
            progress("Minimap calibration cancelled.");
        return 1;
    }
    throw std::runtime_error("Minimap calibration timed out after five minutes");
}

} // namespace smp
