#include "cli/commands.h"

#include "analysis/analyzer.h"
#include "capture/collector.h"
#include "cli/automatic_session_stats.h"
#include "cli/calibration.h"
#include "cli/report.h"
#include "config/config.h"
#include "platform/automatic_lifecycle.h"
#include "platform/clock.h"
#include "platform/screen_regions.h"
#include "storage/session.h"
#include "util/json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <windows.h>

namespace smp {
namespace {

std::atomic<bool> recordingRequested{false};
std::atomic<bool> automaticRequested{false};

BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        recordingRequested.store(false, std::memory_order_release);
        automaticRequested.store(false, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

class ConsoleHandlerRegistration {
  public:
    ConsoleHandlerRegistration() {
        if (!SetConsoleCtrlHandler(consoleHandler, TRUE))
            throw std::runtime_error("Unable to register the console stop handler");
    }
    ~ConsoleHandlerRegistration() {
        SetConsoleCtrlHandler(consoleHandler, FALSE);
    }
    ConsoleHandlerRegistration(const ConsoleHandlerRegistration&) = delete;
    ConsoleHandlerRegistration& operator=(const ConsoleHandlerRegistration&) = delete;
};

struct RecordOptions {
    bool verbose{};
    bool showRaw{};
    bool saveRaw{};
    bool debugRegions{};
    bool debugNavigation{};
    bool quiet{};
};

RecordOptions parseRecordOptions(const std::vector<std::string>& arguments) {
    RecordOptions options;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        if (argument == "--verbose")
            options.verbose = true;
        else if (argument == "--show-raw-events")
            options.showRaw = true;
        else if (argument == "--save-raw")
            options.saveRaw = true;
        else if (argument == "--debug-regions")
            options.debugRegions = true;
        else if (argument == "--debug-navigation")
            options.debugNavigation = true;
        else if (argument == "--quiet")
            options.quiet = true;
        else
            throw std::runtime_error("Unknown record option: " + argument);
    }
    return options;
}

bool sameRect(const ScreenRect& first, const ScreenRect& second) {
    return first.left == second.left && first.top == second.top && first.right == second.right &&
           first.bottom == second.bottom;
}

bool sameRegions(const ScreenRegions& first, const ScreenRegions& second) {
    return first.clientArea == second.clientArea && first.gameArea == second.gameArea &&
           first.viewport == second.viewport && first.minimap == second.minimap &&
           first.commandCard == second.commandCard;
}

void printRect(const char* label, const ScreenRect& rect) {
    std::cout << std::left << std::setw(24) << label;
    if (!rect.valid()) {
        std::cout << "not calibrated\n";
        return;
    }
    std::cout << '(' << rect.left << ',' << rect.top << ") -> (" << rect.right << ',' << rect.bottom << ")  "
              << rect.width() << 'x' << rect.height() << '\n';
}

void printRegionDiagnostics(const ScreenRegions& regions, int edgeMarginPx) {
    std::cout << "\nStarCraft geometry\n\n";
    printRect("Client:", regions.clientArea);
    printRect("Derived 4:3 game area:", regions.gameArea);
    printRect("Calibrated minimap:", regions.minimap);
    std::cout << std::left << std::setw(24) << "Edge margin:" << edgeMarginPx << " px\n\n";
}

void printFocusGeometry(const ScreenRegions& previous, const ScreenRegions& current) {
    const bool clientSame = sameRect(previous.clientArea, current.clientArea);
    const bool gameSame = sameRect(previous.gameArea, current.gameArea);
    const bool minimapSame = sameRect(previous.minimap, current.minimap);
    std::cout << "\nFOCUS REGAINED\n\n"
              << "Client:   " << (clientSame ? "unchanged" : "changed") << '\n'
              << "Game:     " << (gameSame ? "unchanged" : "changed") << '\n'
              << "Minimap:  " << (minimapSame ? "unchanged" : "changed") << '\n';
    if (!clientSame) {
        printRect("Old client:", previous.clientArea);
        printRect("New client:", current.clientArea);
    }
    if (!gameSame) {
        printRect("Old game:", previous.gameArea);
        printRect("New game:", current.gameArea);
    }
    if (!minimapSame) {
        printRect("Old minimap:", previous.minimap);
        printRect("New minimap:", current.minimap);
    }
    if (clientSame && (!gameSame || !minimapSame))
        std::cout << "WARNING: geometry changed despite identical StarCraft client rectangle\n";
    std::cout << '\n';
}

void printNavigationDebug(const CameraNavigationEvent& event) {
    std::cout << std::fixed << std::setprecision(3) << std::setw(8) << event.activeMs / 1000.0 << "  ";
    switch (event.type) {
    case CameraNavigationType::ControlGroupJump:
        std::cout << "CG_JUMP       group=" << event.id;
        break;
    case CameraNavigationType::LocationHotkey:
        std::cout << "LOCATION      F" << event.id;
        break;
    case CameraNavigationType::MinimapJump:
        std::cout << "MINIMAP       x=" << event.cursorX << " y=" << event.cursorY;
        break;
    case CameraNavigationType::EdgeScroll:
        std::cout << "EDGE_SCROLL   " << edgeDirectionName(event.edgeDirection) << " duration="
                  << std::setprecision(0) << event.durationMs << "ms";
        break;
    }
    std::cout << '\n';
}

void printRecenterDebug(const CameraRecenterEvent& event) {
    std::cout << std::fixed << std::setprecision(3) << std::setw(8) << event.activeMs / 1000.0 << "  ";
    if (event.type == CameraRecenterType::ControlGroup)
        std::cout << "CG_RECENTER   group=" << event.id;
    else
        std::cout << "LOCATION_REVISIT F" << event.id;
    std::cout << '\n';
}

void drainAnalyzerDebug(Analyzer& analyzer, const RecordOptions& options) {
    const auto navigation = analyzer.takeEmittedNavigationEvents();
    const auto recenters = analyzer.takeEmittedRecenters();
    if (!options.debugNavigation || options.quiet)
        return;
    for (const auto& event : navigation)
        printNavigationDebug(event);
    for (const auto& event : recenters)
        printRecenterDebug(event);
}

void printRegionDebug(const RawInputEvent& event, const ScreenRegions& regions, int edgeMarginPx,
                      EdgeDirection& previousEdge, const RecordOptions& options) {
    if (!options.debugRegions || options.quiet || !regions.gameArea.valid())
        return;
    const ScreenPoint cursor{event.cursorX, event.cursorY};
    if (event.type == RawEventType::MouseLeftDown || event.type == RawEventType::MouseLeftUp) {
        std::cout << (event.type == RawEventType::MouseLeftDown ? "LEFT_DOWN" : "LEFT_UP  ") << "  x="
                  << event.cursorX << " y=" << event.cursorY
                  << " region=" << screenRegionName(classifyScreenRegion(regions, cursor)) << '\n';
    }
    if (event.type == RawEventType::MouseMove) {
        const auto edge = edgeDirectionAt(regions.gameArea, edgeMarginPx, cursor);
        if (edge != previousEdge && edge != EdgeDirection::None)
            std::cout << "CURSOR_EDGE x=" << event.cursorX << " y=" << event.cursorY
                      << " edge=" << edgeDirectionName(edge) << '\n';
        previousEdge = edge;
    } else if (event.type == RawEventType::ForegroundLost) {
        previousEdge = EdgeDirection::None;
    }
}

struct RecordingSessionResult {
    AnalysisResult analysis;
    std::filesystem::path navPath;
};

RecordingSessionResult runRecordingSession(const std::filesystem::path& workingDirectory, Config config,
                                           const std::vector<std::string>& arguments, bool showSummary) {
    const auto options = parseRecordOptions(arguments);
    QpcClock clock;
    RawEventQueue queue;
    Collector collector(queue, config.starcraftProcess, clock);
    if (!collector.start())
        throw std::runtime_error(collector.error());

    SessionWriter writer(workingDirectory / "sessions", clock.frequency(), config.flushIntervalMs,
                         options.saveRaw);
    // Absolute rectangles in config.json are calibration diagnostics, not a
    // runtime fallback. Live detection must establish geometry for this focus
    // session before minimap or edge classification can begin.
    Config runtimeConfig = config;
    runtimeConfig.gameArea = {};
    runtimeConfig.viewport = {};
    runtimeConfig.minimap = {};
    runtimeConfig.commandCard = {};
    Analyzer analyzer(std::move(runtimeConfig), clock.frequency());
    ScreenRegions activeRegions{};
    std::optional<ScreenRect> announcedGameArea;
    std::optional<ScreenRegions> previousFocusRegions;
    EdgeDirection previousDebugEdge = EdgeDirection::None;
    bool awaitingGeometry = false;
    bool activeTimelineAnchored = false;

    if (!options.quiet) {
        if (options.debugNavigation && options.debugRegions) {
            std::cout << "\nLIVE DETECTION DEBUG MODE\n\n"
                      << "This shows detected camera actions, click regions, edge scrolling,\n"
                      << "and geometry stability when StarCraft regains focus.\n"
                      << "Only input received while StarCraft is active is analyzed.\n\n";
        }
        std::cout << "Waiting for StarCraft...\n\nPress Ctrl+C to stop.\n";
        if (options.verbose) {
            std::cout << "Session: " << writer.sessionId() << '\n';
            if (writer.rawEnabled())
                std::cout << "Raw events: " << writer.rawPath().string() << '\n';
        }
    }

    const auto applyGeometryWhenReady = [&]() {
        if (!awaitingGeometry)
            return;
        auto selected = collector.screenRegions();
        if (!selected)
            return;
        *selected = withCalibratedMinimap(*selected, config.calibratedMinimap);
        if (!sameRegions(activeRegions, *selected)) {
            activeRegions = *selected;
            analyzer.setScreenRegions(*selected);
        }
        const bool changed = !announcedGameArea || !sameRect(*announcedGameArea, selected->gameArea);
        if (changed && !options.quiet)
            printRegionDiagnostics(*selected, config.edgeMarginPx);
        if (!selected->minimap.valid() && !options.quiet)
            std::cout << "Minimap is not calibrated. Choose Calibrate minimap from the main menu.\n\n";
        if (previousFocusRegions && options.debugRegions && !options.quiet)
            printFocusGeometry(*previousFocusRegions, *selected);
        previousFocusRegions = *selected;
        announcedGameArea = selected->gameArea;
        awaitingGeometry = false;
    };

    const auto consume = [&](const RawInputEvent& event) {
        writer.submitRaw(event);

        if (event.type == RawEventType::ForegroundGained) {
            if (!activeTimelineAnchored) {
                writer.setActiveTimelineAnchor(clock.wallClockAnchorAt(event.timestampTicks));
                activeTimelineAnchored = true;
            }
            activeRegions = {};
            analyzer.setScreenRegions(activeRegions);
            awaitingGeometry = true;
            applyGeometryWhenReady();
            if (awaitingGeometry && !options.quiet)
                std::cout << "StarCraft geometry is not ready yet. Retrying automatically...\n";
        } else if (event.type == RawEventType::ForegroundLost) {
            awaitingGeometry = false;
        } else {
            // When initial activation geometry was transiently unavailable, the
            // collector retries before posting its next input/timer event.
            applyGeometryWhenReady();
        }

        printRegionDebug(event, activeRegions, config.edgeMarginPx, previousDebugEdge, options);
        if (options.showRaw && !options.quiet) {
            std::cout << "RAW " << event.sequence << " type=" << static_cast<int>(event.type)
                      << " vk=" << event.virtualKey << " cursor=(" << event.cursorX << ',' << event.cursorY << ")\n";
        }
        analyzer.process(event);
        drainAnalyzerDebug(analyzer, options);
    };

    CollectorState announced = CollectorState::Waiting;
    while (recordingRequested.load(std::memory_order_acquire)) {
        bool consumed = false;
        RawInputEvent event{};
        while (queue.tryPop(event)) {
            consumed = true;
            consume(event);
        }
        const auto state = collector.state();
        if (state != announced && !options.quiet) {
            if (state == CollectorState::Recording) {
                std::cout << (announced == CollectorState::Paused ? "StarCraft active. Recording resumed.\n"
                                                                  : "StarCraft detected. Recording.\n");
            } else if (state == CollectorState::Paused) {
                std::cout << "StarCraft inactive. Recording paused.\n";
            }
            announced = state;
        }
        if (!consumed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    collector.stop();
    RawInputEvent event{};
    while (queue.tryPop(event))
        consume(event);

    analyzer.finalize(clock.now(), collector.droppedEvents() + writer.droppedEvents());
    drainAnalyzerDebug(analyzer, options);
    writer.stop();
    if (writer.failed())
        throw std::runtime_error("Session storage failed while writing the optional raw event file");
    analyzer.setDroppedEventCount(collector.droppedEvents() + writer.droppedEvents());
    RecordingSessionResult completed{analyzer.result(), {}};
    completed.navPath = writer.writeNavigation(completed.analysis);

    if (showSummary && !options.quiet)
        printSummary(analysisToJson(completed.analysis, writer.sessionId()), completed.navPath);
    return completed;
}

int record(const std::filesystem::path& workingDirectory, Config config,
           const std::vector<std::string>& arguments) {
    automaticRequested.store(false, std::memory_order_release);
    recordingRequested.store(true, std::memory_order_release);
    ConsoleHandlerRegistration consoleHandlerRegistration;
    try {
        (void)runRecordingSession(workingDirectory, std::move(config), arguments, true);
        recordingRequested.store(false, std::memory_order_release);
        return 0;
    } catch (...) {
        recordingRequested.store(false, std::memory_order_release);
        throw;
    }
}

enum class AutomaticEventType {
    MinimapViewportDetected,
    LastReplayChanged,
    RecorderEnded,
};

struct AutomaticEvent {
    AutomaticEventType type{};
    ReplayMetadata replay;
    std::uint64_t generation{};
    std::exception_ptr failure;
};

struct FinishedAutomaticRecording {
    std::uint64_t generation{};
    std::optional<RecordingSessionResult> result;
};

int automaticRecord(const std::filesystem::path& workingDirectory, Config config,
                    const std::vector<std::string>& arguments, bool controlledByMenu = false,
                    const std::function<void()>& readyCallback = {}) {
    // Validate options before any background monitoring begins. The automatic
    // command accepts the same recorder diagnostics as the manual command.
    auto recorderArguments = arguments;
    if (controlledByMenu)
        recorderArguments.push_back("--quiet");
    (void)parseRecordOptions(recorderArguments);

    const auto lastReplayPath = defaultLastReplayPath();
    AutomaticLifecycleState lifecycle;
    LastReplayWatcher replayWatcher;
    MinimapStartMonitor startMonitor(config.starcraftProcess, config.calibratedMinimap, !controlledByMenu);
    std::mutex eventMutex;
    std::condition_variable eventReady;
    std::deque<AutomaticEvent> events;
    std::thread recorderThread;
    std::optional<RecordingSessionResult> recorderResult;
    AutomaticSessionState sessionStats;
    std::uint64_t nextGeneration = 0;
    std::uint64_t activeGeneration = 0;

    const auto enqueue = [&](AutomaticEvent event) {
        {
            std::scoped_lock lock(eventMutex);
            events.push_back(std::move(event));
        }
        eventReady.notify_one();
    };
    const auto startMinimapDetector = [&](MinimapDetectorState initialState) {
        return startMonitor.start([&]() { enqueue({AutomaticEventType::MinimapViewportDetected}); }, initialState);
    };

    automaticRequested.store(true, std::memory_order_release);
    recordingRequested.store(false, std::memory_order_release);
    ConsoleHandlerRegistration consoleHandlerRegistration;
    if (!startMinimapDetector(MinimapDetectorState::WaitForAppearance))
        throw std::runtime_error("Unable to start the minimap detector. Calibrate the minimap first.");

    if (controlledByMenu) {
        std::cout << "\nWaiting for user to enter game...\n";
    } else {
        std::cout << "\nAUTOMATIC GAME RECORDING\n\n"
                  << "Waiting for the white camera viewport outline on the calibrated minimap.\n"
                  << "Recording starts after two consecutive detections and stops when LastReplay.rep changes.\n"
                  << "Keep this window open. Press Ctrl+C here to leave automatic mode.\n\n";
        std::cout << "LastReplay: " << lastReplayPath.string() << '\n';
    }
    if (readyCallback)
        readyCallback();

    const auto finishRecorder = [&]() {
        const auto finishedGeneration = activeGeneration;
        recordingRequested.store(false, std::memory_order_release);
        replayWatcher.stop();
        if (recorderThread.joinable())
            recorderThread.join();
        FinishedAutomaticRecording finished{finishedGeneration, std::move(recorderResult)};
        recorderResult.reset();
        activeGeneration = 0;
        return finished;
    };
    const auto cleanup = [&]() {
        startMonitor.stop();
        lifecycle.forceStop();
        auto finished = finishRecorder();
        automaticRequested.store(false, std::memory_order_release);
        return finished;
    };
    const auto addCompletedGame = [&](FinishedAutomaticRecording finished) {
        return finished.generation != 0 && finished.result &&
               sessionStats.addFinalizedGame(finished.generation, finished.result->analysis);
    };

    try {
        while (automaticRequested.load(std::memory_order_acquire)) {
            AutomaticEvent event;
            {
                std::unique_lock lock(eventMutex);
                eventReady.wait_for(lock, std::chrono::milliseconds(250), [&]() {
                    return !events.empty() || !automaticRequested.load(std::memory_order_acquire);
                });
                if (events.empty())
                    continue;
                event = std::move(events.front());
                events.pop_front();
            }

            if (event.type == AutomaticEventType::MinimapViewportDetected) {
                startMonitor.stop();
                const auto baseline = readReplayMetadata(lastReplayPath);
                if (!lifecycle.tryStart(baseline)) {
                    if (!controlledByMenu)
                        std::cout << "AUTO_START_IGNORED already_recording\n";
                    continue;
                }
                const std::uint64_t generation = ++nextGeneration;
                activeGeneration = generation;
                if (!controlledByMenu) {
                    std::cout << "\nLASTREPLAY_BASELINE\n"
                              << "path=" << lastReplayPath.string() << '\n'
                              << "writeTimeUtc=" << formatReplayWriteTimeUtc(baseline) << '\n'
                              << "size=" << (baseline.exists ? std::to_string(baseline.size) : "missing") << "\n\n";
                }
                if (!replayWatcher.start(lastReplayPath, baseline, [&, generation](const ReplayMetadata& current) {
                        enqueue({AutomaticEventType::LastReplayChanged, current, generation});
                    })) {
                    lifecycle.forceStop();
                    activeGeneration = 0;
                    throw std::runtime_error(replayWatcher.error());
                }

                recordingRequested.store(true, std::memory_order_release);
                if (controlledByMenu)
                    std::cout << "\nRecording started...\n";
                else
                    std::cout << "AUTO_START\nreason=minimap_viewport_detected\n";
                recorderResult.reset();
                recorderThread = std::thread([&, generation]() {
                    std::exception_ptr failure;
                    try {
                        recorderResult = runRecordingSession(workingDirectory, config, recorderArguments, false);
                    } catch (...) {
                        failure = std::current_exception();
                    }
                    enqueue({AutomaticEventType::RecorderEnded, {}, generation, failure});
                });
                continue;
            }

            if (event.type == AutomaticEventType::LastReplayChanged) {
                if (event.generation != activeGeneration || !lifecycle.tryStop(event.replay)) {
                    if (!controlledByMenu)
                        std::cout << "LASTREPLAY_CHANGE_IGNORED not_recording\n";
                    continue;
                }
                if (!controlledByMenu) {
                    std::cout << "\nAUTO_STOP\n"
                              << "reason=LastReplay.rep changed\n"
                              << "writeTimeUtc=" << formatReplayWriteTimeUtc(event.replay) << '\n'
                              << "size=" << event.replay.size << "\n";
                }
                if (addCompletedGame(finishRecorder()))
                    printAutomaticSessionReport(sessionStats);
                if (!startMinimapDetector(MinimapDetectorState::WaitForAbsence))
                    throw std::runtime_error("Unable to restart the minimap detector");
                if (controlledByMenu)
                    std::cout << "\nWaiting for user to enter game...\n";
                continue;
            }

            if (event.type == AutomaticEventType::RecorderEnded && event.generation == activeGeneration &&
                lifecycle.state() == AutomaticRecordingState::Recording) {
                lifecycle.forceStop();
                (void)finishRecorder();
                if (event.failure)
                    std::rethrow_exception(event.failure);
                throw std::runtime_error("The recorder stopped unexpectedly during automatic recording");
            }
        }
    } catch (...) {
        (void)cleanup();
        throw;
    }
    (void)addCompletedGame(cleanup());
    printAutomaticSessionReport(sessionStats);
    std::cout << "\nAutomatic recording stopped.\n";
    return 0;
}

json::Value loadNavSummary(const std::filesystem::path& path) {
    const auto session = readNavSession(path);
    return analysisToJson(session.analysis, session.sessionId);
}

int summaryCommand(const std::filesystem::path& sessionsRoot, const std::vector<std::string>& arguments) {
    if (arguments.size() != 2)
        throw std::runtime_error("Usage: \"Starcraft Mechanics Profiler.exe\" summary <latest|session-id>");
    const auto path = resolveNavSession(sessionsRoot, arguments[1]);
    printSummary(loadNavSummary(path), path);
    return 0;
}

int compareCommand(const std::filesystem::path& sessionsRoot, const std::vector<std::string>& arguments) {
    if (arguments.size() != 3)
        throw std::runtime_error(
            "Usage: \"Starcraft Mechanics Profiler.exe\" compare <session-id> <session-id> | compare last <N>");
    if (arguments[1] == "last") {
        const int count = std::stoi(arguments[2]);
        if (count <= 0)
            throw std::runtime_error("N must be positive");
        const auto paths = listNavSessions(sessionsRoot);
        if (paths.size() < 2)
            throw std::runtime_error("At least two sessions are required for comparison");
        std::vector<json::Value> baselines;
        const auto available = std::min<std::size_t>(static_cast<std::size_t>(count), paths.size() - 1);
        for (std::size_t i = paths.size() - 1 - available; i < paths.size() - 1; ++i)
            baselines.push_back(loadNavSummary(paths[i]));
        printComparison(loadNavSummary(paths.back()), baselines);
        return 0;
    }
    const auto current = resolveNavSession(sessionsRoot, arguments[1]);
    const auto baseline = resolveNavSession(sessionsRoot, arguments[2]);
    printComparison(loadNavSummary(current), {loadNavSummary(baseline)});
    return 0;
}

void waitForEnter() {
    std::cout << "\nPress Enter to return to the menu..." << std::flush;
    std::string line;
    std::getline(std::cin, line);
}

class AutomaticDetectorToggle {
  public:
    AutomaticDetectorToggle() = default;
    ~AutomaticDetectorToggle() {
        (void)stop();
    }

    AutomaticDetectorToggle(const AutomaticDetectorToggle&) = delete;
    AutomaticDetectorToggle& operator=(const AutomaticDetectorToggle&) = delete;

    void start(const std::filesystem::path& workingDirectory, Config config) {
        if (running())
            return;
        (void)reapFinished();
        {
            std::scoped_lock lock(stateMutex_);
            startupComplete_ = false;
            failure_ = nullptr;
        }
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this, workingDirectory, config = std::move(config)]() mutable {
            try {
                (void)automaticRecord(workingDirectory, std::move(config), {"auto"}, true, [this]() {
                    {
                        std::scoped_lock lock(stateMutex_);
                        startupComplete_ = true;
                    }
                    stateReady_.notify_all();
                });
            } catch (...) {
                std::scoped_lock lock(stateMutex_);
                failure_ = std::current_exception();
                startupComplete_ = true;
            }
            running_.store(false, std::memory_order_release);
            stateReady_.notify_all();
        });

        std::unique_lock lock(stateMutex_);
        stateReady_.wait(lock, [this]() { return startupComplete_; });
        const auto failure = failure_;
        lock.unlock();
        if (failure) {
            if (thread_.joinable())
                thread_.join();
            std::rethrow_exception(failure);
        }
    }

    std::exception_ptr stop() {
        automaticRequested.store(false, std::memory_order_release);
        if (thread_.joinable())
            thread_.join();
        running_.store(false, std::memory_order_release);
        std::scoped_lock lock(stateMutex_);
        return std::exchange(failure_, nullptr);
    }

    std::exception_ptr reapFinished() {
        if (running() || !thread_.joinable())
            return nullptr;
        thread_.join();
        std::scoped_lock lock(stateMutex_);
        return std::exchange(failure_, nullptr);
    }

    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

  private:
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex stateMutex_;
    std::condition_variable stateReady_;
    bool startupComplete_{};
    std::exception_ptr failure_;
};

int interactiveMenu(const std::filesystem::path& workingDirectory) {
    AutomaticDetectorToggle automaticDetector;
    for (;;) {
        if (const auto failure = automaticDetector.reapFinished()) {
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& error) {
                std::cout << "\nAutomatic detector stopped: " << error.what() << "\n";
            }
        }
        std::cout << "\n============================================================\n"
                  << "Starcraft Mechanics Profiler " << STARCRAFT_MECHANICS_PROFILER_VERSION << "\n"
                  << "Camera-navigation profiler for StarCraft: Remastered\n"
                  << "============================================================\n"
                  << "Data folder: " << workingDirectory.string() << "\n"
                  << "Automatic detector: " << (automaticDetector.running() ? "ON" : "OFF") << "\n\n"
                  << "  1. Turn automatic detector " << (automaticDetector.running() ? "off" : "on") << "\n"
                  << "  2. Calibrate minimap\n"
                  << "  3. Test live detection (debug mode)\n"
                  << "  4. Show configuration\n"
                  << "  5. Show latest session summary\n"
                  << "  6. Show command-line help\n"
                  << "  0. Exit\n\n"
                  << "Choose an option: " << std::flush;

        std::string choice;
        if (!std::getline(std::cin, choice)) {
            (void)automaticDetector.stop();
            return 0;
        }
        try {
            const auto configPath = workingDirectory / "config.json";
            if (choice == "1") {
                if (automaticDetector.running()) {
                    if (const auto failure = automaticDetector.stop())
                        std::rethrow_exception(failure);
                    std::cout << "\nAutomatic detector is off.\n";
                } else {
                    automaticDetector.start(workingDirectory, Config::loadOrCreate(configPath));
                }
            } else if (choice == "2") {
                if (automaticDetector.running()) {
                    std::cout << "\nTurn the automatic detector off before calibrating the minimap.\n";
                    waitForEnter();
                    continue;
                }
                auto config = Config::loadOrCreate(configPath);
                runCalibration(config, configPath);
                waitForEnter();
            } else if (choice == "3") {
                if (automaticDetector.running()) {
                    std::cout << "\nTurn the automatic detector off before starting debug mode.\n";
                    waitForEnter();
                    continue;
                }
                record(workingDirectory, Config::loadOrCreate(configPath),
                       {"record", "--debug-navigation", "--debug-regions"});
            } else if (choice == "4") {
                (void)Config::loadOrCreate(configPath);
                std::cout << "\nConfiguration: " << configPath.string() << "\n\n";
                std::ifstream input(configPath, std::ios::binary);
                std::cout << input.rdbuf();
                waitForEnter();
            } else if (choice == "5") {
                const auto path = resolveNavSession(workingDirectory / "sessions", "latest");
                printSummary(loadNavSummary(path), path);
                waitForEnter();
            } else if (choice == "6") {
                printUsage();
                waitForEnter();
            } else if (choice == "0" || choice == "q" || choice == "Q") {
                if (const auto failure = automaticDetector.stop())
                    std::rethrow_exception(failure);
                return 0;
            } else {
                std::cout << "\nPlease enter a number from 0 through 6.\n";
            }
        } catch (const std::exception& error) {
            std::cout << "\nUnable to complete that action: " << error.what() << '\n';
            waitForEnter();
        }
    }
}

} // namespace

