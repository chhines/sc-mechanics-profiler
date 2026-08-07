#include "capture/collector.h"

#include "platform/raw_input.h"

#include <array>
#include <utility>

namespace scm {
namespace {

constexpr wchar_t windowClassName[] = L"ScMechanicsRawInputWindow";
constexpr UINT_PTR foregroundTimer = 1;

} // namespace

Collector::Collector(RawEventQueue& queue, std::wstring expectedProcess, const QpcClock& clock)
    : queue_(queue), foreground_(std::move(expectedProcess)), clock_(clock) {}

Collector::~Collector() {
    stop();
}

bool Collector::start() {
    if (thread_.joinable())
        return true;
    {
        std::lock_guard lock(startupMutex_);
        startupComplete_ = false;
        startupSuccess_ = false;
        error_.clear();
    }
    thread_ = std::thread(&Collector::run, this);
    std::unique_lock lock(startupMutex_);
    startupCv_.wait(lock, [&] { return startupComplete_; });
    return startupSuccess_;
}

void Collector::stop() {
    if (!thread_.joinable())
        return;
    if (const auto window = window_.load(std::memory_order_acquire))
        PostMessageW(window, WM_CLOSE, 0, 0);
    else if (const auto threadId = threadId_.load(std::memory_order_acquire); threadId != 0)
        PostThreadMessageW(threadId, WM_QUIT, 0, 0);
    thread_.join();
    state_.store(CollectorState::Stopped, std::memory_order_release);
}

std::string Collector::error() const {
    std::lock_guard lock(startupMutex_);
    return error_;
}

void Collector::run() {
    threadId_.store(GetCurrentThreadId(), std::memory_order_release);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Collector::windowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = windowClassName;
    RegisterClassExW(&windowClass);

    const auto window = CreateWindowExW(0, windowClassName, L"scmechanics input collector", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                        nullptr, instance, this);
    window_.store(window, std::memory_order_release);
    bool success = window != nullptr && registerRawInput(window);
    if (success) {
        SetTimer(window, foregroundTimer, 100, nullptr);
        state_.store(CollectorState::Waiting, std::memory_order_release);
        updateForeground();
    }
    {
        std::lock_guard lock(startupMutex_);
        startupComplete_ = true;
        startupSuccess_ = success;
        if (!success)
            error_ = "Unable to create the Raw Input collector window or register input devices (Windows error " +
                     std::to_string(GetLastError()) + ")";
    }
    startupCv_.notify_all();
    if (!success) {
        state_.store(CollectorState::Failed, std::memory_order_release);
        if (window)
            DestroyWindow(window);
        window_.store(nullptr, std::memory_order_release);
        return;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    unregisterRawInput();
    if (const auto remainingWindow = window_.load(std::memory_order_acquire))
        DestroyWindow(remainingWindow);
    window_.store(nullptr, std::memory_order_release);
    UnregisterClassW(windowClassName, instance);
}

LRESULT CALLBACK Collector::windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Collector* self = reinterpret_cast<Collector*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Collector*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handleMessage(window, message, wParam, lParam)
                : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Collector::handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INPUT: {
        updateForeground();
        if (foregroundActive_) {
            std::array<RawInputEvent, 8> events{};
            const auto count = decodeRawInput(lParam, clock_.now(), events);
            for (std::size_t i = 0; i < count; ++i)
                push(events[i]);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_TIMER:
        if (wParam == foregroundTimer) {
            updateForeground();
            if (foregroundActive_) {
                RawInputEvent sample{};
                sample.timestampTicks = clock_.now();
                sample.type = RawEventType::MouseMove;
                sample.flags = RawEventFlagPolledCursor;
                POINT cursor{};
                GetCursorPos(&cursor);
                sample.cursorX = cursor.x;
                sample.cursorY = cursor.y;
                push(sample);
            }
        }
        return 0;
    case WM_CLOSE:
        KillTimer(window, foregroundTimer);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        window_.store(nullptr, std::memory_order_release);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void Collector::updateForeground() {
    const bool matches = foreground_.matchesForeground();
    if (matches == foregroundActive_)
        return;
    foregroundActive_ = matches;
    RawInputEvent transition{};
    transition.timestampTicks = clock_.now();
    POINT cursor{};
    GetCursorPos(&cursor);
    transition.cursorX = cursor.x;
    transition.cursorY = cursor.y;
    if (matches) {
        transition.type = RawEventType::ForegroundGained;
        everActive_ = true;
        state_.store(CollectorState::Recording, std::memory_order_release);
    } else {
        transition.type = RawEventType::ForegroundLost;
        state_.store(everActive_ ? CollectorState::Paused : CollectorState::Waiting, std::memory_order_release);
    }
    push(transition);
}

void Collector::push(RawInputEvent event) {
    event.sequence = nextSequence_++;
    if (!queue_.tryPush(event))
        dropped_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace scm
