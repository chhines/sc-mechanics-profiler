#include "test_framework.h"
#include "platform/replay_ui_detector.h"
#include "platform/automatic_lifecycle.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>

namespace {
struct Hud {
    int width, height, stride;
    std::vector<std::uint8_t> pixels;
    Hud(int w, int h) : width(w), height(h), stride(width * 4 + 16),
        pixels(static_cast<std::size_t>(stride) * height) {}
    explicit Hud(double scale = 1) : Hud(static_cast<int>(640 * scale), static_cast<int>(168 * scale)) {}
    smp::BgraImageView view() const { return {width, height, stride, pixels}; }
    void pixel(int x, int y, bool green = false, int brightness = 220) {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        const auto i = static_cast<std::size_t>(y) * stride + x * 4;
        pixels[i] = 25; pixels[i + 1] = static_cast<std::uint8_t>(brightness);
        pixels[i + 2] = static_cast<std::uint8_t>(green ? 30 : brightness);
    }
    void rect(int x, int y, int w, int h, bool green) {
        for (int dy = 0; dy < h; ++dy) for (int dx = 0; dx < w; ++dx) pixel(x + dx, y + dy, green);
    }
    void ring(int cx, int cy, int w, int h, bool filled = false, bool square = false, int brightness = 220) {
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const double dx = (x - (w - 1) * 0.5) / (w * 0.5);
            const double dy = (y - (h - 1) * 0.5) / (h * 0.5);
            const double distance = square ? std::max(std::abs(dx), std::abs(dy)) : std::sqrt(dx * dx + dy * dy);
            if (distance <= 1 && (filled || distance >= 0.70))
                pixel(cx - w / 2 + x, cy - h / 2 + y, false, brightness);
        }
    }
    void panel(double scale = 1, int offset = 0, bool controls = true, bool bar = false, bool filled = false,
               bool square = false, int brightness = 220) {
        const int cx = width * 3 / 4 + offset, cy = static_cast<int>(110 * scale);
        const int radius = static_cast<int>(10 * scale), step = static_cast<int>(30 * scale);
        if (bar) rect(cx - static_cast<int>(46 * scale), cy - static_cast<int>(30 * scale),
                      static_cast<int>(92 * scale), std::max(2, static_cast<int>(4 * scale)), true);
        if (!controls) return;
        for (int k = -1; k <= 1; ++k)
            ring(cx + k * step, cy, radius * 2 + 1, radius * 2 + 1, filled, square, brightness);
    }
};
}

TEST_CASE("replay transport signature scales shifts and tolerates brightness and padded rows") {
    for (double scale : {0.75, 1.0, 1.5, 2.0}) for (int shift : {-90, 0, 90}) {
        Hud hud(scale); hud.panel(scale, static_cast<int>(shift * scale), true, false, false, false, 120);
        REQUIRE(smp::containsReplayTransportPanel(hud.view()));
    }
}

TEST_CASE("replay detector accepts rings without progress but rejects filled and square command icons") {
    Hud controls; controls.panel(1, 0, true, false);
    REQUIRE(smp::containsReplayTransportPanel(controls.view()));
    Hud withBar; withBar.panel(1, 0, true, true);
    REQUIRE(smp::containsReplayTransportPanel(withBar.view()));
    Hud bar; bar.panel(1, 0, false, true);
    REQUIRE(!smp::containsReplayTransportPanel(bar.view()));
    Hud filled; filled.panel(1, 0, true, true, true);
    REQUIRE(!smp::containsReplayTransportPanel(filled.view()));
    Hud square; square.panel(1, 0, true, true, false, true);
    REQUIRE(!smp::containsReplayTransportPanel(square.view()));
    Hud commands;
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 3; ++x)
        commands.rect(450 + x * 30, 65 + y * 30, 20, 20, false);
    commands.rect(450, 45, 90, 4, true);
    REQUIRE(!smp::containsReplayTransportPanel(commands.view()));
    Hud unrelated; unrelated.panel(1, 0, true, false); unrelated.rect(20, 15, 100, 4, true);
    REQUIRE(smp::containsReplayTransportPanel(unrelated.view()));
    Hud irregular; irregular.panel(1, 0, true, true);
    // Remove the middle control and replace it off the common row.
    for (int y = 99; y <= 121; ++y) for (int x = 469; x <= 491; ++x) {
        const auto i = static_cast<std::size_t>(y) * irregular.stride + x * 4;
        irregular.pixels[i + 1] = irregular.pixels[i + 2] = 0;
    }
    irregular.ring(480, 145, 21, 21);
    REQUIRE(!smp::containsReplayTransportPanel(irregular.view()));
    REQUIRE(!smp::containsReplayTransportPanel({}));
    REQUIRE(!smp::containsReplayTransportPanel({100, 100, 400, {}}));
}

