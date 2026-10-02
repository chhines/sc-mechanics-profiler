#include "test_framework.h"

#include "cli/automatic_recording.h"
#include "cli/replay_snapshot.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Gate {
    std::mutex mutex;
    std::condition_variable ready;
    bool released{};
    void wait() {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&]() { return released; });
    }
    void release() {
        { std::scoped_lock lock(mutex); released = true; }
        ready.notify_all();
    }
};

// This fixture uses the production coordinator, FIFO worker and detector callback
// gate. Each launch creates a real recorder thread; only the OS input source is fake.
struct CaptureHarness {
    smp::AutomaticCaptureCoordinator capture;
    Gate finalizationGate;
    std::promise<void> finalizerEntered;
    std::future<void> entered{finalizerEntered.get_future()};
    std::atomic<int> order{};
    std::atomic<int> live{};
    std::atomic<int> maximumLive{};
    std::vector<std::uint64_t> starts;
    std::vector<std::uint64_t> finalized;
    std::vector<int> captureOrders;
    int finalizerStartOrder{};
    int finalizerFinishOrder{};
    std::vector<std::string> diagnostics;
    std::mutex diagnosticMutex;
    bool failFirst{};
    smp::AutomaticFinalizationWorker worker;
    std::optional<smp::MinimapStartConfirmation> detector;
    Gate recorderGate;
    std::thread recorder;
    Clock::time_point stoppedAt;
    std::chrono::microseconds rearmLatency{};
    std::chrono::microseconds candidateLatency{};

    explicit CaptureHarness(bool fail = false)
        : failFirst(fail), worker([&](smp::AutomaticFinalizationJob job) {
            if (job.generation == 1) {
                finalizerStartOrder = ++order;
                finalizerEntered.set_value();
                finalizationGate.wait();
                finalizerFinishOrder = ++order;
                if (failFirst) throw std::runtime_error("test analysis failure");
            }
            finalized.push_back(job.generation);
        }, [&](std::string line) {
            std::scoped_lock lock(diagnosticMutex);
            diagnostics.push_back(std::move(line));
        }) {}

    ~CaptureHarness() {
        // Always release the test gate, even if a REQUIRE throws.
        finalizationGate.release();
        stopRecorder();
        worker.stopAndDrain();
    }

    std::optional<smp::RecordingSessionResult> stopRecorder() {
        recorderGate.release();
        if (recorder.joinable()) recorder.join();
        stoppedAt = Clock::now();
        return smp::RecordingSessionResult{};
    }

    bool start(const smp::ReplayMetadata& baseline = {}) {
        const auto candidateAt = Clock::now();
        return capture.tryStart(baseline, [&](std::uint64_t generation) {
            detector.reset();
            { std::scoped_lock lock(recorderGate.mutex); recorderGate.released = false; }
            std::promise<void> started;
            auto ready = started.get_future();
            recorder = std::thread([&, generation, candidateAt, started = std::move(started)]() mutable {
                const int count = ++live;
                maximumLive.store(std::max(maximumLive.load(), count));
                starts.push_back(generation);
                captureOrders.push_back(++order);
                candidateLatency = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - candidateAt);
                started.set_value();
                recorderGate.wait();
                --live;
            });
            // Synchronizes the actual recorder startup with the lifecycle fixture.
            ready.wait();
        });
    }

    bool finish(std::uint64_t generation, smp::ReplayMetadata changed) {
        return capture.tryFinish(generation, changed, [&]() { return stopRecorder(); }, worker, [&]() {
            detector.emplace(smp::MinimapDetectorState::WaitForAbsence);
            rearmLatency = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - stoppedAt);
        });
    }

    void observe(bool present, bool replay = false) {
        if (!detector) return;
        const auto result = detector->observe(present);
        smp::dispatchMinimapStart(result, [=]() { return std::optional<bool>{replay}; }, [&]() { start(); });
    }

    bool finalizationStarted() { return entered.wait_for(std::chrono::seconds(3)) == std::future_status::ready; }
};

} // namespace

TEST_CASE("next recorder starts while previous finalization is blocked and observes absence live") {
    CaptureHarness test;
    REQUIRE(test.start());
    REQUIRE(test.finish(1, {true, 100, 200}));
    REQUIRE(test.finalizationStarted());
    REQUIRE(test.detector->state() == smp::MinimapDetectorState::WaitForAbsence);
    test.observe(false);
    test.observe(false);
    REQUIRE(test.detector->state() == smp::MinimapDetectorState::WaitForAppearance);
    test.observe(true);
    test.observe(true);
    REQUIRE(test.capture.activeGeneration() == 2);
    REQUIRE(test.live == 1);
    REQUIRE(test.starts == std::vector<std::uint64_t>({1, 2}));
    REQUIRE(!test.start()); // No second live recorder.
    REQUIRE(test.finalizerStartOrder < test.captureOrders[1]);
    REQUIRE(test.finalizerFinishOrder == 0); // Still behind the gate.
    test.finalizationGate.release();
    test.worker.stopAndDrain();
    REQUIRE(test.captureOrders[1] < test.finalizerFinishOrder);
    REQUIRE(test.capture.activeGeneration() == 2); // Old completion cannot stop new capture.
    REQUIRE(test.maximumLive == 1);
    std::cout << "[TIMING] blocked finalizer: stop_to_detector_us=" << test.rearmLatency.count()
              << " candidate_to_recorder_us=" << test.candidateLatency.count() << '\n';
}

