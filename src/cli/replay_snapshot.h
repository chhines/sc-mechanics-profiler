#pragma once

#include "cli/replay_readiness.h"

namespace smp {

// Pins a file object, not its pathname. All sharing flags remain enabled so the
// game can continue writing, replace the path, or delete the old directory entry.
class PinnedReplaySource {
  public:
    PinnedReplaySource() = default;
    ~PinnedReplaySource();
    PinnedReplaySource(PinnedReplaySource&& other) noexcept;
    PinnedReplaySource& operator=(PinnedReplaySource&& other) noexcept;
    PinnedReplaySource(const PinnedReplaySource&) = delete;
    PinnedReplaySource& operator=(const PinnedReplaySource&) = delete;

    [[nodiscard]] static PinnedReplaySource open(const std::filesystem::path& path) noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] ReplayMetadata metadata() const noexcept;
    // Empty result means a consistent snapshot was accepted. Failed/changed
    // attempts remove their partial destination and can be retried without locking writers.
    [[nodiscard]] std::string snapshot(
        const std::filesystem::path& destination, ReplayReadinessHooks::Clock::time_point deadline,
        const std::function<ReplayReadinessHooks::Clock::time_point()>& now,
        const std::function<void()>& copiedChunk = {});

  private:
    explicit PinnedReplaySource(void* handle) : handle_(handle) {}
    void close() noexcept;
    void* handle_{};
};

struct PinnedReplayReadinessHooks {
    // Defaults use the real clock, sleep and bundled parser. Injection lets tests
    // coordinate a real Windows writer deterministically during a copy.
    std::function<ReplayReadinessHooks::Clock::time_point()> now;
    std::function<void(std::chrono::milliseconds)> wait;
    std::function<ReplayExtractionResult(const std::filesystem::path&, std::chrono::milliseconds)> parse;
    std::function<void()> copiedChunk;
};

[[nodiscard]] ReplayExtractionResult waitForPinnedReplayReadiness(
    PinnedReplaySource& source, const std::filesystem::path& snapshotPath,
    PinnedReplayReadinessHooks hooks = {}, const ReplayReadinessPolicy& policy = {});

} // namespace smp
