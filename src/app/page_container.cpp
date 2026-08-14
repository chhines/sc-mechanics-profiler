#include "app/page_container.h"

namespace smp {
namespace {

LRESULT CALLBACK pageContainerProcedure(HWND window, UINT message, WPARAM wParam,
                                        LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
    case WM_NOTIFY:
        if (const HWND parent = GetParent(window))
            return SendMessageW(parent, message, wParam, lParam);
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

bool registerPageContainerClass(HINSTANCE instance) noexcept {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = pageContainerProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = pageContainerClassName;
    return RegisterClassExW(&windowClass) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND createPageContainer(HWND parent, HINSTANCE instance) noexcept {
    return CreateWindowExW(
        WS_EX_CONTROLPARENT, pageContainerClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, parent, nullptr,
        instance, nullptr);
}

} // namespace smp
