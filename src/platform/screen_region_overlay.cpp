#include "platform/screen_region_overlay.h"

#include <algorithm>
#include <string>

namespace smp {
namespace {

constexpr UINT updateOverlayMessage = WM_APP + 41;
constexpr UINT hideOverlayMessage = WM_APP + 42;
constexpr COLORREF transparentColor = RGB(0, 0, 0);
constexpr COLORREF clientColor = RGB(0, 180, 220);
constexpr COLORREF gameColor = RGB(255, 128, 0);
constexpr COLORREF minimapColor = RGB(0, 220, 80);
constexpr COLORREF commandCardColor = RGB(220, 0, 180);
constexpr COLORREF edgeColor = RGB(80, 100, 235);
constexpr COLORREF automaticCandidateColor = RGB(235, 50, 160);

#ifndef WDA_EXCLUDEFROMCAPTURE
constexpr DWORD WDA_EXCLUDEFROMCAPTURE = 0x00000011;
#endif

void drawOutline(HDC dc, const ScreenRect& rect, COLORREF color,
                 int style = PS_SOLID, int width = 2) {
    if (!rect.valid())
        return;
    const HPEN pen = CreatePen(style, style == PS_SOLID ? width : 1, color);
    const auto oldPen = SelectObject(dc, pen);
    const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right + 1, rect.bottom + 1);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void drawLabel(HDC dc, const ScreenRect& rect, const wchar_t* text,
               COLORREF color, int yOffset = 3) {
    if (!rect.valid())
        return;
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, rect.left + 4, rect.top + yOffset, text,
             static_cast<int>(std::wcslen(text)));
}

const wchar_t* displayModeLabel(StarcraftDisplayMode mode) noexcept {
    switch (mode) {
    case StarcraftDisplayMode::OriginalAspect:
        return L"DISPLAY MODE: ORIGINAL ASPECT";
    case StarcraftDisplayMode::Widescreen:
        return L"DISPLAY MODE: WIDESCREEN";
    case StarcraftDisplayMode::Unknown:
        return L"DISPLAY MODE: UNKNOWN [4:3 FALLBACK]";
    }
    return L"DISPLAY MODE: UNKNOWN [4:3 FALLBACK]";
}

} // namespace

ScreenRect overlayLocalRect(const ScreenRect& desktopRect,
                            const ScreenRect& desktopBounds) noexcept {
    if (!desktopRect.valid() || !desktopBounds.valid())
        return {};
    return {desktopRect.left - desktopBounds.left,
            desktopRect.top - desktopBounds.top,
            desktopRect.right - desktopBounds.left,
            desktopRect.bottom - desktopBounds.top};
}

std::array<ScreenRect, 4> overlayEdgeMarginRects(
    const ScreenRect& localGameArea, int edgeMarginPx) noexcept {
    std::array<ScreenRect, 4> result{};
    if (!localGameArea.valid() || edgeMarginPx <= 0)
        return result;
    const int horizontal = std::min(edgeMarginPx, localGameArea.width());
    const int vertical = std::min(edgeMarginPx, localGameArea.height());
    result[0] = {localGameArea.left, localGameArea.top,
                 localGameArea.left + horizontal - 1, localGameArea.bottom};
    result[1] = {localGameArea.right - horizontal + 1, localGameArea.top,
                 localGameArea.right, localGameArea.bottom};
    result[2] = {localGameArea.left, localGameArea.top, localGameArea.right,
                 localGameArea.top + vertical - 1};
    result[3] = {localGameArea.left, localGameArea.bottom - vertical + 1,
                 localGameArea.right, localGameArea.bottom};
    return result;
}

ScreenRegionOverlayModel makeScreenRegionOverlayModel(
    const ScreenRegions& regions, const ResolvedMinimapRegion& resolvedMinimap,
    int edgeMarginPx, bool starcraftForeground) noexcept {
    ScreenRegionOverlayModel model;
    if (!starcraftForeground || !regions.clientArea.valid())
        return model;
    model.visible = true;
    model.desktopBounds = regions.clientArea;
    model.displayMode = regions.displayMode;
    model.clientRect = overlayLocalRect(regions.clientArea, regions.clientArea);
    model.gameViewportRect = overlayLocalRect(regions.gameArea, regions.clientArea);
    model.minimapRect = overlayLocalRect(regions.minimap, regions.clientArea);
    model.commandCardRect = overlayLocalRect(regions.commandCard, regions.clientArea);
    if (resolvedMinimap.source == MinimapRegionSource::CalibratedOverride) {
        model.automaticCandidateRect = overlayLocalRect(
            resolvedMinimap.automaticCandidate, regions.clientArea);
    }
    model.edgeMarginRects = overlayEdgeMarginRects(model.gameViewportRect,
                                                   edgeMarginPx);
    model.minimapSource = resolvedMinimap.source;
    return model;
}

ScreenRegionDebugOverlay::~ScreenRegionDebugOverlay() {
    stop();
}