TEST_CASE("replay detector tolerates noise without promoting noise to replay") {
    Hud noisy;
    std::mt19937 random(10);
    for (int n = 0; n < 1800; ++n)
        noisy.pixel(static_cast<int>(random() % noisy.width), static_cast<int>(random() % noisy.height), random() % 2 != 0);
    noisy.rect(45, 50, 25, 18, false); noisy.rect(500, 35, 55, 3, true);
    REQUIRE(!smp::containsReplayTransportPanel(noisy.view()));
    noisy.panel();
    REQUIRE(smp::containsReplayTransportPanel(noisy.view()));
}

TEST_CASE("real scale replay button approximation needs no progress bar") {
    // Dimensions described in the user's real 1920x1080 Original Aspect frame.
    // This fixture is synthetic, not a replacement for validating that frame.
    Hud hud(1440, 378);
    for (int x : {1134, 1220, 1306}) hud.ring(x, 275, 54, 55);
    REQUIRE(smp::containsReplayTransportPanel(hud.view()));
}

TEST_CASE("strong replay triple requires lower right placement") {
    for (int offset : {-380, -280, -160}) {
        Hud hud; hud.panel(1, offset, true, true);
        REQUIRE(!smp::containsReplayTransportPanel(hud.view()));
    }
}

TEST_CASE("replay triple rejects irregular spacing unequal dimensions and implausible sizes") {
    Hud irregular;
    for (int x : {450, 480, 550}) irregular.ring(x, 110, 21, 21);
    REQUIRE(!smp::containsReplayTransportPanel(irregular.view()));
    Hud wide;
    for (int x : {400, 470, 540}) wide.ring(x, 110, 21, 21);
    REQUIRE(!smp::containsReplayTransportPanel(wide.view()));
    Hud unequal;
    unequal.ring(450, 110, 21, 21); unequal.ring(480, 110, 31, 31); unequal.ring(510, 110, 21, 21);
    REQUIRE(!smp::containsReplayTransportPanel(unequal.view()));
    Hud two;
    two.ring(450, 110, 21, 21); two.ring(480, 110, 21, 21);
    REQUIRE(!smp::containsReplayTransportPanel(two.view()));
    Hud tiny;
    for (int x : {450, 480, 510}) tiny.ring(x, 110, 3, 3);
    REQUIRE(!smp::containsReplayTransportPanel(tiny.view()));
    Hud giant;
    for (int x : {300, 420, 540}) giant.ring(x, 100, 75, 75);
    REQUIRE(!smp::containsReplayTransportPanel(giant.view()));
}

TEST_CASE("extra yellow ring candidates do not prevent a valid replay triple") {
    Hud hud; hud.panel();
    hud.ring(200, 110, 21, 21); hud.ring(430, 40, 21, 21);
    hud.rect(580, 70, 25, 25, false);
    REQUIRE(smp::containsReplayTransportPanel(hud.view()));
}

