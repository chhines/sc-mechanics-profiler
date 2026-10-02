#include "cli/commands.h"

#include "analysis/analyzer.h"
#include "analysis/captured_geometry.h"
#include "analysis/replay_analysis.h"
#include "capture/collector.h"
#include "cli/automatic_recording.h"
#include "cli/automatic_session_files.h"
#include "cli/automatic_session_stats.h"
#include "cli/calibration.h"
#include "cli/replay_readiness.h"
#include "cli/replay_snapshot.h"
#include "cli/report.h"
#include "cli/session_summary_paths.h"
#include "config/config.h"
#include "platform/automatic_lifecycle.h"
#include "platform/clock.h"
#include "platform/screen_region_overlay.h"
#include "platform/screen_regions.h"
#include "storage/nav_retention.h"
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
#include <syncstream>
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

void printRect(const char* label, const ScreenRect& rect) {
    std::cout << std::left << std::setw(24) << label;
    if (!rect.valid()) {
        std::cout << "unavailable\n";
        return;
    }
    std::cout << '(' << rect.left << ',' << rect.top << ") -> (" << rect.right << ',' << rect.bottom << ")  "
              << rect.width() << 'x' << rect.height() << '\n';
}

void printRegionDiagnostics(const ScreenRegions& regions,
                            MinimapRegionSource minimapSource, int edgeMarginPx) {
    std::cout << "\nStarCraft geometry\n\n";
    printRect("Client:", regions.clientArea);
    std::cout << std::left << std::setw(24) << "Display mode:"
              << starcraftDisplayModeName(regions.displayMode);
    if (regions.displayMode == StarcraftDisplayMode::Unknown)
        std::cout << " (original-aspect fallback)";
    std::cout << '\n';
    printRect("Resolved game area:", regions.gameArea);
    std::cout << std::left << std::setw(24) << "Minimap source:"
              << minimapRegionSourceName(minimapSource) << '\n';
    printRect("Minimap:", regions.minimap);
    std::cout << std::left << std::setw(24) << "Edge margin:" << edgeMarginPx << " px\n\n";
}

void printFocusGeometry(const ScreenRegions& previous, const ScreenRegions& current) {
    const bool clientSame = sameRect(previous.clientArea, current.clientArea);
    const bool gameSame = sameRect(previous.gameArea, current.gameArea);
    const bool minimapSame = sameRect(previous.minimap, current.minimap);
    const bool displayModeSame = previous.displayMode == current.displayMode;
    std::cout << "\nFOCUS REGAINED\n\n"
              << "Client:   " << (clientSame ? "unchanged" : "changed") << '\n'
              << "Mode:     " << (displayModeSame ? "unchanged" : "changed") << '\n'
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
    if (clientSame && (!displayModeSame || !gameSame || !minimapSame))
        std::cout << "WARNING: geometry changed despite identical StarCraft client rectangle\n";
    std::cout << '\n';
}

std::string formatNavigationDebug(const CameraNavigationEvent& event) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(3) << std::setw(8)
           << event.activeMs / 1000.0 << "  ";
    switch (event.type) {
    case CameraNavigationType::ControlGroupJump:
        output << "CG_JUMP       group=" << event.id;
        break;
    case CameraNavigationType::LocationHotkey:
        output << "LOCATION      F" << event.id;
        break;
    case CameraNavigationType::MinimapJump:
        output << "MINIMAP       x=" << event.cursorX << " y=" << event.cursorY;
        break;
    case CameraNavigationType::EdgeScroll:
        output << "EDGE_SCROLL   " << edgeDirectionName(event.edgeDirection)
               << " duration=" << std::setprecision(0) << event.durationMs << "ms";
        break;
    }
    return output.str();
}

std::string formatRecenterDebug(const CameraRecenterEvent& event) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(3) << std::setw(8)
           << event.activeMs / 1000.0 << "  ";
    if (event.type == CameraRecenterType::ControlGroup)
        output << "CG_RECENTER   group=" << event.id;
    else
        output << "LOCATION_REVISIT F" << event.id;
    return output.str();
}

void emitDiagnostic(const ProfilerCallbacks* callbacks, const std::string& message) {
    if (callbacks && callbacks->diagnostic)
        callbacks->diagnostic(message);
}

