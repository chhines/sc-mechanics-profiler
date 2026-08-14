#include "app/gui_single_instance.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace smp {

GuiInstanceClaim::GuiInstanceClaim(HANDLE handle, bool ownsInstance) noexcept
    : handle_(handle), ownsInstance_(ownsInstance) {}

GuiInstanceClaim::~GuiInstanceClaim() {
    close();
}

GuiInstanceClaim::GuiInstanceClaim(GuiInstanceClaim&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      ownsInstance_(std::exchange(other.ownsInstance_, false)) {}

GuiInstanceClaim& GuiInstanceClaim::operator=(GuiInstanceClaim&& other) noexcept {
    if (this == &other)
        return *this;
    close();
    handle_ = std::exchange(other.handle_, nullptr);
    ownsInstance_ = std::exchange(other.ownsInstance_, false);
    return *this;
}

GuiInstanceClaim GuiInstanceClaim::acquire(std::wstring_view mutexName) {
    if (mutexName.empty())
        throw std::invalid_argument("The GUI instance mutex name cannot be empty");
    const std::wstring terminatedName(mutexName);
    SetLastError(ERROR_SUCCESS);
    const HANDLE handle = CreateMutexW(nullptr, FALSE, terminatedName.c_str());
    const DWORD creationStatus = GetLastError();
    if (!handle) {
        throw std::system_error(static_cast<int>(creationStatus),
                                std::system_category(),
                                "Unable to create the GUI instance mutex");
    }
    const bool ownsInstance = creationStatus != ERROR_ALREADY_EXISTS;
    return GuiInstanceClaim(handle, ownsInstance);
}

void GuiInstanceClaim::close() noexcept {
    if (handle_) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
    ownsInstance_ = false;
}

UINT showExistingGuiInstanceMessage() noexcept {
    static const UINT message =
        RegisterWindowMessageW(showExistingGuiInstanceMessageName);
    return message;
}

bool signalExistingGuiInstance(DWORD maximumWaitMs) noexcept {
    const UINT message = showExistingGuiInstanceMessage();
    if (message == 0)
        return false;

    const ULONGLONG started = GetTickCount64();
    constexpr DWORD retryIntervalMs = 40;
    for (;;) {
        if (const HWND window = FindWindowW(guiMainWindowClassName, nullptr)) {
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId != 0)
                AllowSetForegroundWindow(processId);
            if (PostMessageW(window, message, 0, 0))
                return true;
        } else {
            (void)SendNotifyMessageW(HWND_BROADCAST, message, 0, 0);
        }

        const ULONGLONG elapsed = GetTickCount64() - started;
        if (elapsed >= maximumWaitMs)
            return false;
        Sleep(std::min<DWORD>(retryIntervalMs,
                              maximumWaitMs - static_cast<DWORD>(elapsed)));
    }
}

} // namespace smp
