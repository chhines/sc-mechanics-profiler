#include "test_framework.h"

#include "capture/collector.h"

TEST_CASE("stable raw input foreground checks never request geometry refresh") {
    int geometryRefreshes = 0;
    for (int packet = 0; packet < 1000; ++packet) {
        const auto decision = smp::collectorForegroundDecision(true, true, false);
        REQUIRE(decision.transition ==
                smp::CollectorForegroundTransition::None);
        if (decision.refreshGeometry)
            ++geometryRefreshes;
    }
    REQUIRE(geometryRefreshes == 0);
}

TEST_CASE("stable foreground timer checks request one geometry refresh per tick") {
    constexpr int timerTicks = 25;
    int geometryRefreshes = 0;
    for (int tick = 0; tick < timerTicks; ++tick) {
        const auto decision = smp::collectorForegroundDecision(true, true, true);
        REQUIRE(decision.transition ==
                smp::CollectorForegroundTransition::None);
        if (decision.refreshGeometry)
            ++geometryRefreshes;
    }
    REQUIRE(geometryRefreshes == timerTicks);
}

TEST_CASE("foreground gain refreshes immediately and foreground loss does not") {
    const auto gained = smp::collectorForegroundDecision(false, true, false);
    REQUIRE(gained.transition == smp::CollectorForegroundTransition::Gained);
    REQUIRE(gained.refreshGeometry);

    const auto lost = smp::collectorForegroundDecision(true, false, true);
    REQUIRE(lost.transition == smp::CollectorForegroundTransition::Lost);
    REQUIRE(!lost.refreshGeometry);

    const auto remainsInactive =
        smp::collectorForegroundDecision(false, false, true);
    REQUIRE(remainsInactive.transition ==
            smp::CollectorForegroundTransition::None);
    REQUIRE(!remainsInactive.refreshGeometry);
}

TEST_CASE("foreground transitions retain their caller observation timestamp") {
    constexpr std::uint64_t observationTimestampTicks = 42'000;
    const auto gained = smp::makeCollectorForegroundTransitionEvent(
        smp::CollectorForegroundTransition::Gained,
        observationTimestampTicks, 640, 480);
    REQUIRE(gained.type == smp::RawEventType::ForegroundGained);
    REQUIRE(gained.timestampTicks == observationTimestampTicks);
    REQUIRE(gained.cursorX == 640);
    REQUIRE(gained.cursorY == 480);

    const auto lost = smp::makeCollectorForegroundTransitionEvent(
        smp::CollectorForegroundTransition::Lost,
        observationTimestampTicks, 641, 481);
    REQUIRE(lost.type == smp::RawEventType::ForegroundLost);
    REQUIRE(lost.timestampTicks == observationTimestampTicks);
}

#include "platform/raw_input.h"
#include <chrono>
#include <thread>

// Documented native flag values occupy only low bits (RAWKEYBOARD/RAWMOUSE).
static_assert(((RI_KEY_BREAK | RI_KEY_E0 | RI_KEY_E1 | MOUSE_MOVE_ABSOLUTE |
                MOUSE_VIRTUAL_DESKTOP | MOUSE_ATTRIBUTES_CHANGED | MOUSE_MOVE_NOCOALESCE) &
               (smp::RawEventFlagMessageCursor | smp::RawEventFlagPolledCursor)) == 0);
static_assert(sizeof(smp::RawInputEvent) == 48);
static_assert(sizeof(smp::CapturedInputEvent) == 136);

TEST_CASE("raw keyboard retains queued cursor and native flags") {
    RAWINPUT packet{};
    packet.header.dwType = RIM_TYPEKEYBOARD;
    packet.data.keyboard.VKey = VK_F2;
    packet.data.keyboard.MakeCode = 60;
    packet.data.keyboard.Flags = RI_KEY_E0;
    std::array<smp::RawInputEvent, 8> events{};
    REQUIRE(smp::decodeRawInputPacket(packet, 123, POINT{350, 900}, events) == 1);
    REQUIRE(events[0].type == smp::RawEventType::KeyDown);
    REQUIRE(events[0].cursorX == 350);
    REQUIRE(events[0].cursorY == 900);
    REQUIRE(events[0].timestampTicks == 123);
    REQUIRE(events[0].scanCode == 60);
    REQUIRE(events[0].flags == (RI_KEY_E0 | smp::RawEventFlagMessageCursor));
    packet.data.keyboard.Flags |= RI_KEY_BREAK;
    REQUIRE(smp::decodeRawInputPacket(packet, 124, POINT{-1200, 500}, events) == 1);
    REQUIRE(events[0].type == smp::RawEventType::KeyUp);
    REQUIRE(events[0].cursorX == -1200);
    REQUIRE(events[0].cursorY == 500);
}

TEST_CASE("relative mouse preserves raw deltas independently of message cursor") {
    RAWINPUT packet{};
    packet.header.dwType = RIM_TYPEMOUSE;
    packet.data.mouse.lLastX = 15;
    packet.data.mouse.lLastY = -7;
    std::array<smp::RawInputEvent, 8> events{};
    REQUIRE(smp::decodeRawInputPacket(packet, 1, POINT{500, 400}, events) == 1);
    REQUIRE(events[0].type == smp::RawEventType::MouseMove);
    REQUIRE(events[0].cursorX == 500);
    REQUIRE(events[0].cursorY == 400);
    REQUIRE(events[0].mouseDx == 15);
    REQUIRE(events[0].mouseDy == -7);
    REQUIRE(events[0].flags == smp::RawEventFlagMessageCursor);
}

