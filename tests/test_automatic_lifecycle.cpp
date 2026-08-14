#include "test_framework.h"

#include "platform/automatic_lifecycle.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

TEST_CASE("automatic lifecycle suppresses duplicate starts and stops") {
    smp::AutomaticLifecycleState lifecycle;
    const smp::ReplayMetadata baseline{true, 100, 200};
    REQUIRE(lifecycle.state() == smp::AutomaticRecordingState::Idle);
    REQUIRE(lifecycle.tryStart(baseline));
    REQUIRE(!lifecycle.tryStart(baseline));
    REQUIRE(lifecycle.state() == smp::AutomaticRecordingState::Recording);
    REQUIRE(!lifecycle.tryStop(baseline));
    REQUIRE(!lifecycle.tryStop({false, 0, 0}));
    REQUIRE(lifecycle.tryStop({true, 101, 200}));
    REQUIRE(!lifecycle.tryStop({true, 102, 201}));
    REQUIRE(lifecycle.state() == smp::AutomaticRecordingState::Idle);
    REQUIRE(!lifecycle.baseline().has_value());
}

TEST_CASE("a LastReplay creation changes a missing baseline") {
    smp::AutomaticLifecycleState lifecycle;
    REQUIRE(lifecycle.tryStart({}));
    REQUIRE(!lifecycle.tryStop({}));
    REQUIRE(lifecycle.tryStop({true, 500, 20}));
}

TEST_CASE("minimap start monitor starts in automatic mode without calibration") {
    smp::MinimapStartMonitor monitor(
        L"StarcraftMechanicsProfilerMissingProcess.exe",
        smp::MinimapMode::Automatic, std::nullopt, false);
    REQUIRE(monitor.start([]() {}));
    monitor.stop();
}

TEST_CASE("LastReplay watcher reacts once to a genuine metadata change") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root =
        std::filesystem::temp_directory_path() / ("starcraft-last-replay-watcher-" + std::to_string(nonce));
    const auto path = root / "LastReplay.rep";
    std::filesystem::create_directories(root);
    {
        std::ofstream file(path, std::ios::binary);
        file << 'a';
    }
    const auto baseline = smp::readReplayMetadata(path);
    REQUIRE(baseline.exists);

    std::atomic<int> callbacks{0};
    smp::ReplayMetadata observed;
    smp::LastReplayWatcher watcher;
    REQUIRE(watcher.start(path, baseline, [&](const smp::ReplayMetadata& metadata) {
        observed = metadata;
        callbacks.fetch_add(1, std::memory_order_release);
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file << "bc";
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (callbacks.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    watcher.stop();

    REQUIRE(callbacks.load(std::memory_order_acquire) == 1);
    REQUIRE(observed.exists);
    REQUIRE(observed.size != baseline.size || observed.writeTimeUtc != baseline.writeTimeUtc);
    std::filesystem::remove_all(root);
}