TEST_CASE("replay probe uses inclusive game coordinates and bottom 35 percent") {
    const auto rect = smp::replayUiProbeRect({100, 200, 739, 679});
    REQUIRE(rect == smp::ScreenRect(100, 512, 739, 679));
    REQUIRE(rect.width() == 640); REQUIRE(rect.height() == 168);
    REQUIRE(smp::replayUiProbeRect({0, 0, 1919, 1079}).height() == 378);
    REQUIRE(!smp::replayUiProbeRect({}).valid());
}

TEST_CASE("production start gate suppresses replay without lifecycle generation watcher or artifacts then accepts live") {
    smp::MinimapStartConfirmation confirmation;
    smp::AutomaticLifecycleState lifecycle;
    int generation = 0, probes = 0, baselines = 0, watchers = 0, suppressed = 0, rearmed = 0;
    Hud replay; replay.panel(); Hud live;
    const auto root = std::filesystem::temp_directory_path() /
        ("smp-replay-suppression-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto start = [&] {
        ++baselines;
        REQUIRE(lifecycle.tryStart({})); ++generation; ++watchers;
        for (const auto* suffix : {".nav", ".json", ".events.bin"}) std::ofstream(root / (std::string("game") + suffix)) << 'x';
    };
    const auto observation = [&](smp::MinimapMonitorObservation value) {
        if (value == smp::MinimapMonitorObservation::ReplayPlaybackSuppressed) ++suppressed;
        else ++rearmed;
    };
    const auto sample = [&](bool viewport, const Hud& hud) {
        return smp::dispatchMinimapStart(confirmation.observe(viewport), [&]() -> std::optional<bool> {
            ++probes; return smp::containsReplayTransportPanel(hud.view());
        }, start, observation);
    };
    REQUIRE(!sample(true, replay)); REQUIRE(probes == 0);
    REQUIRE(!sample(true, replay)); REQUIRE(probes == 1);
    REQUIRE(confirmation.state() == smp::MinimapDetectorState::WaitForAbsence);
    for (int i = 0; i < 6; ++i) REQUIRE(!sample(true, replay));
    REQUIRE(suppressed == 1); REQUIRE(probes == 1);
    REQUIRE(lifecycle.state() == smp::AutomaticRecordingState::Idle);
    REQUIRE(!lifecycle.baseline()); REQUIRE(generation == 0); REQUIRE(baselines == 0); REQUIRE(watchers == 0);
    REQUIRE(std::filesystem::is_empty(root));
    REQUIRE(!sample(false, replay)); REQUIRE(!sample(false, replay));
    REQUIRE(rearmed == 1); REQUIRE(confirmation.state() == smp::MinimapDetectorState::WaitForAppearance);
    REQUIRE(!sample(true, live)); REQUIRE(sample(true, live));
    REQUIRE(generation == 1); REQUIRE(baselines == 1); REQUIRE(watchers == 1); REQUIRE(probes == 2);
    REQUIRE(lifecycle.state() == smp::AutomaticRecordingState::Recording);
    for (const auto* suffix : {".nav", ".json", ".events.bin"})
        REQUIRE(std::filesystem::exists(root / (std::string("game") + suffix)));
    std::filesystem::remove_all(root);
}

TEST_CASE("unavailable and throwing replay probes preserve live starts") {
    int starts = 0;
    REQUIRE(smp::dispatchMinimapStart({true, false}, [] { return std::optional<bool>{}; }, [&] { ++starts; }));
    REQUIRE(smp::dispatchMinimapStart({true, false}, []() -> std::optional<bool> { throw std::runtime_error("capture"); }, [&] { ++starts; }));
    REQUIRE(starts == 2);
}

TEST_CASE("replay detector one shot execution timing") {
    Hud hud(2); hud.panel(2);
    const auto before = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) REQUIRE(smp::containsReplayTransportPanel(hud.view()));
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - before).count();
    std::cout << "REPLAY_UI synthetic_probe=" << hud.width << 'x' << hud.height << " average_detector_us=" << micros / 20 << '\n';
}
