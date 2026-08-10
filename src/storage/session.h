#pragma once

#include "analysis/analyzer.h"
#include "capture/raw_event.h"
#include "capture/ring_buffer.h"
#include "platform/clock.h"
#include "util/json.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace smp {

constexpr std::uint16_t navFileSchemaVersion = 4;

struct NavSession {
    std::string sessionId;
    std::uint64_t qpcFrequency{};
    std::int64_t sessionStartUnixMs{};
    std::optional<QpcWallClockAnchor> activeTimelineAnchor;
    AnalysisResult analysis;
};

class SessionWriter {
  public:
    SessionWriter(const std::filesystem::path& sessionsRoot, std::uint64_t qpcFrequency, int flushIntervalMs,
                  bool saveRaw = false);
    ~SessionWriter();
    SessionWriter(const SessionWriter&) = delete;
    SessionWriter& operator=(const SessionWriter&) = delete;

    bool submitRaw(const RawInputEvent& event) noexcept;
    void setActiveTimelineAnchor(QpcWallClockAnchor anchor) noexcept;
    void stop();
    std::filesystem::path writeNavigation(const AnalysisResult& result);

    [[nodiscard]] const std::string& sessionId() const noexcept {
        return sessionId_;
    }
    [[nodiscard]] const std::filesystem::path& navPath() const noexcept {
        return navPath_;
    }
    [[nodiscard]] const std::filesystem::path& rawPath() const noexcept {
        return rawPath_;
    }
    [[nodiscard]] std::int64_t sessionStartUnixMs() const noexcept {
        return sessionStartUnixMs_;
    }
    [[nodiscard]] bool rawEnabled() const noexcept {
        return rawEnabled_;
    }
    [[nodiscard]] std::uint64_t droppedEvents() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool failed() const noexcept {
        return failed_.load(std::memory_order_acquire);
    }

  private:
    void run();

    using RawStorageQueue = SpscRingBuffer<RawInputEvent, 65536>;
    RawStorageQueue rawQueue_;
    std::ofstream rawFile_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> failed_{false};
    std::string sessionId_;
    std::filesystem::path navPath_;
    std::filesystem::path rawPath_;
    std::uint64_t qpcFrequency_{};
    std::int64_t sessionStartUnixMs_{};
    std::optional<QpcWallClockAnchor> activeTimelineAnchor_;
    int flushIntervalMs_{};
    bool rawEnabled_{};
};

std::filesystem::path writeNavSession(const std::filesystem::path& navPath, const AnalysisResult& result,
                                      const std::string& sessionId, std::uint64_t qpcFrequency,
                                      std::int64_t sessionStartUnixMs,
                                      std::optional<QpcWallClockAnchor> activeTimelineAnchor = std::nullopt);
NavSession readNavSession(const std::filesystem::path& navPath);
std::optional<std::int64_t> qpcTimestampToUnixNanoseconds(const NavSession& session,
                                                         std::uint64_t timestampTicks) noexcept;
std::vector<std::filesystem::path> listNavSessions(const std::filesystem::path& sessionsRoot);
std::filesystem::path resolveNavSession(const std::filesystem::path& sessionsRoot, const std::string& selector);

json::Value analysisToJson(const AnalysisResult& result, const std::string& sessionId);
std::filesystem::path exportSessionCsv(const std::filesystem::path& sessionsRoot,
                                       const std::filesystem::path& exportRoot, const std::string& selector);

} // namespace smp
