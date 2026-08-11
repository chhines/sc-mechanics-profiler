#pragma once

#include "analysis/production_visit.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace smp {

enum class ReplayProductionKind : std::uint8_t {
    Train,
    TrainFighter,
    UnitMorph
};

struct ReplayPlayer {
    int id{-1};
    std::string name;
};

struct ReplayControlGroupEvent {
    std::int64_t replayFrame{};
    int playerId{-1};
    int group{-1};
};

struct ReplayProductionEvent {
    std::int64_t replayFrame{};
    int playerId{-1};
    ReplayProductionKind kind{ReplayProductionKind::Train};
    std::string unit;
    int unitId{-1};
};

struct ReplayData {
    std::int64_t totalFrames{};
    std::vector<ReplayPlayer> players;
    std::vector<ReplayControlGroupEvent> controlGroupSelections;
    std::vector<ReplayProductionEvent> productionEvents;
};

struct ReplayExtractionResult {
    bool available{};
    std::string unavailableReason;
    std::string parser;
    ReplayData replay;
};

struct ReplayPlayerMatch {
    bool available{};
    std::string unavailableReason;
    int playerId{-1};
    std::string playerName;
    double sequenceScore{};
    double runnerUpSequenceScore{};
    std::vector<std::pair<std::size_t, std::size_t>> matchedEventIndices;
};

[[nodiscard]] ReplayData parseScrepReplayJson(const std::string& replayJson);
[[nodiscard]] ReplayExtractionResult extractReplayWithBundledScrep(
    const std::filesystem::path& replayPath) noexcept;

[[nodiscard]] ReplayPlayerMatch identifyReplayPlayer(
    const std::vector<MechanicalInputEvent>& liveEvents, const ReplayData& replay);

[[nodiscard]] MacroProductType classifyReplayProduction(const ReplayProductionEvent& event) noexcept;

[[nodiscard]] ProductionAnalysis correlateProductionVisitsWithReplay(
    const AnalysisResult& result, const MacroHotkeyProfile& hotkeys, std::uint64_t qpcFrequency,
    ProductionAnalysis analysis, const ReplayData& replay, std::string parserName = {});

} // namespace smp