TEST_CASE("mouse button and multi-event packets share the supplied screen snapshot") {
    RAWINPUT packet{};
    packet.header.dwType = RIM_TYPEMOUSE;
    packet.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN;
    std::array<smp::RawInputEvent, 8> events{};
    REQUIRE(smp::decodeRawInputPacket(packet, 2, POINT{351, 901}, events) == 1);
    REQUIRE(events[0].type == smp::RawEventType::MouseLeftDown);
    REQUIRE(events[0].cursorX == 351);
    REQUIRE(events[0].cursorY == 901);
    packet.data.mouse.lLastX = 15;
    packet.data.mouse.lLastY = -7;
    packet.data.mouse.usButtonFlags |= RI_MOUSE_WHEEL;
    packet.data.mouse.usButtonData = static_cast<USHORT>(-120);
    REQUIRE(smp::decodeRawInputPacket(packet, 3, POINT{-1200, 500}, events) == 3);
    REQUIRE(events[0].type == smp::RawEventType::MouseMove);
    REQUIRE(events[1].type == smp::RawEventType::MouseLeftDown);
    REQUIRE(events[2].type == smp::RawEventType::MouseWheel);
    REQUIRE(events[2].wheelDelta == -120);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(events[i].cursorX == -1200);
        REQUIRE(events[i].cursorY == 500);
        REQUIRE(events[i].timestampTicks == 3);
        REQUIRE(events[i].flags == smp::RawEventFlagMessageCursor);
    }
}

TEST_CASE("absolute raw coordinates stay raw while cursor uses message screen coordinates") {
    RAWINPUT packet{};
    packet.header.dwType = RIM_TYPEMOUSE;
    packet.data.mouse.usFlags = MOUSE_MOVE_ABSOLUTE | MOUSE_VIRTUAL_DESKTOP | MOUSE_MOVE_NOCOALESCE;
    packet.data.mouse.lLastX = 60000;
    packet.data.mouse.lLastY = 10000;
    std::array<smp::RawInputEvent, 8> events{};
    REQUIRE(smp::decodeRawInputPacket(packet, 4, POINT{-1200, 500}, events) == 1);
    REQUIRE(events[0].cursorX == -1200);
    REQUIRE(events[0].cursorY == 500);
    REQUIRE(events[0].mouseDx == 60000);
    REQUIRE(events[0].mouseDy == 10000);
    REQUIRE(events[0].flags == (packet.data.mouse.usFlags | smp::RawEventFlagMessageCursor));
}

TEST_CASE("dispatch context preserves queued input cursor through artificial handler delay") {
    MSG queued{};
    queued.hwnd = reinterpret_cast<HWND>(1);
    queued.message = WM_INPUT;
    queued.wParam = RIM_INPUT;
    queued.lParam = 42;
    queued.pt = POINT{350, 900};
    smp::CollectorDispatchContext context;
    context.begin(queued);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // A later observed cursor is deliberately different; never consult a live cursor.
    queued.pt = POINT{900, 500};
    const auto cursor = context.takeCursor(queued.hwnd, queued.message, queued.wParam, queued.lParam);
    REQUIRE(cursor.has_value());
    RAWINPUT packet{};
    packet.header.dwType = RIM_TYPEMOUSE;
    packet.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN;
    std::array<smp::RawInputEvent, 8> events{};
    REQUIRE(smp::decodeRawInputPacket(packet, 999999, *cursor, events) == 1);
    REQUIRE(events[0].cursorX == 350);
    REQUIRE(events[0].cursorY == 900);
    REQUIRE(!context.takeCursor(queued.hwnd, queued.message, queued.wParam, queued.lParam));
}

TEST_CASE("dispatch context rejects missing mismatched expired and reused snapshots") {
    MSG queued{};
    queued.hwnd = reinterpret_cast<HWND>(1);
    queued.message = WM_INPUT;
    queued.wParam = RIM_INPUT;
    queued.lParam = 42;
    smp::CollectorDispatchContext context;
    REQUIRE(!context.takeCursor(queued.hwnd, WM_INPUT, RIM_INPUT, 42));
    context.begin(queued);
    REQUIRE(!context.takeCursor(nullptr, WM_INPUT, RIM_INPUT, 42));
    REQUIRE(!context.takeCursor(queued.hwnd, WM_TIMER, RIM_INPUT, 42));
    REQUIRE(!context.takeCursor(queued.hwnd, WM_INPUT, RIM_INPUTSINK, 42));
    REQUIRE(!context.takeCursor(queued.hwnd, WM_INPUT, RIM_INPUT, 43));
    REQUIRE(context.takeCursor(queued.hwnd, WM_INPUT, RIM_INPUT, 42));
    REQUIRE(!context.takeCursor(queued.hwnd, WM_INPUT, RIM_INPUT, 42));
    context.begin(queued);
    context.clear();
    REQUIRE(!context.takeCursor(queued.hwnd, WM_INPUT, RIM_INPUT, 42));
}

TEST_CASE("foreground observation cursor provenance stays polled") {
    const auto event = smp::makeCollectorForegroundTransitionEvent(
        smp::CollectorForegroundTransition::Gained, 123, -1200, 500);
    REQUIRE(event.flags == smp::RawEventFlagPolledCursor);
}
