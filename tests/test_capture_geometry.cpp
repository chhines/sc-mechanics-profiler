#include "test_framework.h"

#include "analysis/captured_geometry.h"
#include "capture/collector.h"

#include <algorithm>
#include <iostream>
#include <utility>
#include <vector>

namespace {

smp::ScreenRegions original(int left = 0) {
    return smp::calculateStarcraftScreenRegions(
        {left, 0, left + 639, 479}, smp::StarcraftDisplayMode::OriginalAspect);
}

smp::RawInputEvent input(std::uint64_t time, smp::RawEventType type,
                         int x = 100, int y = 400, std::uint16_t key = 0) {
    smp::RawInputEvent event{};
    event.timestampTicks = time;
    event.type = type;
    event.cursorX = x;
    event.cursorY = y;
    event.virtualKey = key;
    return event;
}

class CaptureReplay {
  public:
    explicit CaptureReplay(smp::Config settings = {})
        : config(std::move(settings)), analyzer(config, 1000) {}

    void capture(smp::RawInputEvent event) {
        event.sequence = sequence++;
        // Same value-copy operation used by Collector::push. Changing current
        // after this call simulates a refresh while the consumer is behind.
        REQUIRE(queue.tryPush(smp::CapturedInputEvent{event, current}));
    }

    bool consumeOne() {
        smp::CapturedInputEvent captured{};
        if (!queue.tryPop(captured))
            return false;
        geometry.apply(captured, config, analyzer);
        const auto& regions = geometry.regions();
        const auto analysisRegions = analyzer.screenRegions();
        REQUIRE(regions.gameArea == analysisRegions.gameArea);
        REQUIRE(regions.viewport == analysisRegions.viewport);
        REQUIRE(regions.minimap == analysisRegions.minimap);
        REQUIRE(regions.commandCard == analysisRegions.commandCard);
        REQUIRE(regions.displayMode == analysisRegions.displayMode);
        debugRegions.push_back(smp::classifyScreenRegion(
            regions, {captured.event.cursorX, captured.event.cursorY}));
        analyzer.process(captured.event);
        return true;
    }

    void drain() { while (consumeOne()) {} }

    smp::AnalysisResult finish(std::uint64_t time) {
        drain();
        analyzer.finalize(time, 0);
        return analyzer.result();
    }

    smp::Config config;
    smp::Analyzer analyzer;
    smp::CapturedGeometry geometry;
    smp::CapturedEventQueue queue;
    std::optional<smp::ScreenRegions> current;
    std::vector<smp::ScreenRegion> debugRegions;
    std::uint64_t sequence{1};
};

std::size_t count(const smp::AnalysisResult& result, smp::CameraNavigationType type) {
    return static_cast<std::size_t>(std::count_if(
        result.navigationEvents.begin(), result.navigationEvents.end(),
        [type](const auto& event) { return event.type == type; }));
}

void requireSameResults(const smp::AnalysisResult& a, const smp::AnalysisResult& b) {
    REQUIRE(a.activeDurationSeconds == b.activeDurationSeconds);
    REQUIRE(a.pausedDurationSeconds == b.pausedDurationSeconds);
    REQUIRE(a.locationRecallCount == b.locationRecallCount);
    REQUIRE(a.navigationEvents.size() == b.navigationEvents.size());
    for (std::size_t i = 0; i < a.navigationEvents.size(); ++i) {
        const auto& x = a.navigationEvents[i];
        const auto& y = b.navigationEvents[i];
        REQUIRE(x.timestampTicks == y.timestampTicks);
        REQUIRE(x.activeMs == y.activeMs);
        REQUIRE(x.type == y.type);
        REQUIRE(x.id == y.id);
        REQUIRE(x.cursorX == y.cursorX);
        REQUIRE(x.cursorY == y.cursorY);
        REQUIRE(x.durationMs == y.durationMs);
        REQUIRE(x.edgeDirection == y.edgeDirection);
        REQUIRE(x.startCursorX == y.startCursorX);
        REQUIRE(x.startCursorY == y.startCursorY);
    }
    REQUIRE(a.recenters.size() == b.recenters.size());
    for (std::size_t i = 0; i < a.recenters.size(); ++i) {
        const auto& x = a.recenters[i];
        const auto& y = b.recenters[i];
        REQUIRE(x.timestampTicks == y.timestampTicks);
        REQUIRE(x.activeMs == y.activeMs);
        REQUIRE(x.type == y.type);
        REQUIRE(x.id == y.id);
        REQUIRE(x.cursorX == y.cursorX);
        REQUIRE(x.cursorY == y.cursorY);
    }
    REQUIRE(a.mechanicalEvents.size() == b.mechanicalEvents.size());
    for (std::size_t i = 0; i < a.mechanicalEvents.size(); ++i) {
        const auto& x = a.mechanicalEvents[i];
        const auto& y = b.mechanicalEvents[i];
        REQUIRE(x.timestampTicks == y.timestampTicks);
        REQUIRE(x.activeMs == y.activeMs);
        REQUIRE(x.type == y.type);
        REQUIRE(x.virtualKey == y.virtualKey);
        REQUIRE(x.scanCode == y.scanCode);
        REQUIRE(x.modifiers == y.modifiers);
        REQUIRE(x.value == y.value);
        REQUIRE(x.cursorX == y.cursorX);
        REQUIRE(x.cursorY == y.cursorY);
    }
}

} // namespace

