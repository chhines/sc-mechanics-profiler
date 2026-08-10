#include "platform/automatic_lifecycle.h"

#include "platform/foreground.h"
#include "platform/screen_region_capture.h"
#include "platform/screen_regions.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <syncstream>
#include <thread>

namespace smp {
namespace {

constexpr auto minimapSampleInterval = std::chrono::milliseconds(50);

HANDLE asHandle(void* value) {
    return static_cast<HANDLE>(value);
}

void logDiagnostic(const std::string& text) {
    std::osyncstream(std::cout) << text << '\n';
}

std::string windowsErrorMessage(DWORD error) {
    std::array<char, 512> buffer{};
    const DWORD length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
                                        0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    if (length == 0)
        return "Windows error " + std::to_string(error);
    std::string message(buffer.data(), length);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
        message.pop_back();
    return message;
}

bool stopped(HANDLE stopEvent) {
    return WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0;
}

} // namespace

bool operator==(const ReplayMetadata& first, const ReplayMetadata& second) noexcept {
    return first.exists == second.exists && first.writeTimeUtc == second.writeTimeUtc && first.size == second.size;
}

bool replayMetadataChanged(const ReplayMetadata& baseline, const ReplayMetadata& current) noexcept {
    if (!current.exists)
        return false;
    return !baseline.exists || current.writeTimeUtc != baseline.writeTimeUtc || current.size != baseline.size;
}

ReplayMetadata readReplayMetadata(const std::filesystem::path& path) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {};
    }
    const ULARGE_INTEGER writeTime{attributes.ftLastWriteTime.dwLowDateTime,
                                   attributes.ftLastWriteTime.dwHighDateTime};
    const ULARGE_INTEGER size{attributes.nFileSizeLow, attributes.nFileSizeHigh};
    return {true, writeTime.QuadPart, size.QuadPart};
}

std::filesystem::path defaultLastReplayPath() {
    PWSTR documents = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents);
    if (FAILED(result) || !documents)
        throw std::runtime_error("Unable to locate the Windows Documents folder for LastReplay.rep");
    const std::filesystem::path path =
        std::filesystem::path(documents) / L"Starcraft" / L"maps" / L"replays" / L"LastReplay.rep";
    CoTaskMemFree(documents);
    return path;
}

std::string formatReplayWriteTimeUtc(const ReplayMetadata& metadata) {
    if (!metadata.exists)
        return "missing";
    ULARGE_INTEGER value{};
    value.QuadPart = metadata.writeTimeUtc;
    FILETIME fileTime{value.LowPart, value.HighPart};
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&fileTime, &utc))
        return std::to_string(metadata.writeTimeUtc);
    std::ostringstream formatted;
    formatted << std::setfill('0') << std::setw(4) << utc.wYear << '-' << std::setw(2) << utc.wMonth << '-'
              << std::setw(2) << utc.wDay << 'T' << std::setw(2) << utc.wHour << ':' << std::setw(2) << utc.wMinute
              << ':' << std::setw(2) << utc.wSecond << '.' << std::setw(3) << utc.wMilliseconds << 'Z';
    return formatted.str();
}

bool AutomaticLifecycleState::tryStart(const ReplayMetadata& baseline) {
    std::scoped_lock lock(mutex_);
    if (state_ != AutomaticRecordingState::Idle)
        return false;
    state_ = AutomaticRecordingState::Recording;
    baseline_ = baseline;
    return true;
}

bool AutomaticLifecycleState::tryStop(const ReplayMetadata& current) {
    std::scoped_lock lock(mutex_);
    if (state_ != AutomaticRecordingState::Recording || !baseline_ || !replayMetadataChanged(*baseline_, current))
        return false;
    state_ = AutomaticRecordingState::Idle;
    baseline_.reset();
    return true;
}

bool AutomaticLifecycleState::forceStop() {
    std::scoped_lock lock(mutex_);
    if (state_ != AutomaticRecordingState::Recording)
        return false;
    state_ = AutomaticRecordingState::Idle;
    baseline_.reset();
    return true;
}

AutomaticRecordingState AutomaticLifecycleState::state() const {
    std::scoped_lock lock(mutex_);
    return state_;
}

std::optional<ReplayMetadata> AutomaticLifecycleState::baseline() const {
    std::scoped_lock lock(mutex_);
    return baseline_;
}

LastReplayWatcher::~LastReplayWatcher() {
    stop();
}

bool LastReplayWatcher::start(const std::filesystem::path& path, ReplayMetadata baseline,
                              ChangeCallback callback) {
    stop();
    path_ = path;
    baseline_ = baseline;
    callback_ = std::move(callback);
    error_.clear();

    const HANDLE directory = CreateFileW(path.parent_path().c_str(), FILE_LIST_DIRECTORY,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        error_ = "Unable to watch " + path.parent_path().string() + ": " + windowsErrorMessage(GetLastError());
        return false;
    }
    const HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE changeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent || !changeEvent) {
        error_ = "Unable to create LastReplay watcher events: " + windowsErrorMessage(GetLastError());
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
    thread_ = std::thread(&LastReplayWatcher::run, this);
    return true;
}

void LastReplayWatcher::stop() {
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
    callback_ = {};
}

