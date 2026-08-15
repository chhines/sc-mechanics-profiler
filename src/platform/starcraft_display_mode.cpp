#include "platform/starcraft_display_mode.h"

#include "util/json.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <array>
#include <cstddef>
#include <utility>

namespace smp {
namespace {

HANDLE asHandle(void* value) noexcept {
    return static_cast<HANDLE>(value);
}

std::string windowsError(DWORD error) {
    return "Windows error " + std::to_string(error);
}

} // namespace

const char* starcraftDisplayModeName(StarcraftDisplayMode mode) noexcept {
    switch (mode) {
    case StarcraftDisplayMode::OriginalAspect:
        return "original_aspect";
    case StarcraftDisplayMode::Widescreen:
        return "widescreen";
    case StarcraftDisplayMode::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::filesystem::path defaultStarcraftSettingsPath() noexcept {
    PWSTR documents = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)) ||
        !documents) {
        return {};
    }
    try {
        const auto path = std::filesystem::path(documents) /
                          L"StarCraft" / L"CSettings.json";
        CoTaskMemFree(documents);
        return path;
    } catch (...) {
        CoTaskMemFree(documents);
        return {};
    }
}

StarcraftDisplayMode readStarcraftDisplayMode(
    const std::filesystem::path& path) noexcept {
    if (path.empty())
        return StarcraftDisplayMode::Unknown;
    try {
        const auto root = json::parseFile(path);
        const auto& value = root["OriginalAspectRatio"];
        if (!value.isBool())
            return StarcraftDisplayMode::Unknown;
        return value.asBool() ? StarcraftDisplayMode::OriginalAspect
                              : StarcraftDisplayMode::Widescreen;
    } catch (...) {
        return StarcraftDisplayMode::Unknown;
    }
}

StarcraftDisplayModeWatcher::StarcraftDisplayModeWatcher(
    std::filesystem::path path, std::chrono::milliseconds debounceInterval)
    : path_(std::move(path)), debounceInterval_(debounceInterval) {
    if (debounceInterval_.count() < 0)
        debounceInterval_ = std::chrono::milliseconds(0);
}

StarcraftDisplayModeWatcher::~StarcraftDisplayModeWatcher() {
    stop();
}

bool StarcraftDisplayModeWatcher::start() {
    stop();
    error_.clear();
    mode_.store(readStarcraftDisplayMode(path_), std::memory_order_release);
    const auto directoryPath = path_.parent_path();
    if (path_.empty() || directoryPath.empty()) {
        error_ = "StarCraft settings path has no parent directory";
        return false;
    }

    const HANDLE directory = CreateFileW(
        directoryPath.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        error_ = "Unable to watch " + directoryPath.string() + ": " +
                 windowsError(GetLastError());
        return false;
    }
    const HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE changeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent || !changeEvent) {
        error_ = "Unable to create StarCraft display-mode watcher events: " +
                 windowsError(GetLastError());
        if (stopEvent)
            CloseHandle(stopEvent);
        if (changeEvent)
            CloseHandle(changeEvent);
        CloseHandle(directory);
        return false;
    }

    directoryHandle_ = directory;
    stopEvent_ = stopEvent;
    changeEvent_ = changeEvent;
    {
        std::scoped_lock lock(startupMutex_);
        startupComplete_ = false;
        startupSucceeded_ = false;
    }
    thread_ = std::thread(&StarcraftDisplayModeWatcher::run, this);
    std::unique_lock lock(startupMutex_);
    startupReady_.wait(lock, [this]() { return startupComplete_; });
    const bool succeeded = startupSucceeded_;
    lock.unlock();
    if (!succeeded) {
        stop();
    } else {
        // Close the small gap between the initial read and arming the directory
        // notification without ever replacing a valid mode with Unknown.
        const auto armedMode = readStarcraftDisplayMode(path_);
        if (armedMode != StarcraftDisplayMode::Unknown)
            mode_.store(armedMode, std::memory_order_release);
    }
    return succeeded;
}

void StarcraftDisplayModeWatcher::stop() noexcept {
    if (stopEvent_)
        SetEvent(asHandle(stopEvent_));
    if (thread_.joinable())
        thread_.join();
    if (directoryHandle_) {
        CloseHandle(asHandle(directoryHandle_));
        directoryHandle_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(asHandle(stopEvent_));
        stopEvent_ = nullptr;
    }
    if (changeEvent_) {
        CloseHandle(asHandle(changeEvent_));
        changeEvent_ = nullptr;
    }
}

bool StarcraftDisplayModeWatcher::waitForDebounce() const noexcept {
    const auto timeout = static_cast<DWORD>(debounceInterval_.count());
    return WaitForSingleObject(asHandle(stopEvent_), timeout) == WAIT_TIMEOUT;
}

void StarcraftDisplayModeWatcher::updateFromDiskRetainingValidMode() noexcept {
    auto candidate = readStarcraftDisplayMode(path_);
    if (candidate == StarcraftDisplayMode::Unknown && waitForDebounce())
        candidate = readStarcraftDisplayMode(path_);
    if (candidate != StarcraftDisplayMode::Unknown)
        mode_.store(candidate, std::memory_order_release);
}

void StarcraftDisplayModeWatcher::run() noexcept {
    const HANDLE directory = asHandle(directoryHandle_);
    const HANDLE stopEvent = asHandle(stopEvent_);
    const HANDLE changeEvent = asHandle(changeEvent_);
    alignas(FILE_NOTIFY_INFORMATION) std::array<std::byte, 8192> buffer{};
    bool startupReported = false;
    const auto reportStartup = [this, &startupReported](bool succeeded) {
        if (startupReported)
            return;
        {
            std::scoped_lock lock(startupMutex_);
            startupSucceeded_ = succeeded;
            startupComplete_ = true;
        }
        startupReported = true;
        startupReady_.notify_all();
    };

    while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        ResetEvent(changeEvent);
        OVERLAPPED overlapped{};
        overlapped.hEvent = changeEvent;
        const BOOL started = ReadDirectoryChangesW(
            directory, buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_FILE_NAME,
            nullptr, &overlapped, nullptr);
        const bool armed = started || GetLastError() == ERROR_IO_PENDING;
        reportStartup(armed);
        if (!armed)
            return;

        const std::array<HANDLE, 2> events{stopEvent, changeEvent};
        const DWORD wait = WaitForMultipleObjects(
            static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            CancelIoEx(directory, &overlapped);
            (void)WaitForSingleObject(changeEvent, INFINITE);
            return;
        }
        if (wait != WAIT_OBJECT_0 + 1U)
            return;

        DWORD bytes = 0;
        if (!GetOverlappedResult(directory, &overlapped, &bytes, FALSE) ||
            bytes == 0) {
            continue;
        }
        bool relevant = false;
        std::size_t offset = 0;
        while (offset < bytes) {
            const auto* notification =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                    buffer.data() + offset);
            const std::wstring filename(
                notification->FileName,
                notification->FileNameLength / sizeof(wchar_t));
            const bool targetName =
                _wcsicmp(filename.c_str(), path_.filename().c_str()) == 0;
            const bool targetAction =
                notification->Action == FILE_ACTION_ADDED ||
                notification->Action == FILE_ACTION_MODIFIED ||
                notification->Action == FILE_ACTION_RENAMED_NEW_NAME;
            relevant = relevant || (targetName && targetAction);
            if (notification->NextEntryOffset == 0)
                break;
            offset += notification->NextEntryOffset;
        }
        if (!relevant)
            continue;
        if (!waitForDebounce())
            return;
        updateFromDiskRetainingValidMode();
    }
    reportStartup(false);
}

} // namespace smp