void notifyStatus(const ProfilerCallbacks* callbacks, ProfilerActivity activity,
                  std::string detail) {
    if (callbacks && callbacks->statusChanged)
        callbacks->statusChanged(activity, std::move(detail));
}

void drainAnalyzerDebug(Analyzer& analyzer, const RecordOptions& options,
                        const ProfilerCallbacks* callbacks) {
    const auto navigation = analyzer.takeEmittedNavigationEvents();
    const auto recenters = analyzer.takeEmittedRecenters();
    if (!options.debugNavigation)
        return;
    for (const auto& event : navigation) {
        const auto line = formatNavigationDebug(event);
        if (!options.quiet)
            std::cout << line << '\n';
        emitDiagnostic(callbacks, line);
    }
    for (const auto& event : recenters) {
        const auto line = formatRecenterDebug(event);
        if (!options.quiet)
            std::cout << line << '\n';
        emitDiagnostic(callbacks, line);
    }
}

void printRegionDebug(const RawInputEvent& event, const ScreenRegions& regions, int edgeMarginPx,
                      EdgeDirection& previousEdge, const RecordOptions& options,
                      const ProfilerCallbacks* callbacks) {
    if (!options.debugRegions || !regions.gameArea.valid())
        return;
    const ScreenPoint cursor{event.cursorX, event.cursorY};
    if (options.verbose &&
        (event.type == RawEventType::MouseLeftDown ||
         event.type == RawEventType::MouseLeftUp)) {
        std::ostringstream line;
        line << (event.type == RawEventType::MouseLeftDown ? "LEFT_DOWN" : "LEFT_UP  ")
             << "  x=" << event.cursorX << " y=" << event.cursorY
             << " region=" << screenRegionName(classifyScreenRegion(regions, cursor));
        if (!options.quiet)
            std::cout << line.str() << '\n';
        emitDiagnostic(callbacks, line.str());
    }
    if (event.type == RawEventType::MouseMove) {
        const auto edge = edgeDirectionAt(regions.gameArea, edgeMarginPx, cursor);
        if (edge != previousEdge && edge != EdgeDirection::None) {
            std::ostringstream line;
            line << "CURSOR_EDGE x=" << event.cursorX << " y=" << event.cursorY
                 << " edge=" << edgeDirectionName(edge);
            if (!options.quiet)
                std::cout << line.str() << '\n';
            emitDiagnostic(callbacks, line.str());
        }
        previousEdge = edge;
    } else if (event.type == RawEventType::ForegroundLost) {
        previousEdge = EdgeDirection::None;
    }
}