TEST_CASE("queued minimap click retains capture geometry after current geometry changes") {
    CaptureReplay replay;
    replay.current = original();
    replay.capture(input(0, smp::RawEventType::ForegroundGained));
    replay.capture(input(10, smp::RawEventType::MouseLeftDown));
    replay.current = original(400);

    // Reproduce the previous consumer mechanism with the same old event: a
    // latest-global lookup loses its minimap attribution while draining backlog.
    smp::Analyzer oldConsumer({}, 1000);
    smp::CapturedGeometry latest;
    latest.apply({input(0, smp::RawEventType::ForegroundGained), replay.current},
                 replay.config, oldConsumer);
    oldConsumer.process(input(0, smp::RawEventType::ForegroundGained));
    oldConsumer.process(input(10, smp::RawEventType::MouseLeftDown));
    REQUIRE(oldConsumer.result().navigationEvents.empty());

    REQUIRE(replay.consumeOne());
    REQUIRE(replay.geometry.regions().clientArea == original().clientArea);
    const auto result = replay.finish(20);
    REQUIRE(result.navigationEvents.size() == 1);
    REQUIRE(result.navigationEvents[0].type == smp::CameraNavigationType::MinimapJump);
    REQUIRE(result.navigationEvents[0].timestampTicks == 10);
    REQUIRE(replay.debugRegions.back() == smp::ScreenRegion::Minimap);
}

TEST_CASE("two queued geometries independently classify the same minimap coordinate") {
    CaptureReplay replay;
    replay.current = original();
    replay.capture(input(0, smp::RawEventType::ForegroundGained));
    replay.capture(input(10, smp::RawEventType::MouseLeftDown));
    replay.current = original(400);
    replay.capture(input(20, smp::RawEventType::MouseLeftDown));
    const auto result = replay.finish(30);
    REQUIRE(count(result, smp::CameraNavigationType::MinimapJump) == 1);
    REQUIRE(result.navigationEvents[0].timestampTicks == 10);
    REQUIRE(result.mechanicalEvents.size() == 2);
    REQUIRE(replay.debugRegions[1] == smp::ScreenRegion::Minimap);
    REQUIRE(replay.debugRegions[2] == smp::ScreenRegion::Outside);
}

TEST_CASE("queued Original Aspect minimap semantics survive a Widescreen switch") {
    CaptureReplay replay;
    const smp::ScreenRect client{0, 0, 1919, 1079};
    replay.current = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::OriginalAspect);
    replay.capture(input(0, smp::RawEventType::ForegroundGained, 400, 900));
    replay.capture(input(10, smp::RawEventType::MouseLeftDown, 400, 900));
    replay.current = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::Widescreen);
    replay.capture(input(20, smp::RawEventType::MouseLeftDown, 400, 900));
    REQUIRE(replay.consumeOne());
    REQUIRE(replay.geometry.regions().displayMode == smp::StarcraftDisplayMode::OriginalAspect);
    REQUIRE((replay.geometry.regions().gameArea == smp::ScreenRect{240, 0, 1679, 1079}));
    const auto result = replay.finish(30);
    REQUIRE(count(result, smp::CameraNavigationType::MinimapJump) == 1);
    REQUIRE(result.navigationEvents[0].timestampTicks == 10);
    REQUIRE(replay.geometry.regions().displayMode == smp::StarcraftDisplayMode::Widescreen);
    REQUIRE(replay.geometry.regions().gameArea == client);
    REQUIRE(replay.debugRegions[1] == smp::ScreenRegion::Minimap);
    REQUIRE(replay.debugRegions[2] != smp::ScreenRegion::Minimap);
}

