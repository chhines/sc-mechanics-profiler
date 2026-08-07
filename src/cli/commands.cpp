#include "cli/commands.h"

#include "analysis/analyzer.h"
#include "capture/collector.h"
#include "cli/calibration.h"
#include "cli/report.h"
#include "config/config.h"
#include "platform/clock.h"
#include "storage/session.h"
#include "util/json.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <windows.h>

namespace scm {
namespace {

std::atomic<bool> recordingRequested{false};

BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        recordingRequested.store(false, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

struct RecordOptions {
    bool verbose{};
    bool showRaw{};
    bool showLogical{};
    bool showPacs{};
    bool showMacro{};
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
        else if (argument == "--show-logical-events")
            options.showLogical = true;
        else if (argument == "--show-pacs")
            options.showPacs = true;
        else if (argument == "--show-macro")
            options.showMacro = true;
        else if (argument == "--quiet")
            options.quiet = true;
        else
            throw std::runtime_error("Unknown record option: " + argument);
    }
    return options;
}

bool shouldPrintLogical(const LogicalEvent& event, const RecordOptions& options) {
    if (options.showLogical)
        return true;
    if (options.showPacs &&
        (event.type == LogicalEventType::InferredPacStart || event.type == LogicalEventType::InferredPacFirstAction ||
         event.type == LogicalEventType::InferredPacEnd))
        return true;
    if (options.showMacro &&
        (event.type == LogicalEventType::MacroWorkerAttempt || event.type == LogicalEventType::MacroArmyAttempt ||
         event.type == LogicalEventType::MacroEpisodeStart || event.type == LogicalEventType::MacroEpisodeEnd))
        return true;
    return false;
}

int record(const std::filesystem::path& workingDirectory, const Config& config,
           const std::vector<std::string>& arguments) {
    const auto options = parseRecordOptions(arguments);
    QpcClock clock;
    RawEventQueue queue;
    Collector collector(queue, config.starcraftProcess, clock);
    if (!collector.start())
        throw std::runtime_error(collector.error());

    SessionWriter writer(workingDirectory / "sessions", clock.frequency(), config.writeLogicalEvents,
                         config.flushIntervalMs);
    Analyzer analyzer(config, clock.frequency());
    recordingRequested.store(true, std::memory_order_release);
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    if (!options.quiet) {
        std::cout << "Waiting for StarCraft...\n\n"
                  << "Press Ctrl+C to stop.\n";
        if (options.verbose)
            std::cout << "Session: " << writer.sessionId() << '\n';
    }

    CollectorState announced = CollectorState::Waiting;
    while (recordingRequested.load(std::memory_order_acquire)) {
        bool consumed = false;
        RawInputEvent event{};
        while (queue.tryPop(event)) {
            consumed = true;
            writer.submitRaw(event);
            if (options.showRaw && !options.quiet) {
                std::cout << "RAW " << event.sequence << " type=" << static_cast<int>(event.type)
                          << " vk=" << event.virtualKey << " cursor=(" << event.cursorX << ',' << event.cursorY
                          << ")\n";
            }
            analyzer.process(event);
            for (const auto& logical : analyzer.takeEmittedEvents()) {
                writer.submitLogical(logical);
                if (shouldPrintLogical(logical, options) && !options.quiet) {
                    std::cout << logicalEventName(logical.type) << " source=" << logical.sourceSequence
                              << " data=" << logical.data1 << '\n';
                }
            }
        }
        const auto state = collector.state();
        if (state != announced && !options.quiet) {
            if (state == CollectorState::Recording) {
                std::cout << (announced == CollectorState::Paused ? "StarCraft active. Recording resumed.\n"
                                                                  : "StarCraft detected.\nRecording.\n");
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
    while (queue.tryPop(event)) {
        writer.submitRaw(event);
        analyzer.process(event);
        for (const auto& logical : analyzer.takeEmittedEvents())
            writer.submitLogical(logical);
    }
    analyzer.finalize(clock.now(), collector.droppedEvents() + writer.droppedEvents());
    for (const auto& logical : analyzer.takeEmittedEvents())
        writer.submitLogical(logical);
    writer.stop();
    if (writer.failed())
        throw std::runtime_error("Session storage failed while writing event data");
    analyzer.setDroppedEventCount(collector.droppedEvents() + writer.droppedEvents());
    writeSessionSummary(writer.directory(), analyzer.result(), writer.sessionId());

    SetConsoleCtrlHandler(consoleHandler, FALSE);
    if (!options.quiet)
        printSummary(analysisToJson(analyzer.result(), writer.sessionId()), writer.directory());
    return 0;
}

json::Value load(const std::filesystem::path& path) {
    return json::parseFile(path);
}

int summaryCommand(const std::filesystem::path& sessionsRoot, const std::vector<std::string>& arguments) {
    if (arguments.size() != 2)
        throw std::runtime_error("Usage: scmechanics summary <latest|session-id>");
    const auto path = resolveSessionSummary(sessionsRoot, arguments[1]);
    printSummary(load(path), path.parent_path());
    return 0;
}

int compareCommand(const std::filesystem::path& sessionsRoot, const std::vector<std::string>& arguments) {
    if (arguments.size() != 3)
        throw std::runtime_error("Usage: scmechanics compare <session-id> <session-id> | compare last <N>");
    if (arguments[1] == "last") {
        const int count = std::stoi(arguments[2]);
        if (count <= 0)
            throw std::runtime_error("N must be positive");
        const auto paths = listSessionSummaries(sessionsRoot);
        if (paths.size() < 2)
            throw std::runtime_error("At least two sessions are required for comparison");
        std::vector<json::Value> baselines;
        const auto available = std::min<std::size_t>(static_cast<std::size_t>(count), paths.size() - 1);
        for (std::size_t i = paths.size() - 1 - available; i < paths.size() - 1; ++i)
            baselines.push_back(load(paths[i]));
        printComparison(load(paths.back()), baselines);
        return 0;
    }
    const auto current = resolveSessionSummary(sessionsRoot, arguments[1]);
    const auto baseline = resolveSessionSummary(sessionsRoot, arguments[2]);
    printComparison(load(current), {load(baseline)});
    return 0;
}

} // namespace

void printUsage() {
    std::cout << "scmechanics " << SCMECHANICS_VERSION << " - real-time StarCraft: Remastered mechanical profiler\n\n"
              << "Usage:\n"
              << "  scmechanics record [--verbose] [--show-raw-events] [--show-logical-events]\n"
              << "                     [--show-pacs] [--show-macro] [--quiet]\n"
              << "  scmechanics calibrate\n"
              << "  scmechanics config\n"
              << "  scmechanics summary <latest|session-id>\n"
              << "  scmechanics compare <session-id> <session-id>\n"
              << "  scmechanics compare last <N>\n"
              << "  scmechanics export <latest|session-id> --csv\n";
}

int runCommand(const std::vector<std::string>& arguments, const std::filesystem::path& workingDirectory) {
    if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "-h" || arguments[0] == "help") {
        printUsage();
        return 0;
    }
    const auto configPath = workingDirectory / "config.json";
    Config config = Config::loadOrCreate(configPath);
    if (arguments[0] == "record")
        return record(workingDirectory, config, arguments);
    if (arguments[0] == "calibrate")
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
            throw std::runtime_error("Usage: scmechanics export <latest|session-id> --csv");
        const auto path = exportSessionCsv(workingDirectory / "sessions", workingDirectory / "exports", arguments[1]);
        std::cout << "Exported: " << path.string() << '\n';
        return 0;
    }
    throw std::runtime_error("Unknown command: " + arguments[0]);
}

} // namespace scm