RecordingSessionResult runRecordingSession(const std::filesystem::path& workingDirectory, Config config,
                                           const std::vector<std::string>& arguments, bool showSummary,
                                           MacroHotkeyProfile macroHotkeys,
                                           const ProfilerCallbacks* callbacks = nullptr,
                                           const std::function<void()>& captureStarted = {}) {
    const auto options = parseRecordOptions(arguments);
    QpcClock clock;
    CapturedEventQueue queue;
    Collector collector(queue, config.starcraftProcess, clock);
    if (!collector.start())
        throw std::runtime_error(collector.error());
    if (captureStarted) captureStarted();
    ScreenRegionDebugOverlay regionOverlay;
    bool overlayAvailable = !options.debugRegions;
    if (options.debugRegions) {
        try {
            overlayAvailable = regionOverlay.start(
                OverlayCapturePolicy::Capturable);
        } catch (...) {
            overlayAvailable = false;
        }
    }
    if (options.debugRegions) {
        const std::string overlayStatus =
            overlayAvailable ? "REGION_OVERLAY capture_policy=capturable"
                             : "REGION_OVERLAY unavailable";
        if (!options.quiet)
            std::cout << overlayStatus << '\n';
        emitDiagnostic(callbacks, overlayStatus);
    }

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
    CapturedGeometry capturedGeometry;
    const auto& activeRegions = capturedGeometry.regions();
    std::optional<ScreenRect> announcedGameArea;
    std::optional<StarcraftDisplayMode> announcedDisplayMode;
    std::optional<ScreenRegions> previousFocusRegions;
    EdgeDirection previousDebugEdge = EdgeDirection::None;
    bool awaitingGeometry = false;
    bool activeTimelineAnchored = false;

    if (!options.quiet) {
        if (options.debugNavigation && options.debugRegions) {
            std::cout << "\nLIVE DETECTION DEBUG MODE\n\n"
                      << "This shows detected camera actions, the screen-region overlay, edge scrolling,\n"
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
    notifyStatus(callbacks, ProfilerActivity::WaitingForStarCraft,
                 "Waiting for StarCraft to become active");

    const auto applyEventGeometry = [&](const CapturedInputEvent& captured) {
        const bool focusRegained = awaitingGeometry;
        const bool regionsChanged = capturedGeometry.apply(captured, config, analyzer);
        if (regionsChanged)
            previousDebugEdge = EdgeDirection::None;
        if (!captured.screenRegions) {
            if (regionsChanged && options.debugRegions && overlayAvailable)
                regionOverlay.hide();
            return;
        }
        const auto* selected = &activeRegions;
        const auto& resolved = capturedGeometry.minimap();
        if (!announcedDisplayMode ||
            *announcedDisplayMode != selected->displayMode) {
            std::string displayModeDiagnostic =
                std::string("DISPLAY_MODE ") +
                starcraftDisplayModeName(selected->displayMode);
            if (selected->displayMode == StarcraftDisplayMode::Unknown)
                displayModeDiagnostic += " fallback=original_aspect";
            if (!options.quiet)
                std::cout << displayModeDiagnostic << '\n';
            emitDiagnostic(callbacks, displayModeDiagnostic);
            announcedDisplayMode = selected->displayMode;
        }
        if (regionsChanged) {
            std::ostringstream diagnostic;
            diagnostic << "REGIONS_UPDATED minimap_source="
                       << minimapRegionSourceName(resolved.source);
            if (selected->minimap.valid()) {
                diagnostic << " rect=(" << selected->minimap.left << ','
                           << selected->minimap.top << ")->("
                           << selected->minimap.right << ','
                           << selected->minimap.bottom << ')';
            }
            emitDiagnostic(callbacks, diagnostic.str());
            if (options.debugRegions && overlayAvailable &&
                captured.event.type != RawEventType::ForegroundLost) {
                regionOverlay.update(makeScreenRegionOverlayModel(
                    *selected, resolved, config.edgeMarginPx, true));
            }
        }
        const bool changed = !announcedGameArea ||
                             !sameRect(*announcedGameArea, selected->gameArea) ||
                             regionsChanged;
        if (changed && !options.quiet)
            printRegionDiagnostics(*selected, resolved.source, config.edgeMarginPx);
        if (focusRegained && previousFocusRegions && options.debugRegions &&
            !options.quiet)
            printFocusGeometry(*previousFocusRegions, *selected);
        previousFocusRegions = *selected;
        announcedGameArea = selected->gameArea;
        awaitingGeometry = false;
    };

    const auto consume = [&](const CapturedInputEvent& captured) {
        const auto& event = captured.event;
        writer.submitRaw(event);

        if (event.type == RawEventType::ForegroundGained) {
            if (!activeTimelineAnchored) {
                writer.setActiveTimelineAnchor(clock.wallClockAnchorAt(event.timestampTicks));
                activeTimelineAnchored = true;
            }
            if (options.debugRegions && overlayAvailable)
                regionOverlay.hide();
            awaitingGeometry = true;
        } else if (event.type == RawEventType::ForegroundLost) {
            awaitingGeometry = false;
            if (options.debugRegions && overlayAvailable)
                regionOverlay.hide();
        }
        applyEventGeometry(captured);
        if (event.type == RawEventType::ForegroundGained && awaitingGeometry && !options.quiet)
            std::cout << "StarCraft geometry is not ready yet. Retrying automatically...\n";

        printRegionDebug(event, activeRegions, config.edgeMarginPx, previousDebugEdge, options,
                         callbacks);
        if (options.showRaw && !options.quiet) {
            std::cout << "RAW " << event.sequence << " type=" << static_cast<int>(event.type)
                      << " vk=" << event.virtualKey << " cursor=(" << event.cursorX << ',' << event.cursorY << ")\n";
        }
        analyzer.process(event);
        drainAnalyzerDebug(analyzer, options, callbacks);
    };

    CollectorState announced = CollectorState::Waiting;
    while (recordingRequested.load(std::memory_order_acquire)) {
        bool consumed = false;
        CapturedInputEvent event{};
        while (queue.tryPop(event)) {
            consumed = true;
            consume(event);
        }
        const auto state = collector.state();
        if (state != announced) {
            if (state == CollectorState::Recording) {
                if (!options.quiet)
                    std::cout << (announced == CollectorState::Paused
                                      ? "StarCraft active. Recording resumed.\n"
                                      : "StarCraft detected. Recording.\n");
                notifyStatus(callbacks, ProfilerActivity::Recording,
                             "StarCraft active; recording input");
            } else if (state == CollectorState::Paused) {
                if (!options.quiet)
                    std::cout << "StarCraft inactive. Recording paused.\n";
                notifyStatus(callbacks, ProfilerActivity::Paused,
                             "StarCraft is not foreground; recording paused");
            }
            announced = state;
        }
        if (!consumed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    collector.stop();
    regionOverlay.stop();
    CapturedInputEvent event{};
    while (queue.tryPop(event))
        consume(event);

    analyzer.reconcileCollectorDrops(collector.droppedEvents());
    analyzer.finalize(clock.now(), collector.droppedEvents() + writer.droppedEvents());
    drainAnalyzerDebug(analyzer, options, callbacks);
    writer.stop();
    if (writer.failed())
        throw std::runtime_error("Session storage failed while writing the optional raw event file");
    analyzer.setDroppedEventCount(collector.droppedEvents() + writer.droppedEvents());
    RecordingSessionResult completed;
    completed.analysis = analyzer.result();
    completed.macroHotkeys = std::move(macroHotkeys);
    completed.qpcFrequency = clock.frequency();
    completed.sessionId = writer.sessionId();
    completed.navPath = writer.writeNavigation(completed.analysis);
    if (writer.rawEnabled())
        completed.rawPath = writer.rawPath();
    (void)showSummary;
    return completed;
}

ReplayExtractionResult waitForSettledReplay(const std::filesystem::path& replayPath,
                                            const ReplayMetadata& observedChange) {
    ReplayReadinessHooks hooks;
    hooks.now = []() { return std::chrono::steady_clock::now(); };
    hooks.readMetadata = [&]() { return readReplayMetadata(replayPath); };
    hooks.readable = [&]() {
        std::ifstream input(replayPath, std::ios::binary);
        char byte{};
        return input.read(&byte, 1).gcount() == 1;
    };
    hooks.parse = [&](std::chrono::milliseconds timeout) {
        return extractReplayWithBundledScrep(replayPath, timeout);
    };
    hooks.wait = [](std::chrono::milliseconds duration) {
        std::this_thread::sleep_for(duration);
    };
    return waitForReplayReadiness(observedChange, hooks);
}

void markReplayUnavailable(ProductionAnalysis& production, const ReplayExtractionResult& extraction) {
    production.replayCorrelation = {};
    production.replayCorrelation.unavailableReason = extraction.unavailableReason;
    production.replayCorrelation.parser = extraction.parser;
    production.replayCorrelation.unmatchedProductionVisits = production.productionVisits.size();
    production.workerMacroCycles = {};
    production.workerMacroCycles.productType = MacroProductType::Worker;
    production.workerMacroCycles.unavailableReason = extraction.unavailableReason;
    production.armyMacroCycles = {};
    production.armyMacroCycles.productType = MacroProductType::Army;
    production.armyMacroCycles.unavailableReason = extraction.unavailableReason;
    production.armyControlGroupManagement.available = false;
    production.armyControlGroupManagement.unavailableReason = extraction.unavailableReason;
    production.armyCommandActivity = {};
    production.armyCommandActivity.unavailableReason = extraction.unavailableReason;
    production.abilityActivity = {};
    production.abilityActivity.unavailableReason = extraction.unavailableReason;
}

json::Value finalizeDerivedAnalysis(RecordingSessionResult& completed,
                                    const std::optional<ReplayExtractionResult>& replay = std::nullopt) {
    completed.production = analyzeProductionVisits(completed.analysis, completed.macroHotkeys,
                                                   completed.qpcFrequency);
    if (replay) {
        if (replay->available) {
            try {
                completed.production = correlateProductionVisitsWithReplay(
                    completed.analysis, completed.macroHotkeys, completed.qpcFrequency,
                    completed.production, replay->replay, replay->parser);
            } catch (const std::exception& error) {
                ReplayExtractionResult correlationFailure;
                correlationFailure.parser = replay->parser;
                correlationFailure.unavailableReason =
                    std::string("Replay correlation failed: ") + error.what();
                markReplayUnavailable(completed.production, correlationFailure);
            } catch (...) {
                ReplayExtractionResult correlationFailure;
                correlationFailure.parser = replay->parser;
                correlationFailure.unavailableReason = "Replay correlation failed";
                markReplayUnavailable(completed.production, correlationFailure);
            }
        } else {
            markReplayUnavailable(completed.production, *replay);
        }
    }
    const auto analysisJson = analysisToJson(completed.analysis, completed.sessionId,
                                             completed.production, completed.macroHotkeys);
    completed.jsonPath = writeAnalysisJson(completed.navPath, analysisJson);
    return analysisJson;
}

int record(const std::filesystem::path& workingDirectory, Config config,
           const std::vector<std::string>& arguments,
           const ProfilerCallbacks* callbacks = nullptr) {
    auto macroHotkeys = loadStarCraftHotkeyProfile();
    std::optional<std::filesystem::path> lastReplayPath;
    ReplayMetadata replayBaseline;
    try {
        lastReplayPath = defaultLastReplayPath();
        replayBaseline = readReplayMetadata(*lastReplayPath);
    } catch (...) {
        lastReplayPath.reset();
    }
    automaticRequested.store(false, std::memory_order_release);
    recordingRequested.store(true, std::memory_order_release);
    ConsoleHandlerRegistration consoleHandlerRegistration;
    try {
        auto completed = runRecordingSession(workingDirectory, std::move(config), arguments, true,
                                             std::move(macroHotkeys), callbacks);
        std::optional<ReplayExtractionResult> replay;
        if (lastReplayPath) {
            const auto current = readReplayMetadata(*lastReplayPath);
            if (replayMetadataChanged(replayBaseline, current))
                replay = waitForSettledReplay(*lastReplayPath, current);
        }
        notifyStatus(callbacks, ProfilerActivity::AnalyzingReplay,
                     "Finalizing replay-backed analysis");
        const auto analysisJson = finalizeDerivedAnalysis(completed, replay);
        if (callbacks && callbacks->gameCompleted) {
            AutomaticSessionStats oneGame =
                automaticSessionStatsForGame(completed.analysis, completed.production);
            callbacks->gameCompleted(analysisJson, completed.jsonPath, oneGame);
        }
        if (!parseRecordOptions(arguments).quiet)
            printSummary(analysisJson, completed.navPath);
        recordingRequested.store(false, std::memory_order_release);
        return 0;
    } catch (...) {
        recordingRequested.store(false, std::memory_order_release);
        throw;
    }
}

enum class AutomaticEventType {
    MinimapViewportDetected,
    ReplayPlaybackSuppressed,
    MinimapRearmed,
    LastReplayChanged,
    RecorderEnded,
};

struct AutomaticEvent {
    AutomaticEventType type{};
    ReplayMetadata replay;
    std::uint64_t generation{};
    std::chrono::steady_clock::time_point detectedAt{std::chrono::steady_clock::now()};
};

int automaticRecord(const std::filesystem::path& workingDirectory, Config config,
                     const std::vector<std::string>& arguments, bool controlledByMenu = false,
                     const std::function<void()>& readyCallback = {},
                     const ProfilerCallbacks* callbacks = nullptr,
                     ReportVisibilityProvider currentReportVisibility = {}) {
    auto recorderArguments = arguments;
    if (controlledByMenu)
        recorderArguments.push_back("--quiet");
    (void)parseRecordOptions(recorderArguments);

    const auto lastReplayPath = defaultLastReplayPath();
    AutomaticCaptureCoordinator capture;
    std::mutex eventMutex;
    std::condition_variable eventReady;
    std::deque<AutomaticEvent> events;
    // Monitors are destroyed before the event queue they call into.
    LastReplayWatcher replayWatcher;
    MinimapStartMonitor startMonitor(config.starcraftProcess,
                                     config.originalAspectMinimapMode,
                                     config.widescreenMinimapMode,
                                     config.calibratedMinimap,
                                     config.widescreenCalibratedMinimap,
                                     !controlledByMenu);
    std::thread recorderThread;
    std::optional<RecordingSessionResult> recorderResult;
    std::exception_ptr recorderFailure;
    // Only the finalizer touches this state until stopAndDrain has joined it.
    AutomaticSessionState sessionStats;
    const auto sessionHistoryPath =
        makeSeparatedAutomaticSessionSummaryPath(workingDirectory / "sessions");
    std::chrono::steady_clock::time_point recorderStoppedAt;

    const auto diagnostic = [&](std::string message) {
        if (!controlledByMenu) std::osyncstream(std::cout) << message << '\n';
        emitDiagnostic(callbacks, message);
    };
    const auto enqueue = [&](AutomaticEvent event) {
        {
            std::scoped_lock lock(eventMutex);
            events.push_back(std::move(event));
        }
        eventReady.notify_one();
    };
    const auto startMinimapDetector = [&](MinimapDetectorState initialState) {
        return startMonitor.start([&]() { enqueue({AutomaticEventType::MinimapViewportDetected}); }, initialState,
            [&](MinimapMonitorObservation observation) {
                enqueue({observation == MinimapMonitorObservation::ReplayPlaybackSuppressed
                             ? AutomaticEventType::ReplayPlaybackSuppressed : AutomaticEventType::MinimapRearmed});
            });
    };
    AutomaticFinalizationWorker finalizer([&](AutomaticFinalizationJob job) {
        if (job.aborted) {
            (void)sessionStats.markAbortedGeneration(job.generation);
            if (job.recording) {
                const auto discarded = discardAbortedAutomaticRecordingFiles(
                    {job.recording->navPath, job.recording->jsonPath, job.recording->rawPath});
                if (!discarded.failedPaths.empty())
                    diagnostic("Warning: unable to remove " + std::to_string(discarded.failedPaths.size()) +
                               " incomplete recording file(s), generation=" + std::to_string(job.generation));
                diagnostic("Incomplete recording discarded, generation=" + std::to_string(job.generation));
            }
            return;
        }
        if (!job.recording) {
            throw std::runtime_error("Completed recorder did not produce a result");
        }
        auto& completed = *job.recording;
        auto snapshotPath = completed.navPath;
        snapshotPath.replace_extension(".finalizing.rep");
        const auto replay = waitForPinnedReplayReadiness(job.replaySource, snapshotPath);
        if (!replay.available)
            diagnostic("REPLAY_UNAVAILABLE generation=" + std::to_string(job.generation) +
                       " reason=" + replay.unavailableReason);
        const auto analysisJson = finalizeDerivedAnalysis(completed, replay);
        if (!sessionStats.addFinalizedGame(job.generation, completed.analysis, completed.production))
            return;
        if (callbacks && callbacks->gameCompleted)
            callbacks->gameCompleted(analysisJson, completed.jsonPath, sessionStats.stats());
        printAutomaticSessionReport(sessionStats);
        bool historyPersisted = false;
        try {
            writeSeparatedAutomaticSessionHistory(sessionHistoryPath, sessionStats, currentReportVisibility);
            historyPersisted = true;
        } catch (const std::exception& error) {
            diagnostic(std::string("Warning: unable to save automatic session summary: ") + error.what());
        }
        if (callbacks && callbacks->sessionUpdated)
            callbacks->sessionUpdated(sessionStats.stats());
        if (canRunNavRetention({true, !completed.jsonPath.empty(), historyPersisted})) {
            auto retentionPolicy = config.navRetention;
            try {
                retentionPolicy = Config::loadOrCreate(workingDirectory / "config.json").navRetention;
            } catch (...) {
            }
            const auto retention = recordFinalizedAutomaticNavAndApplyRetention(
                workingDirectory / "sessions", completed.navPath, completed.jsonPath,
                sessionHistoryPath, retentionPolicy);
            if (!retention.warning.empty()) diagnostic("Warning: " + retention.warning);
            if (!retention.cleanup.failedPaths.empty())
                diagnostic("Warning: unable to remove " + std::to_string(retention.cleanup.failedPaths.size()) +
                           " older navigation session file(s).");
        }
    }, [&](std::string message) {
        diagnostic(std::move(message) + " active_generation=" + std::to_string(capture.activeGeneration()));
    });

    const auto finishRecorder = [&]() -> std::optional<RecordingSessionResult> {
        recordingRequested.store(false, std::memory_order_release);
        replayWatcher.stop();
        if (recorderThread.joinable()) recorderThread.join();
        recorderStoppedAt = std::chrono::steady_clock::now();
        if (capture.activeGeneration() != 0)
            diagnostic("AUTO_GAME_ENDED generation=" + std::to_string(capture.activeGeneration()));
        auto finished = std::move(recorderResult);
        recorderResult.reset();
        const auto failure = std::exchange(recorderFailure, {});
        if (failure) std::rethrow_exception(failure);
        return finished;
    };
    const auto cleanup = [&]() {
        startMonitor.stop();
        automaticRequested.store(false, std::memory_order_release);
        capture.abort(finishRecorder, finalizer);
        finalizer.stopAndDrain();
    };

    automaticRequested.store(true, std::memory_order_release);
    recordingRequested.store(false, std::memory_order_release);
    ConsoleHandlerRegistration consoleHandlerRegistration;
    try {
        if (!startMinimapDetector(MinimapDetectorState::WaitForAppearance))
            throw std::runtime_error("Unable to start the minimap detector.");
        if (controlledByMenu) {
            std::cout << "\nWaiting for user to enter game...\n";
        } else {
            std::cout << "\nAUTOMATIC GAME RECORDING\n\n"
                      << "Waiting for the minimap viewport outline.\n"
                      << "Recording starts after two consecutive detections and stops when LastReplay.rep changes.\n"
                      << "Keep this window open. Press Ctrl+C here to leave automatic mode.\n\n"
                      << "LastReplay: " << lastReplayPath.string() << '\n';
        }
        if (readyCallback) readyCallback();
        notifyStatus(callbacks, ProfilerActivity::WaitingForGame, "Waiting for the minimap viewport outline");

        while (automaticRequested.load(std::memory_order_acquire)) {
            AutomaticEvent event;
            {
                std::unique_lock lock(eventMutex);
                eventReady.wait_for(lock, std::chrono::milliseconds(250), [&]() {
                    return !events.empty() || !automaticRequested.load(std::memory_order_acquire);
                });
                if (!automaticRequested.load(std::memory_order_acquire)) break;
                if (events.empty()) continue;
                event = std::move(events.front());
                events.pop_front();
            }
            if (event.type == AutomaticEventType::ReplayPlaybackSuppressed ||
                event.type == AutomaticEventType::MinimapRearmed) {
                if (capture.activeGeneration() == 0)
                    notifyStatus(callbacks, ProfilerActivity::WaitingForGame,
                        event.type == AutomaticEventType::ReplayPlaybackSuppressed
                            ? "Replay playback detected; waiting for a live game" : "Waiting for game");
                continue;
            }
            if (event.type == AutomaticEventType::MinimapViewportDetected) {
                if (capture.activeGeneration() != 0) continue;
                startMonitor.stop();
                const auto baseline = readReplayMetadata(lastReplayPath);
                capture.tryStart(baseline, [&](std::uint64_t generation) {
                    if (!controlledByMenu) {
                        std::osyncstream(std::cout) << "\nLASTREPLAY_BASELINE\n"
                            << "path=" << lastReplayPath.string() << '\n'
                            << "writeTimeUtc=" << formatReplayWriteTimeUtc(baseline) << '\n'
                            << "size=" << (baseline.exists ? std::to_string(baseline.size) : "missing") << "\n\n";
                    }
                    // Configuration belongs to the new game, independent of old analysis.
                    auto gameMacroHotkeys = loadStarCraftHotkeyProfile();
                    if (!replayWatcher.start(lastReplayPath, baseline,
                        [&, generation](const ReplayMetadata& current) {
                            enqueue({AutomaticEventType::LastReplayChanged, current, generation});
                        })) throw std::runtime_error(replayWatcher.error());
                    recordingRequested.store(true, std::memory_order_release);
                    notifyStatus(callbacks, ProfilerActivity::Recording, "Game detected; recording input");
                    if (controlledByMenu) std::cout << "\nRecording started...\n";
                    recorderResult.reset();
                    recorderFailure = {};
                    const auto candidateAt = event.detectedAt;
                    recorderThread = std::thread([&, generation, candidateAt,
                                                  gameMacroHotkeys = std::move(gameMacroHotkeys)]() mutable {
                        try {
                            recorderResult = runRecordingSession(workingDirectory, config, recorderArguments, false,
                                std::move(gameMacroHotkeys), callbacks, [&, generation, candidateAt]() {
                                    const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - candidateAt).count();
                                    diagnostic("AUTO_START generation=" + std::to_string(generation) +
                                               " candidate_to_capture_us=" + std::to_string(latency));
                                });
                        } catch (...) {
                            recorderFailure = std::current_exception();
                        }
                        enqueue({AutomaticEventType::RecorderEnded, {}, generation});
                    });
                });
                continue;
            }
            if (event.type == AutomaticEventType::LastReplayChanged) {
                capture.tryFinish(event.generation, event.replay, finishRecorder,
                    finalizer, [&]() {
                        if (!startMinimapDetector(MinimapDetectorState::WaitForAbsence))
                            throw std::runtime_error("Unable to restart the minimap detector");
                        const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - recorderStoppedAt).count();
                        diagnostic("AUTO_DETECTOR_REARMED generation_after=" + std::to_string(event.generation) +
                                   " stop_to_detector_us=" + std::to_string(latency));
                        notifyStatus(callbacks, ProfilerActivity::WaitingForGame, "Waiting for the next game");
                    }, [&]() { return PinnedReplaySource::open(lastReplayPath); });
                continue;
            }
            if (event.type == AutomaticEventType::RecorderEnded &&
                event.generation == capture.activeGeneration()) {
                // Joined recorder failures are surfaced only for their active generation.
                capture.abort(finishRecorder, finalizer);
                throw std::runtime_error("The recorder stopped unexpectedly during automatic recording");
            }
        }
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
    printAutomaticSessionReport(sessionStats); // Worker is joined; ownership returns here.
    std::cout << "\nAutomatic recording stopped.\n";
    notifyStatus(callbacks, ProfilerActivity::Idle, "Automatic detector is off");
    return 0;
}
json::Value loadNavSummary(const std::filesystem::path& path) {
    auto jsonPath = path;
    jsonPath.replace_extension(".json");
    if (std::filesystem::is_regular_file(jsonPath))
        return json::parseFile(jsonPath);
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
                  << "  2. Calibrate minimap override\n"
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
                const auto path = findLatestSeparatedAutomaticSessionSummary(
                    workingDirectory / "sessions");
                if (!path) {
                    std::cout << "\nNo automatic session summary has been saved yet.\n";
                } else {
                    std::ifstream input(*path, std::ios::binary);
                    if (!input)
                        throw std::runtime_error("Unable to open the latest automatic session summary");
                    std::cout << '\n' << input.rdbuf();
                }
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

int runAutomaticProfiler(const std::filesystem::path& workingDirectory, Config config,
                          const ProfilerCallbacks& callbacks,
                          ReportVisibilityProvider currentReportVisibility) {
    return automaticRecord(workingDirectory, std::move(config), {"auto"}, true, {},
                           &callbacks, std::move(currentReportVisibility));
}

int runDebugProfiler(const std::filesystem::path& workingDirectory, Config config,
                     const ProfilerCallbacks& callbacks) {
    const int result = record(workingDirectory, std::move(config),
                              {"record", "--debug-navigation", "--debug-regions",
                               "--quiet"},
                              &callbacks);
    notifyStatus(&callbacks, ProfilerActivity::Idle, "Live detection stopped");
    return result;
}

void requestAutomaticProfilerStop() noexcept {
    automaticRequested.store(false, std::memory_order_release);
}

void requestRecordingProfilerStop() noexcept {
    recordingRequested.store(false, std::memory_order_release);
}

} // namespace smp
