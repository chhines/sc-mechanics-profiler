#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace scm {

struct Rect {
    int left{};
    int top{};
    int right{};
    int bottom{};

    [[nodiscard]] bool valid() const noexcept {
        return right > left && bottom > top;
    }
    [[nodiscard]] bool contains(int x, int y) const noexcept {
        return valid() && x >= left && x <= right && y >= top && y <= bottom;
    }
};

struct ProductionRule {
    int group{};
    std::vector<std::uint16_t> trainKeys;
};

struct Config {
    std::wstring starcraftProcess{L"StarCraft.exe"};
    int controlGroupDoubleTapMs{300};
    std::vector<std::uint16_t> locationHotkeys{0x71, 0x72, 0x73}; // F2-F4
    std::uint16_t attackKey{'A'};
    std::uint16_t moveKey{'M'};
    std::uint16_t patrolKey{'P'};
    std::uint16_t stopKey{'S'};
    std::uint16_t holdKey{'H'};

    Rect viewport{};
    Rect minimap{};
    Rect commandCard{};
    int edgeThicknessPx{5};
    int edgeDwellMs{100};

    int dragThresholdPx{4};
    int reselectionIntervalMs{500};
    double reselectionIou{0.5};

    int macroRecognitionIntervalMs{750};
    int macroEpisodeGapMs{2000};
    std::vector<ProductionRule> workerRules;
    std::vector<ProductionRule> armyRules;

    int microWindowMs{2000};
    int microMinimumEvents{8};
    int microEndQuietMs{1000};

    int loadWindowSeconds{10};
    int loadMinimumObservations{10};
    bool writeLogicalEvents{true};
    int flushIntervalMs{1000};

    static Config loadOrCreate(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
};

std::uint16_t keyNameToVirtualKey(const std::string& value);
std::string virtualKeyToName(std::uint16_t key);

} // namespace scm
