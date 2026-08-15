#include "platform/starcraft_display_mode.h"

#include "util/json.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <system_error>
#include <utility>

namespace smp {

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

StarcraftDisplayModeReader::StarcraftDisplayModeReader(
    std::filesystem::path path, std::chrono::milliseconds checkInterval)
    : path_(std::move(path)), checkInterval_(checkInterval) {}

StarcraftDisplayModeRefresh StarcraftDisplayModeReader::refreshNow() noexcept {
    nextCheck_ = std::chrono::steady_clock::now() + checkInterval_;
    hasCheckDeadline_ = true;
    return refreshFromDisk();
}

StarcraftDisplayModeRefresh StarcraftDisplayModeReader::refreshIfDue(
    std::chrono::steady_clock::time_point now) noexcept {
    if (hasCheckDeadline_ && now < nextCheck_)
        return {mode_, false, false};
    nextCheck_ = now + checkInterval_;
    hasCheckDeadline_ = true;
    return refreshFromDisk();
}

StarcraftDisplayModeRefresh StarcraftDisplayModeReader::refreshFromDisk() noexcept {
    const auto currentState = readFileState(path_);
    if (fileState_ && currentState == *fileState_ &&
        (!currentState.exists || mode_ != StarcraftDisplayMode::Unknown)) {
        return {mode_, false, true};
    }

    const auto previous = mode_;
    mode_ = currentState.exists ? readStarcraftDisplayMode(path_)
                                : StarcraftDisplayMode::Unknown;
    fileState_ = currentState;
    return {mode_, mode_ != previous, true};
}

StarcraftDisplayModeReader::FileState
StarcraftDisplayModeReader::readFileState(
    const std::filesystem::path& path) noexcept {
    if (path.empty())
        return {};
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return {};
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return {};
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};
    return {true, writeTime, size};
}

} // namespace smp