TEST_CASE("three rapid games retain generations and FIFO backlog while finalizer is blocked") {
    CaptureHarness test;
    REQUIRE(test.start());
    REQUIRE(test.finish(1, {true, 100, 200}));
    REQUIRE(test.finalizationStarted());
    for (int game = 2; game <= 4; ++game) {
        test.observe(false); test.observe(false); test.observe(true); test.observe(true);
        REQUIRE(test.capture.activeGeneration() == static_cast<std::uint64_t>(game));
        REQUIRE(!test.finish(1, {true, 500, 600})); // Stale replay stop ignored.
        if (game < 4) REQUIRE(test.finish(game, {true, static_cast<std::uint64_t>(game * 100), 200}));
    }
    REQUIRE(test.starts == std::vector<std::uint64_t>({1, 2, 3, 4}));
    REQUIRE(test.finalizerFinishOrder == 0);
    test.finalizationGate.release();
    test.worker.stopAndDrain();
    REQUIRE(test.finalized == std::vector<std::uint64_t>({1, 2, 3}));
    REQUIRE(test.capture.activeGeneration() == 4);
    REQUIRE(test.maximumLive == 1);
    REQUIRE(std::any_of(test.diagnostics.begin(), test.diagnostics.end(), [](const auto& line) {
        return line.find("FINALIZATION_BACKLOG") != std::string::npos;
    }));
}

TEST_CASE("finalizer failure stays with old generation and subsequent jobs still drain") {
    CaptureHarness test(true);
    REQUIRE(test.start());
    REQUIRE(test.finish(1, {true, 100, 200}));
    REQUIRE(test.finalizationStarted());
    test.observe(false); test.observe(false); test.observe(true); test.observe(true);
    REQUIRE(test.finish(2, {true, 200, 300}));
    test.observe(false); test.observe(false); test.observe(true); test.observe(true);
    test.finalizationGate.release();
    test.worker.stopAndDrain();
    REQUIRE(test.finalized == std::vector<std::uint64_t>({2}));
    REQUIRE(test.capture.activeGeneration() == 3);
    REQUIRE(test.live == 1);
    REQUIRE(std::any_of(test.diagnostics.begin(), test.diagnostics.end(), [](const auto& line) {
        return line.find("FINALIZATION_FAILED generation=1") != std::string::npos;
    }));
}

TEST_CASE("automatic shutdown stops capture then drains completed and aborted jobs and closes queue") {
    CaptureHarness test;
    REQUIRE(test.start());
    REQUIRE(test.finish(1, {true, 100, 200}));
    REQUIRE(test.finalizationStarted());
    test.observe(false); test.observe(false); test.observe(true); test.observe(true);
    test.capture.abort([&]() { return test.stopRecorder(); }, test.worker);
    REQUIRE(test.live == 0);
    REQUIRE(test.capture.activeGeneration() == 0);
    auto drained = std::async(std::launch::async, [&]() { test.worker.stopAndDrain(); });
    const auto status = drained.wait_for(std::chrono::milliseconds(20));
    test.finalizationGate.release();
    drained.get();
    REQUIRE(status == std::future_status::timeout);
    REQUIRE(test.finalized == std::vector<std::uint64_t>({1, 2}));
    bool rejected = false;
    try { test.worker.enqueue({3, {}, {}, false}); }
    catch (const std::logic_error&) { rejected = true; }
    REQUIRE(rejected);
    test.worker.stopAndDrain(); // Repeated cleanup is safe.
}

TEST_CASE("replay suppression remains one shot during previous game finalization") {
    CaptureHarness test;
    REQUIRE(test.start());
    REQUIRE(test.finish(1, {true, 100, 200}));
    REQUIRE(test.finalizationStarted());
    test.observe(false); test.observe(false); test.observe(true); test.observe(true, true);
    test.observe(true); test.observe(true);
    REQUIRE(test.starts.size() == 1);
    test.observe(false); test.observe(false); test.observe(true); test.observe(true);
    REQUIRE(test.starts.size() == 2);
    REQUIRE(test.capture.activeGeneration() == 2);
}

TEST_CASE("generation replay snapshot survives source overwrite and rejects a newer replay") {
    const auto root = std::filesystem::temp_directory_path() /
        ("smp-generation-snapshot-" + std::to_string(Clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    } cleanup{root};
    const auto source = root / "LastReplay.rep";
    const auto snapshot = root / "game1.rep";
    { std::ofstream file(source, std::ios::binary); file << "game one"; }
    const auto expected = smp::readReplayMetadata(source);
    REQUIRE(smp::snapshotGenerationReplay(source, snapshot, expected).empty());
    { std::ofstream file(source, std::ios::binary); file << "a different game two"; }
    std::ifstream input(snapshot, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE(bytes == "game one");
    const auto failure = smp::snapshotGenerationReplay(source, root / "wrong.rep", expected);
    REQUIRE(failure.find("generation mismatch") != std::string::npos);
    REQUIRE(!std::filesystem::exists(root / "wrong.rep"));
}