TEST_CASE("queued snapshots preserve UI regions and resolve their own calibrated minimap profile") {
    smp::Config config;
    config.useCalibratedMinimapOverride({0.1, 0.7, 0.3, 0.9});
    config.useWidescreenCalibratedMinimapOverride({0.6, 0.7, 0.8, 0.9});
    CaptureReplay replay(config);
    auto a = original();
    a.viewport = {0, 0, 639, 300};
    a.commandCard = {500, 350, 639, 479};
    replay.current = a;
    replay.capture(input(0, smp::RawEventType::ForegroundGained));
    replay.capture(input(10, smp::RawEventType::MouseLeftDown));
    auto b = smp::calculateStarcraftScreenRegions(
        {0, 0, 639, 479}, smp::StarcraftDisplayMode::Widescreen);
    b.viewport = {0, 0, 639, 320};
    b.commandCard = {550, 350, 639, 479};
    replay.current = b;
    replay.capture(input(20, smp::RawEventType::MouseLeftDown));
    REQUIRE(replay.consumeOne());
    REQUIRE(replay.geometry.regions().viewport == a.viewport);
    REQUIRE(replay.geometry.regions().commandCard == a.commandCard);
    REQUIRE(replay.geometry.minimap().source == smp::MinimapRegionSource::CalibratedOverride);
    REQUIRE(replay.consumeOne());
    REQUIRE(replay.debugRegions.back() == smp::ScreenRegion::Minimap);
    const auto result = replay.finish(30);
    REQUIRE(replay.geometry.regions().viewport == b.viewport);
    REQUIRE(replay.geometry.regions().commandCard == b.commandCard);
    REQUIRE(replay.geometry.minimap().source == smp::MinimapRegionSource::CalibratedOverride);
    REQUIRE(replay.debugRegions.back() == smp::ScreenRegion::OtherUi);
    REQUIRE(count(result, smp::CameraNavigationType::MinimapJump) == 1);
    REQUIRE(result.navigationEvents[0].timestampTicks == 10);
}

TEST_CASE("queued edge samples keep old edges and cannot gain edges from future geometry") {
    const smp::ScreenRect client{0, 0, 1919, 1079};
    const auto aspect = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::OriginalAspect);
    const auto wide = smp::calculateStarcraftScreenRegions(
        client, smp::StarcraftDisplayMode::Widescreen);
    for (const bool oldWasEdge : {true, false}) {
        CaptureReplay replay;
        replay.current = oldWasEdge ? aspect : wide;
        replay.capture(input(0, smp::RawEventType::ForegroundGained, 500, 500));
        replay.capture(input(10, smp::RawEventType::MouseMove, 242, 500));
        replay.capture(input(30, smp::RawEventType::MouseMove, 242, 500));
        replay.capture(input(40, smp::RawEventType::MouseMove, 500, 500));
        replay.current = oldWasEdge ? wide : aspect;
        replay.capture(input(50, smp::RawEventType::MouseMove, 242, 500));
        replay.capture(input(70, smp::RawEventType::MouseMove, 242, 500));
        replay.capture(input(80, smp::RawEventType::MouseMove, 500, 500));
        const auto result = replay.finish(90);
        REQUIRE(count(result, smp::CameraNavigationType::EdgeScroll) == 1);
        const auto& edge = result.navigationEvents[0];
        REQUIRE(edge.timestampTicks == (oldWasEdge ? 10u : 50u));
        REQUIRE(edge.durationMs == 30.0);
        REQUIRE(edge.edgeDirection == smp::EdgeDirection::Left);
    }
}

