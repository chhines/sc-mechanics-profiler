#include "cli/replay_snapshot.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <thread>
#include <utility>
#include <windows.h>

namespace smp {

PinnedReplaySource::~PinnedReplaySource() { close(); }

PinnedReplaySource::PinnedReplaySource(PinnedReplaySource&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

PinnedReplaySource& PinnedReplaySource::operator=(PinnedReplaySource&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

void PinnedReplaySource::close() noexcept {
    if (handle_) CloseHandle(std::exchange(handle_, nullptr));
}

PinnedReplaySource PinnedReplaySource::open(const std::filesystem::path& path) noexcept {
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    return handle == INVALID_HANDLE_VALUE ? PinnedReplaySource{} : PinnedReplaySource{handle};
}

ReplayMetadata PinnedReplaySource::metadata() const noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!handle_ || !GetFileInformationByHandle(handle_, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return {};
    return {true,
        (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
            info.ftLastWriteTime.dwLowDateTime,
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow};
}

std::string PinnedReplaySource::snapshot(
    const std::filesystem::path& destination, ReplayReadinessHooks::Clock::time_point deadline,
    const std::function<ReplayReadinessHooks::Clock::time_point()>& now,
    const std::function<void()>& copiedChunk) {
    const auto before = metadata();
    if (!before.exists || before.size == 0)
        return "Pinned replay is missing, unreadable or empty";
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(handle_, start, nullptr, FILE_BEGIN))
        return "Unable to seek pinned replay";

    // Declared before the stream so all early returns close it before deleting.
    struct DiscardPartial {
        std::filesystem::path path;
        bool accepted{};
        ~DiscardPartial() {
            if (!accepted) { std::error_code ignored; std::filesystem::remove(path, ignored); }
        }
    } partial{destination};
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) return "Unable to create replay snapshot";
    std::array<char, 65536> bytes{};
    std::uint64_t copied = 0;
    while (copied < before.size) {
        if (now() >= deadline) return "Replay snapshot exceeded the readiness deadline";
        const auto requested = static_cast<DWORD>(
            std::min<std::uint64_t>(bytes.size(), before.size - copied));
        DWORD count{};
        if (!ReadFile(handle_, bytes.data(), requested, &count, nullptr) || count == 0)
            return "Pinned replay snapshot read failed or file became shorter";
        output.write(bytes.data(), count);
        if (!output) return "Replay snapshot write failed";
        copied += count;
        if (copiedChunk) copiedChunk();
    }
    output.close();
    if (!output) return "Replay snapshot write failed";
    const auto after = metadata();
    if (!(before == after) || copied != before.size)
        return "Pinned replay changed during snapshot copy; retrying after settling";
    if (now() >= deadline) return "Replay snapshot exceeded the readiness deadline";
    partial.accepted = true;
    return {};
}

ReplayExtractionResult waitForPinnedReplayReadiness(
    PinnedReplaySource& source, const std::filesystem::path& snapshotPath,
    PinnedReplayReadinessHooks hooks, const ReplayReadinessPolicy& policy) try {
    ReplayExtractionResult unavailable;
    unavailable.parser = bundledReplayParserDiagnostic;
    if (!source) {
        unavailable.unavailableReason = "Unable to pin this generation's replay source";
        return unavailable;
    }
    if (!hooks.now) hooks.now = []() { return ReplayReadinessHooks::Clock::now(); };
    if (!hooks.wait) hooks.wait = [](std::chrono::milliseconds duration) { std::this_thread::sleep_for(duration); };
    if (!hooks.parse) hooks.parse = extractReplayWithBundledScrep;
    const auto deadline = hooks.now() + policy.timeout;
    bool captured = false;
    ReplayMetadata capturedMetadata;
    struct RemoveSnapshot {
        std::filesystem::path path;
        ~RemoveSnapshot() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } removeSnapshot{snapshotPath};
    ReplayReadinessHooks readiness;
    readiness.now = hooks.now;
    readiness.wait = hooks.wait;
    readiness.readMetadata = [&]() { return captured ? capturedMetadata : source.metadata(); };
    readiness.readable = [&]() { return captured || static_cast<bool>(source); };
    readiness.parse = [&](std::chrono::milliseconds timeout) {
        if (!captured) {
            const auto failure = source.snapshot(snapshotPath, deadline, hooks.now, hooks.copiedChunk);
            if (!failure.empty()) {
                unavailable.unavailableReason = failure;
                return unavailable;
            }
            captured = true;
            capturedMetadata = readReplayMetadata(snapshotPath);
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - hooks.now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            unavailable.unavailableReason = "Replay snapshot exceeded the readiness deadline";
            return unavailable;
        }
        return hooks.parse(snapshotPath, std::min(timeout, remaining));
    };
    // No watcher size/write-time comparison: the handle provides identity, and
    // metadata changes on that handle are ordinary settling of the same file.
    return waitForReplayReadiness({}, readiness, policy);
} catch (const std::exception& error) {
    ReplayExtractionResult unavailable;
    unavailable.parser = bundledReplayParserDiagnostic;
    unavailable.unavailableReason = std::string("Pinned replay finalization failed: ") + error.what();
    return unavailable;
} catch (...) {
    ReplayExtractionResult unavailable;
    unavailable.parser = bundledReplayParserDiagnostic;
    unavailable.unavailableReason = "Pinned replay finalization failed";
    return unavailable;
}

} // namespace smp