void LastReplayWatcher::run() {
    const HANDLE directory = asHandle(directoryHandle_);
    const HANDLE stopEvent = asHandle(stopEvent_);
    const HANDLE changeEvent = asHandle(changeEvent_);
    std::array<unsigned char, 8192> buffer{};
    while (!stopped(stopEvent)) {
        ResetEvent(changeEvent);
        OVERLAPPED overlapped{};
        overlapped.hEvent = changeEvent;
        const BOOL started = ReadDirectoryChangesW(directory, buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
                                                   FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                                                       FILE_NOTIFY_CHANGE_FILE_NAME,
                                                   nullptr, &overlapped, nullptr);
        if (!started && GetLastError() != ERROR_IO_PENDING)
            return;
        const std::array<HANDLE, 2> events{stopEvent, changeEvent};
        const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            CancelIoEx(directory, &overlapped);
            (void)WaitForSingleObject(changeEvent, INFINITE);
            return;
        }
        if (wait != WAIT_OBJECT_0 + 1U)
            return;
        DWORD bytes = 0;
        if (!GetOverlappedResult(directory, &overlapped, &bytes, FALSE) || bytes == 0)
            continue;

        bool relevant = false;
        std::size_t offset = 0;
        while (offset < bytes) {
            const auto* notification = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
            const std::wstring filename(notification->FileName, notification->FileNameLength / sizeof(wchar_t));
            const bool targetName = _wcsicmp(filename.c_str(), path_.filename().c_str()) == 0;
            const bool targetAction = notification->Action == FILE_ACTION_ADDED ||
                                      notification->Action == FILE_ACTION_MODIFIED ||
                                      notification->Action == FILE_ACTION_RENAMED_NEW_NAME;
            relevant = relevant || (targetName && targetAction);
            if (notification->NextEntryOffset == 0)
                break;
            offset += notification->NextEntryOffset;
        }
        if (!relevant)
            continue;

        // StarCraft may replace the replay in several writes. This one-shot delay
        // is local to the directory notification and lets its final metadata settle.
        if (WaitForSingleObject(stopEvent, 100) == WAIT_OBJECT_0)
            return;
        const auto current = readReplayMetadata(path_);
        if (replayMetadataChanged(baseline_, current)) {
            callback_(current);
            return;
        }
    }
}

MinimapStartMonitor::MinimapStartMonitor(std::wstring executableName,
                                         std::optional<NormalizedScreenRect> calibratedMinimap)
    : executableName_(std::move(executableName)), calibratedMinimap_(std::move(calibratedMinimap)) {}

MinimapStartMonitor::~MinimapStartMonitor() {
    stop();
}

bool MinimapStartMonitor::start(StartCallback callback, MinimapDetectorState initialState) {
    stop();
    if (!calibratedMinimap_ || !calibratedMinimap_->valid())
        return false;
    callback_ = std::move(callback);
    initialState_ = initialState;
    const HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent)
        return false;
    stopEvent_ = stopEvent;
    thread_ = std::thread(&MinimapStartMonitor::run, this);
    return true;
}

void MinimapStartMonitor::stop() {
    if (stopEvent_)
        SetEvent(asHandle(stopEvent_));
    if (thread_.joinable())
        thread_.join();
    if (stopEvent_) {
        CloseHandle(asHandle(stopEvent_));
        stopEvent_ = nullptr;
    }
    callback_ = {};
}

void MinimapStartMonitor::run() {
    const HANDLE stopEvent = asHandle(stopEvent_);
    ForegroundMatcher foreground(executableName_);
    ScreenRegionCapture capture;
    MinimapStartConfirmation confirmation(initialState_);
    std::uint64_t frame = 0;
    bool captureFailureAnnounced = false;
    auto nextSample = std::chrono::steady_clock::now();

    logDiagnostic(initialState_ == MinimapDetectorState::WaitForAbsence
                      ? "MINIMAP_START_DETECTOR waiting_for_absence"
                      : "MINIMAP_START_DETECTOR waiting");

    while (!stopped(stopEvent)) {
        const auto now = std::chrono::steady_clock::now();
        if (now < nextSample) {
            auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(nextSample - now);
            if (wait.count() <= 0)
                wait = std::chrono::milliseconds(1);
            if (WaitForSingleObject(stopEvent, static_cast<DWORD>(wait.count())) == WAIT_OBJECT_0)
                return;
            continue;
        }
        nextSample += minimapSampleInterval;
        if (nextSample <= now)
            nextSample = now + minimapSampleInterval;

        const HWND window = GetForegroundWindow();
        if (!foreground.matches(window))
            continue;
        auto regions = detectScreenRegionsForWindow(window);
        if (!regions)
            continue;
        *regions = withCalibratedMinimap(*regions, calibratedMinimap_);
        if (!isReasonableMinimapRect(regions->minimap, regions->gameArea))
            continue;

        BgraImageView image;
        try {
            image = capture.capture(regions->minimap);
            captureFailureAnnounced = false;
        } catch (const std::exception& error) {
            if (!captureFailureAnnounced) {
                logDiagnostic(std::string("MINIMAP_START_DETECTOR capture_unavailable=") + error.what());
                captureFailureAnnounced = true;
            }
            continue;
        }

        const bool present = containsMinimapViewportOutline(image);
        if (present && confirmation.state() == MinimapDetectorState::WaitForAppearance)
            logDiagnostic("MINIMAP_VIEWPORT_CANDIDATE\nframe=" + std::to_string(frame));
        const auto result = confirmation.observe(present);
        ++frame;
        if (result.rearmed)
            logDiagnostic("MINIMAP_START_DETECTOR waiting_for_next_game");
        if (!result.startDetected)
            continue;
        try {
            callback_();
        } catch (...) {
            // The controller owns lifecycle error reporting.
        }
        return;
    }
}

} // namespace smp
