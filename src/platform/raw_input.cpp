#include "platform/raw_input.h"

#include <hidusage.h>

namespace scm {
namespace {

void append(std::array<RawInputEvent, 8>& output, std::size_t& count, std::uint64_t timestamp, RawEventType type,
            const POINT& cursor) {
    if (count >= output.size())
        return;
    auto& event = output[count++];
    event = {};
    event.timestampTicks = timestamp;
    event.type = type;
    event.cursorX = cursor.x;
    event.cursorY = cursor.y;
}

} // namespace

bool registerRawInput(HWND target) {
    RAWINPUTDEVICE devices[2]{};
    devices[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    devices[0].usUsage = HID_USAGE_GENERIC_KEYBOARD;
    devices[0].dwFlags = RIDEV_INPUTSINK;
    devices[0].hwndTarget = target;
    devices[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    devices[1].usUsage = HID_USAGE_GENERIC_MOUSE;
    devices[1].dwFlags = RIDEV_INPUTSINK;
    devices[1].hwndTarget = target;
    return RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
}

void unregisterRawInput() {
    RAWINPUTDEVICE devices[2]{};
    devices[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    devices[0].usUsage = HID_USAGE_GENERIC_KEYBOARD;
    devices[0].dwFlags = RIDEV_REMOVE;
    devices[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    devices[1].usUsage = HID_USAGE_GENERIC_MOUSE;
    devices[1].dwFlags = RIDEV_REMOVE;
    RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
}

std::size_t decodeRawInput(LPARAM rawInputHandle, std::uint64_t timestamp, std::array<RawInputEvent, 8>& output) {
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(rawInputHandle), RID_INPUT, &input, &size,
                        sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return 0;
    POINT cursor{};
    GetCursorPos(&cursor);
    std::size_t count = 0;

    if (input.header.dwType == RIM_TYPEKEYBOARD) {
        const auto& keyboard = input.data.keyboard;
        if (keyboard.VKey == 255)
            return 0;
        append(output, count, timestamp, (keyboard.Flags & RI_KEY_BREAK) ? RawEventType::KeyUp : RawEventType::KeyDown,
               cursor);
        auto& event = output[0];
        event.scanCode = keyboard.MakeCode;
        event.virtualKey = keyboard.VKey;
        event.flags = keyboard.Flags;
        return count;
    }
    if (input.header.dwType != RIM_TYPEMOUSE)
        return 0;

    const auto& mouse = input.data.mouse;
    if (mouse.lLastX != 0 || mouse.lLastY != 0 || (mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        append(output, count, timestamp, RawEventType::MouseMove, cursor);
        output[count - 1].mouseDx = mouse.lLastX;
        output[count - 1].mouseDy = mouse.lLastY;
        output[count - 1].flags = mouse.usFlags;
    }
    const auto addButton = [&](USHORT mask, RawEventType type) {
        if (mouse.usButtonFlags & mask)
            append(output, count, timestamp, type, cursor);
    };
    addButton(RI_MOUSE_LEFT_BUTTON_DOWN, RawEventType::MouseLeftDown);
    addButton(RI_MOUSE_LEFT_BUTTON_UP, RawEventType::MouseLeftUp);
    addButton(RI_MOUSE_RIGHT_BUTTON_DOWN, RawEventType::MouseRightDown);
    addButton(RI_MOUSE_RIGHT_BUTTON_UP, RawEventType::MouseRightUp);
    addButton(RI_MOUSE_MIDDLE_BUTTON_DOWN, RawEventType::MouseMiddleDown);
    addButton(RI_MOUSE_MIDDLE_BUTTON_UP, RawEventType::MouseMiddleUp);
    if (mouse.usButtonFlags & RI_MOUSE_WHEEL) {
        append(output, count, timestamp, RawEventType::MouseWheel, cursor);
        output[count - 1].wheelDelta = static_cast<SHORT>(mouse.usButtonData);
    }
    return count;
}

} // namespace scm