void printUsage() {
    std::cout << "Starcraft Mechanics Profiler " << STARCRAFT_MECHANICS_PROFILER_VERSION
              << " - camera-navigation profiler for StarCraft: Remastered\n\n"
              << "Usage:\n"
              << "  \"Starcraft Mechanics Profiler.exe\" record [--debug-navigation] [--debug-regions]\n"
              << "      [--show-raw-events] [--save-raw] [--verbose] [--quiet]\n"
              << "  \"Starcraft Mechanics Profiler.exe\" auto [same options as record]\n"
              << "  \"Starcraft Mechanics Profiler.exe\" debug\n"
              << "  \"Starcraft Mechanics Profiler.exe\" calibrate\n"
              << "  \"Starcraft Mechanics Profiler.exe\" config\n"
              << "  \"Starcraft Mechanics Profiler.exe\" summary <latest|session-id>\n"
              << "  \"Starcraft Mechanics Profiler.exe\" compare <session-id> <session-id>\n"
              << "  \"Starcraft Mechanics Profiler.exe\" compare last <N>\n"
              << "  \"Starcraft Mechanics Profiler.exe\" export <latest|session-id> --csv\n";
}

int runCommand(const std::vector<std::string>& arguments, const std::filesystem::path& workingDirectory) {
    if (arguments.empty())
        return interactiveMenu(workingDirectory);
    if (arguments[0] == "--help" || arguments[0] == "-h" || arguments[0] == "help") {
        printUsage();
        return 0;
    }
    const auto configPath = workingDirectory / "config.json";
    Config config = Config::loadOrCreate(configPath);
    if (arguments[0] == "record")
        return record(workingDirectory, config, arguments);
    if (arguments[0] == "auto")
        return automaticRecord(workingDirectory, config, arguments);
    if (arguments[0] == "debug")
        return record(workingDirectory, config, {"record", "--debug-navigation", "--debug-regions"});
    if (arguments[0] == "detect-layout" || arguments[0] == "calibrate")
        return runCalibration(config, configPath);
    if (arguments[0] == "config") {
        std::cout << "Configuration: " << configPath.string() << "\n\n";
        std::ifstream input(configPath, std::ios::binary);
        std::cout << input.rdbuf();
        return 0;
    }
    if (arguments[0] == "summary")
        return summaryCommand(workingDirectory / "sessions", arguments);
    if (arguments[0] == "compare")
        return compareCommand(workingDirectory / "sessions", arguments);
    if (arguments[0] == "export") {
        if (arguments.size() != 3 || arguments[2] != "--csv")
            throw std::runtime_error("Usage: \"Starcraft Mechanics Profiler.exe\" export <latest|session-id> --csv");
        const auto path = exportSessionCsv(workingDirectory / "sessions", workingDirectory / "exports", arguments[1]);
        std::cout << "Exported: " << path.string() << '\n';
        return 0;
    }
    throw std::runtime_error("Unknown command: " + arguments[0]);
}

} // namespace smp