TEST_CASE("unknown focus gain and inputs never acquire subsequently detected geometry") {
    // Also verify stale absolute calibration rectangles cannot fill in missing evidence.
    smp::Config config;
    config.gameArea = {0, 0, 639, 479};
    config.minimap = {20, 360, 120, 470};
    CaptureReplay replay(config);
    replay.capture(input(0, smp::RawEventType::ForegroundGained));
    replay.capture(input(10, smp::RawEventType::MouseLeftDown));
    replay.capture(input(20, smp::RawEventType::MouseMove, 2, 200));
    replay.capture(input(50, smp::RawEventType::MouseMove, 2, 200));
    replay.current = original();
    auto timerSample = input(100, smp::RawEventType::MouseMove, 2, 200);
    timerSample.flags = smp::RawEventFlagPolledCursor;
    replay.capture(timerSample);
    replay.capture(input(130, smp::RawEventType::MouseMove, 2, 200));
    replay.capture(input(140, smp::RawEventType::MouseMove, 200, 200));
    replay.capture(input(150, smp::RawEventType::MouseLeftDown));
    REQUIRE(replay.consumeOne());
    REQUIRE(!replay.geometry.regions().gameArea.valid());
    REQUIRE(replay.consumeOne());
    REQUIRE(replay.analyzer.result().navigationEvents.empty());
    const auto result = replay.finish(160);
    REQUIRE(result.navigationEvents.size() == 2);
    REQUIRE(result.navigationEvents[0].type == smp::CameraNavigationType::EdgeScroll);
    REQUIRE(result.navigationEvents[0].timestampTicks == 100);
    REQUIRE(result.navigationEvents[1].type == smp::CameraNavigationType::MinimapJump);
    REQUIRE(result.navigationEvents[1].timestampTicks == 150);
    REQUIRE(result.mechanicalEvents.size() == 2);
}

TEST_CASE("foreground regain binds immediately available geometry or clears prior geometry") {
    for (const bool availableOnGain : {true, false}) {
        CaptureReplay replay;
        replay.current = original(400);
        replay.capture(input(0, smp::RawEventType::ForegroundGained));
        replay.capture(input(10, smp::RawEventType::ForegroundLost));
        replay.current = availableOnGain ? std::optional{original()} : std::nullopt;
        replay.capture(input(20, smp::RawEventType::ForegroundGained));
        replay.capture(input(30, smp::RawEventType::MouseLeftDown));
        replay.current = original();
        replay.capture(input(100, smp::RawEventType::MouseLeftDown));
        REQUIRE(replay.consumeOne());
        REQUIRE(replay.consumeOne());
        REQUIRE(replay.consumeOne());
        REQUIRE(replay.geometry.regions().gameArea.valid() == availableOnGain);
        const auto result = replay.finish(110);
        REQUIRE(count(result, smp::CameraNavigationType::MinimapJump) ==
                (availableOnGain ? 2u : 1u));
        REQUIRE(result.navigationEvents[0].timestampTicks == (availableOnGain ? 30u : 100u));
    }
}

TEST_CASE("geometry transitions break edge candidates and active episodes") {
    for (const bool previouslyQualified : {true, false}) {
        CaptureReplay replay;
        replay.current = original();
        replay.capture(input(0, smp::RawEventType::ForegroundGained, 200, 200));
        replay.capture(input(10, smp::RawEventType::MouseMove, 2, 200));
        if (previouslyQualified)
            replay.capture(input(30, smp::RawEventType::MouseMove, 2, 200));
        // The point remains on the same edge, but the coordinate system changes.
        replay.current = smp::calculateStarcraftScreenRegions(
            {0, 0, 799, 599}, smp::StarcraftDisplayMode::OriginalAspect);
        replay.capture(input(35, smp::RawEventType::MouseMove, 2, 200));
        replay.capture(input(45, smp::RawEventType::MouseMove, 200, 200));
        REQUIRE(replay.finish(50).navigationEvents.empty());
    }
}

TEST_CASE("a dropped geometry transition cannot detach surviving inputs from their snapshots") {
    smp::SpscRingBuffer<smp::CapturedInputEvent, 4> queue;
    auto current = original();
    for (std::uint64_t time = 0; time < 3; ++time)
        REQUIRE(queue.tryPush({input(time, smp::RawEventType::MouseLeftDown), current}));
    current = original(400);
    REQUIRE(!queue.tryPush({input(3, smp::RawEventType::MouseMove), current}));
    smp::CapturedInputEvent popped{};
    REQUIRE(queue.tryPop(popped));
    REQUIRE(popped.screenRegions->clientArea == original().clientArea);
    REQUIRE(queue.tryPush({input(4, smp::RawEventType::MouseLeftDown), current}));
    CaptureReplay consumer;
    std::vector<smp::ScreenRegion> locations;
    while (queue.tryPop(popped)) {
        consumer.geometry.apply(popped, consumer.config, consumer.analyzer);
        locations.push_back(smp::classifyScreenRegion(
            consumer.geometry.regions(), {100, 400}));
        consumer.analyzer.process(popped.event);
    }
    REQUIRE((locations == std::vector{smp::ScreenRegion::Minimap,
                                     smp::ScreenRegion::Minimap, smp::ScreenRegion::Outside}));
    REQUIRE(count(consumer.analyzer.result(), smp::CameraNavigationType::MinimapJump) == 2);
}

