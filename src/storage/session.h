#pragma once

#include "analysis/analyzer.h"
#include "analysis/logical_event.h"
#include "capture/raw_event.h"
#include "capture/ring_buffer.h"
#include "config/config.h"
#include "util/json.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace scm {

class SessionWriter {
  public:
    SessionWriter(const std::filesystem::path& sessionsRoot, std::uint64_t qpcFrequency, bool writeLogicalEvents,
                  int flushIntervalMs);
    ~SessionWriter();
    SessionWriter(const SessionWriter&) = delete;
    SessionWriter& operator=(const SessionWriter&) = delete;

    bool submitRaw(const RawInputEvent& event) noexcept;
    bool submitLogical(const LogicalEvent& event) noexcept;
    void stop();

    [[nodiscard]] const std::string& sessionId() const noexcept {
        return sessionId_;
    }
    [[nodiscard]] const std::filesystem::path& directory() const noexcept {
        return directory_;
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
    using LogicalStorageQueue = SpscRingBuffer<LogicalEvent, 65536>;
    RawStorageQueue rawQueue_;
    LogicalStorageQueue logicalQueue_;
    std::ofstream rawFile_;
    std::ofstream logicalFile_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> failed_{false};
    std::string sessionId_;
    std::filesystem::path directory_;
    bool writeLogicalEvents_{};
    int flushIntervalMs_{};
};

json::Value analysisToJson(const AnalysisResult& result, const std::string& sessionId);
void writeSessionSummary(const std::filesystem::path& directory, const AnalysisResult& result,
                         const std::string& sessionId);
std::vector<std::filesystem::path> listSessionSummaries(const std::filesystem::path& sessionsRoot);
std::filesystem::path resolveSessionSummary(const std::filesystem::path& sessionsRoot, const std::string& selector);
std::filesystem::path exportSessionCsv(const std::filesystem::path& sessionsRoot,
                                       const std::filesystem::path& exportRoot, const std::string& selector);

} // namespace scm