bool ScreenRegionDebugOverlay::start(OverlayCapturePolicy capturePolicy) {
    stop();
    {
        std::scoped_lock lock(startupMutex_);
        startupComplete_ = false;
        startupSucceeded_ = false;
    }
    captureExclusionApplied_.store(false, std::memory_order_release);
    capturePolicy_ = capturePolicy;
    thread_ = std::thread(&ScreenRegionDebugOverlay::run, this);
    std::unique_lock lock(startupMutex_);
    startupReady_.wait(lock, [this]() { return startupComplete_; });
    const bool succeeded = startupSucceeded_;
    lock.unlock();
    if (!succeeded && thread_.joinable())
        thread_.join();
    return succeeded;
}

void ScreenRegionDebugOverlay::update(
    const ScreenRegionOverlayModel& model) noexcept {
    const HWND window = window_.load(std::memory_order_acquire);
    if (!window)
        return;
    {
        std::scoped_lock lock(modelMutex_);
        pendingModel_ = model;
    }
    PostMessageW(window, updateOverlayMessage, 0, 0);
}

void ScreenRegionDebugOverlay::hide() noexcept {
    if (const HWND window = window_.load(std::memory_order_acquire))
        PostMessageW(window, hideOverlayMessage, 0, 0);
}

void ScreenRegionDebugOverlay::stop() noexcept {
    if (const HWND window = window_.load(std::memory_order_acquire))
        PostMessageW(window, WM_CLOSE, 0, 0);
    if (thread_.joinable())
        thread_.join();
    window_.store(nullptr, std::memory_order_release);
}

void ScreenRegionDebugOverlay::run() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = &ScreenRegionDebugOverlay::windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = screenRegionOverlayClassName;
    const ATOM registered = RegisterClassExW(&windowClass);
    const bool classReady = registered != 0 ||
                            GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    HWND window = nullptr;
    if (classReady) {
        window = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            screenRegionOverlayClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr,
            nullptr, instance, this);
    }
    if (window) {
        SetLayeredWindowAttributes(window, transparentColor, 0, LWA_COLORKEY);
        if (capturePolicy_ == OverlayCapturePolicy::ExcludeFromCapture) {
            captureExclusionApplied_.store(
                SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE) != FALSE,
                std::memory_order_release);
        }
        window_.store(window, std::memory_order_release);
    }
    {
        std::scoped_lock lock(startupMutex_);
        startupComplete_ = true;
        startupSucceeded_ = window != nullptr;
    }
    startupReady_.notify_all();
    if (!window) {
        if (registered)
            UnregisterClassW(screenRegionOverlayClassName, instance);
        return;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    window_.store(nullptr, std::memory_order_release);
    if (registered)
        UnregisterClassW(screenRegionOverlayClassName, instance);
}

LRESULT CALLBACK ScreenRegionDebugOverlay::windowProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<ScreenRegionDebugOverlay*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<ScreenRegionDebugOverlay*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handleMessage(window, message, wParam, lParam)
                : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT ScreenRegionDebugOverlay::handleMessage(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case updateOverlayMessage:
        applyPendingModel(window);
        return 0;
    case hideOverlayMessage:
        currentModel_.visible = false;
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_PAINT:
        paint(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void ScreenRegionDebugOverlay::applyPendingModel(HWND window) {
    {
        std::scoped_lock lock(modelMutex_);
        currentModel_ = pendingModel_;
    }
    if (!currentModel_.visible || !currentModel_.desktopBounds.valid()) {
        ShowWindow(window, SW_HIDE);
        return;
    }
    const auto& bounds = currentModel_.desktopBounds;
    SetWindowPos(window, HWND_TOPMOST, bounds.left, bounds.top, bounds.width(),
                 bounds.height(), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window, nullptr, TRUE);
}

void ScreenRegionDebugOverlay::paint(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const HBRUSH background = CreateSolidBrush(transparentColor);
    FillRect(dc, &client, background);
    DeleteObject(background);

    drawOutline(dc, currentModel_.clientRect, clientColor);
    drawLabel(dc, currentModel_.clientRect, L"CLIENT", clientColor);
    drawLabel(dc, currentModel_.clientRect,
              displayModeLabel(currentModel_.displayMode), clientColor, 20);
    drawOutline(dc, currentModel_.gameViewportRect, gameColor);
    drawLabel(dc, currentModel_.gameViewportRect, L"GAME / VIEWPORT", gameColor);
    for (const auto& edge : currentModel_.edgeMarginRects)
        drawOutline(dc, edge, edgeColor, PS_DOT, 1);
    drawOutline(dc, currentModel_.minimapRect, minimapColor);
    drawLabel(dc, currentModel_.minimapRect,
              currentModel_.minimapSource == MinimapRegionSource::CalibratedOverride
                  ? L"MINIMAP [CALIBRATED OVERRIDE]"
                  : L"MINIMAP [AUTO]",
              minimapColor);
    drawOutline(dc, currentModel_.commandCardRect, commandCardColor);
    drawLabel(dc, currentModel_.commandCardRect, L"COMMAND CARD",
              commandCardColor);
    if (currentModel_.automaticCandidateRect.valid()) {
        drawOutline(dc, currentModel_.automaticCandidateRect,
                    automaticCandidateColor, PS_DASH, 1);
        drawLabel(dc, currentModel_.automaticCandidateRect,
                  L"MINIMAP [AUTO CANDIDATE]", automaticCandidateColor);
    }
    EndPaint(window, &paint);
}

} // namespace smp