TEST_CASE("capture results are invariant across deterministic consumer drain schedules") {
    const auto run = [](std::size_t batchSize) {
        CaptureReplay replay;
        std::size_t capturedCount = 0;
        const auto send = [&](smp::RawInputEvent event) {
            replay.capture(event);
            if (++capturedCount % batchSize == 0)
                replay.drain();
        };
        send(input(0, smp::RawEventType::ForegroundGained));
        send(input(10, smp::RawEventType::MouseLeftDown));
        replay.current = original();
        send(input(100, smp::RawEventType::MouseLeftDown));
        send(input(110, smp::RawEventType::KeyDown, 200, 200, VK_F2));
        send(input(120, smp::RawEventType::KeyUp, 200, 200, VK_F2));
        send(input(130, smp::RawEventType::KeyDown, 200, 200, VK_F2));
        send(input(140, smp::RawEventType::KeyUp, 200, 200, VK_F2));
        send(input(150, smp::RawEventType::MouseMove, 2, 200));
        send(input(180, smp::RawEventType::MouseMove, 2, 200));
        send(input(190, smp::RawEventType::MouseMove, 200, 200));
        replay.current = smp::calculateStarcraftScreenRegions(
            {0, 0, 1919, 1079}, smp::StarcraftDisplayMode::Widescreen);
        send(input(200, smp::RawEventType::MouseLeftDown, 100, 900));
        send(input(210, smp::RawEventType::MouseMove, 2, 500));
        replay.current = original(400);
        send(input(230, smp::RawEventType::MouseMove, 2, 500));
        send(input(240, smp::RawEventType::ForegroundLost));
        replay.current.reset();
        send(input(300, smp::RawEventType::ForegroundGained));
        send(input(310, smp::RawEventType::MouseLeftDown));
        replay.current = original();
        send(input(400, smp::RawEventType::MouseLeftDown));
        return replay.finish(420);
    };
    const auto immediate = run(1);
    REQUIRE(count(immediate, smp::CameraNavigationType::MinimapJump) == 3);
    REQUIRE(count(immediate, smp::CameraNavigationType::EdgeScroll) == 1);
    REQUIRE(immediate.recenters.size() == 1);
    for (const std::size_t batch : {2u, 3u, 5u, 8u, 100u})
        requireSameResults(immediate, run(batch));
}

TEST_CASE("capture envelope size and raw ABI are explicit and trivially copyable") {
    static_assert(sizeof(smp::RawInputEvent) == 48);
    static_assert(std::is_trivially_copyable_v<smp::CapturedInputEvent>);
    std::cout << "Capture queue item bytes: raw=" << sizeof(smp::RawInputEvent)
              << " captured=" << sizeof(smp::CapturedInputEvent)
              << " backing increase="
              << (sizeof(smp::CapturedInputEvent) - sizeof(smp::RawInputEvent)) * 65536
              << '\n';
}

#include "platform/raw_input.h"

TEST_CASE("decoded queued click classification ignores later cursor in both minimap directions") {
    for (const bool messageInside : {true, false}) {
        CaptureReplay replay;
        replay.current = original();
        replay.capture(input(0, smp::RawEventType::ForegroundGained));
        MSG queued{};
        queued.message = WM_INPUT;
        queued.lParam = 42;
        queued.pt = messageInside ? POINT{100, 400} : POINT{300, 200};
        smp::CollectorDispatchContext context;
        context.begin(queued);
        queued.pt = messageInside ? POINT{300, 200} : POINT{100, 400};
        const auto snapshot = context.takeCursor(nullptr, WM_INPUT, 0, 42);
        REQUIRE(snapshot.has_value());
        RAWINPUT packet{};
        packet.header.dwType = RIM_TYPEMOUSE;
        packet.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN;
        std::array<smp::RawInputEvent, 8> events{};
        REQUIRE(smp::decodeRawInputPacket(packet, 10, *snapshot, events) == 1);
        replay.capture(events[0]);
        const auto result = replay.finish(20);
        REQUIRE(count(result, smp::CameraNavigationType::MinimapJump) ==
                (messageInside ? 1u : 0u));
    }
}
