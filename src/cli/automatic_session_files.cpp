#include "cli/automatic_session_files.h"

#include "analysis/macro_gap.h"
#include "analysis/replay_analysis.h"
#include "cli/report.h"
#include "platform/automatic_lifecycle.h"
#include "util/json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>
#include <windows.h>

namespace smp {
namespace {

constexpr std::string_view automaticSessionSummarySuffix = "_session.txt";

struct PersistedSessionStats {
    std::uint64_t games{};
    double activeSeconds{};
    std::uint64_t controlGroupJumps{};
    std::uint64_t locationHotkeyJumps{};
    std::uint64_t minimapJumps{};
    std::uint64_t edgePans{};
    std::uint64_t workerGamesAnalyzed{};
    std::uint64_t workerGamesUnavailable{};
    double workerAnalyzedActiveSeconds{};
    std::uint64_t workerCycles{};
    std::uint64_t workerVisits{};
    double workerDurationMs{};
    std::vector<double> workerGapDurationsMs;
    std::uint64_t armyGamesAnalyzed{};
    std::uint64_t armyGamesUnavailable{};
    double armyAnalyzedActiveSeconds{};
    std::uint64_t armyCycles{};
    std::uint64_t armyVisits{};
    double armyDurationMs{};
    std::vector<double> armyGapDurationsMs;
    std::uint64_t armyControlGroupGamesAnalyzed{};
    std::uint64_t armyControlGroupGamesUnavailable{};
    double armyControlGroupActiveSeconds{};
    std::uint64_t assignments{};
    std::uint64_t additions{};
    std::uint64_t scoutingUnits{};
    std::uint64_t armyCommandGamesAnalyzed{};
    std::uint64_t armyCommandGamesUnavailable{};
    double armyCommandAnalyzedActiveSeconds{};
    std::uint64_t armyCommandCount{};
    std::vector<double> armyCommandGapDurationsMs;
    std::uint64_t abilityGamesAnalyzed{};
    std::uint64_t abilityGamesUnavailable{};
    double abilityAnalyzedActiveSeconds{};
    std::uint64_t totalAbilityUses{};
    std::uint64_t multitaskingGamesAnalyzed{};
    std::uint64_t multitaskingGamesUnavailable{};
    std::uint64_t totalDiversityAcrossActiveWindows{};
    std::uint64_t activeWindowCount{};
    int peakDiversity{};
};

std::string automaticSessionStartId(std::chrono::system_clock::time_point time) {
    const std::time_t value = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    localtime_s(&local, &value);
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d_%H%M%S");
    return output.str();
}

void addArtifact(std::set<std::filesystem::path>& paths,
                 const std::filesystem::path& path) {
    if (!path.empty())
        paths.insert(path);
}

bool isAutomaticSessionSummary(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    return filename.size() > automaticSessionSummarySuffix.size() &&
           filename.ends_with(automaticSessionSummarySuffix);
}

std::string summarySortKey(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    return filename.substr(
        0, filename.size() - automaticSessionSummarySuffix.size());
}

std::filesystem::path sessionDataPath(
    const std::filesystem::path& summaryPath) {
    auto path = summaryPath;
    path.replace_extension(".json");
    return path;
}

json::Value optionalNumber(const std::optional<double>& value) {
    return value ? json::Value(*value) : json::Value(nullptr);
}

PersistedSessionStats persistedStats(const AutomaticSessionStats& stats) {
    PersistedSessionStats result;
    result.games = stats.games;
    result.activeSeconds = stats.activeSeconds;
    result.controlGroupJumps = stats.controlGroupJumps;
    result.locationHotkeyJumps = stats.locationHotkeyJumps;
    result.minimapJumps = stats.minimapJumps;
    result.edgePans = stats.edgePans;
    result.workerGamesAnalyzed = stats.workerMacro.gamesAnalyzed;
    result.workerGamesUnavailable = stats.workerMacro.gamesUnavailable;
    result.workerAnalyzedActiveSeconds =
        stats.workerMacro.analyzedActiveSeconds;
    result.workerCycles = stats.workerMacro.cycles;
    result.workerVisits = stats.workerMacro.productionVisits;
    result.workerDurationMs = stats.workerMacro.totalDurationMs;
    result.workerGapDurationsMs = stats.workerMacro.gapDurationsMs;
    result.armyGamesAnalyzed = stats.armyMacro.gamesAnalyzed;
    result.armyGamesUnavailable = stats.armyMacro.gamesUnavailable;
    result.armyAnalyzedActiveSeconds = stats.armyMacro.analyzedActiveSeconds;
    result.armyCycles = stats.armyMacro.cycles;
    result.armyVisits = stats.armyMacro.productionVisits;
    result.armyDurationMs = stats.armyMacro.totalDurationMs;
    result.armyGapDurationsMs = stats.armyMacro.gapDurationsMs;
    result.armyControlGroupGamesAnalyzed =
        stats.armyControlGroupGamesAnalyzed;
    result.armyControlGroupGamesUnavailable =
        stats.armyControlGroupGamesUnavailable;
    result.armyControlGroupActiveSeconds =
        stats.armyControlGroups.activeDurationSeconds;
    result.assignments = stats.armyControlGroups.assignments;
    result.additions = stats.armyControlGroups.additions;
    result.scoutingUnits =
        stats.armyControlGroups.scoutingUnitActivities.size();
    result.armyCommandGamesAnalyzed = stats.armyCommands.gamesAnalyzed;
    result.armyCommandGamesUnavailable = stats.armyCommands.gamesUnavailable;
    result.armyCommandAnalyzedActiveSeconds =
        stats.armyCommands.analyzedActiveSeconds;
    result.armyCommandCount = stats.armyCommands.commandCount;
    result.armyCommandGapDurationsMs = stats.armyCommands.gapDurationsMs;
    result.abilityGamesAnalyzed = stats.abilityActivity.gamesAnalyzed;
    result.abilityGamesUnavailable = stats.abilityActivity.gamesUnavailable;
    result.abilityAnalyzedActiveSeconds =
        stats.abilityActivity.analyzedActiveSeconds;
    result.totalAbilityUses = stats.abilityActivity.totalUses;
    result.multitaskingGamesAnalyzed = stats.multitasking.gamesAnalyzed;
    result.multitaskingGamesUnavailable = stats.multitasking.gamesUnavailable;
    result.totalDiversityAcrossActiveWindows =
        stats.multitasking.totalDiversityAcrossActiveWindows;
    result.activeWindowCount = stats.multitasking.activeWindowCount;
    result.peakDiversity = stats.multitasking.peakDiversity;
    return result;
}

void merge(PersistedSessionStats& target,
           const PersistedSessionStats& source) {
    target.games += source.games;
    target.activeSeconds += source.activeSeconds;
    target.controlGroupJumps += source.controlGroupJumps;
    target.locationHotkeyJumps += source.locationHotkeyJumps;
    target.minimapJumps += source.minimapJumps;
    target.edgePans += source.edgePans;
    target.workerGamesAnalyzed += source.workerGamesAnalyzed;
    target.workerGamesUnavailable += source.workerGamesUnavailable;
    target.workerAnalyzedActiveSeconds += source.workerAnalyzedActiveSeconds;
    target.workerCycles += source.workerCycles;
    target.workerVisits += source.workerVisits;
    target.workerDurationMs += source.workerDurationMs;
    target.workerGapDurationsMs.insert(target.workerGapDurationsMs.end(),
                                       source.workerGapDurationsMs.begin(),
                                       source.workerGapDurationsMs.end());
    target.armyGamesAnalyzed += source.armyGamesAnalyzed;
    target.armyGamesUnavailable += source.armyGamesUnavailable;
    target.armyAnalyzedActiveSeconds += source.armyAnalyzedActiveSeconds;
    target.armyCycles += source.armyCycles;
    target.armyVisits += source.armyVisits;
    target.armyDurationMs += source.armyDurationMs;
    target.armyGapDurationsMs.insert(target.armyGapDurationsMs.end(),
                                     source.armyGapDurationsMs.begin(),
                                     source.armyGapDurationsMs.end());
    target.armyControlGroupGamesAnalyzed +=
        source.armyControlGroupGamesAnalyzed;
    target.armyControlGroupGamesUnavailable +=
        source.armyControlGroupGamesUnavailable;
    target.armyControlGroupActiveSeconds +=
        source.armyControlGroupActiveSeconds;
    target.assignments += source.assignments;
    target.additions += source.additions;
    target.scoutingUnits += source.scoutingUnits;
    target.armyCommandGamesAnalyzed += source.armyCommandGamesAnalyzed;
    target.armyCommandGamesUnavailable += source.armyCommandGamesUnavailable;
    target.armyCommandAnalyzedActiveSeconds +=
        source.armyCommandAnalyzedActiveSeconds;
    target.armyCommandCount += source.armyCommandCount;
    target.armyCommandGapDurationsMs.insert(
        target.armyCommandGapDurationsMs.end(),
        source.armyCommandGapDurationsMs.begin(),
        source.armyCommandGapDurationsMs.end());
    target.abilityGamesAnalyzed += source.abilityGamesAnalyzed;
    target.abilityGamesUnavailable += source.abilityGamesUnavailable;
    target.abilityAnalyzedActiveSeconds +=
        source.abilityAnalyzedActiveSeconds;
    target.totalAbilityUses += source.totalAbilityUses;
    target.multitaskingGamesAnalyzed += source.multitaskingGamesAnalyzed;
    target.multitaskingGamesUnavailable += source.multitaskingGamesUnavailable;
    target.totalDiversityAcrossActiveWindows +=
        source.totalDiversityAcrossActiveWindows;
    target.activeWindowCount += source.activeWindowCount;
    target.peakDiversity = std::max(target.peakDiversity,
                                    source.peakDiversity);
}

std::uint64_t transitions(const PersistedSessionStats& stats) {
    return stats.controlGroupJumps + stats.locationHotkeyJumps +
           stats.minimapJumps + stats.edgePans;
}

double transitionsPerMinute(const PersistedSessionStats& stats) {
    return stats.activeSeconds > 0.0
               ? static_cast<double>(transitions(stats)) /
                     (stats.activeSeconds / 60.0)
               : 0.0;
}

double editsPerMinute(const PersistedSessionStats& stats) {
    return stats.armyControlGroupActiveSeconds > 0.0
               ? static_cast<double>(stats.assignments + stats.additions) /
                     (stats.armyControlGroupActiveSeconds / 60.0)
               : 0.0;
}

std::optional<double> pooledRatePerMinute(std::uint64_t count,
                                          double analyzedActiveSeconds,
                                          std::uint64_t gamesAnalyzed) {
    if (gamesAnalyzed == 0 || analyzedActiveSeconds <= 0.0)
        return std::nullopt;
    return static_cast<double>(count) / (analyzedActiveSeconds / 60.0);
}

std::optional<double> longestGap(const std::vector<double>& gaps) {
    if (gaps.empty())
        return std::nullopt;
    return *std::max_element(gaps.begin(), gaps.end());
}

std::optional<double> gapsOverPerGame(const std::vector<double>& gaps,
                                      double thresholdMs,
                                      std::uint64_t gamesAnalyzed) {
    if (gamesAnalyzed == 0)
        return std::nullopt;
    const auto count = std::count_if(
        gaps.begin(), gaps.end(), [thresholdMs](double durationMs) {
            return durationMs > thresholdMs;
        });
    return static_cast<double>(count) / static_cast<double>(gamesAnalyzed);
}

std::optional<double> averageActiveDiversity(
    const PersistedSessionStats& stats) {
    if (stats.multitaskingGamesAnalyzed == 0 ||
        stats.activeWindowCount == 0)
        return std::nullopt;
    return static_cast<double>(stats.totalDiversityAcrossActiveWindows) /
           static_cast<double>(stats.activeWindowCount);
}

std::optional<double> peakDiversity(const PersistedSessionStats& stats) {
    return stats.multitaskingGamesAnalyzed > 0
               ? std::optional<double>(
                     static_cast<double>(stats.peakDiversity))
               : std::nullopt;
}

std::optional<double> averageDuration(double totalMs,
                                      std::uint64_t cycles) {
    return cycles > 0
               ? std::optional<double>(
                     totalMs / static_cast<double>(cycles))
               : std::nullopt;
}

json::Value numberArray(const std::vector<double>& values) {
    json::Value::Array result;
    result.reserve(values.size());
    for (const double value : values)
        result.emplace_back(value);
    return result;
}

std::vector<double> numberArray(const json::Value& value) {
    std::vector<double> result;
    if (!value.isArray())
        return result;
    result.reserve(value.asArray().size());
    for (const auto& item : value.asArray()) {
        if (item.isNumber() && std::isfinite(item.asNumber()) &&
            item.asNumber() >= 0.0) {
            result.push_back(item.asNumber());
        }
    }
    return result;
}

json::Value persistedStatsToJson(const PersistedSessionStats& stats) {
    json::Value value(json::Value::Object{});
    value["games"] = static_cast<double>(stats.games);
    value["active_seconds"] = stats.activeSeconds;
    value["navigation"] = json::Value::Object{
        {"control_group_jumps",
         static_cast<double>(stats.controlGroupJumps)},
        {"location_hotkey_jumps",
         static_cast<double>(stats.locationHotkeyJumps)},
        {"minimap_jumps", static_cast<double>(stats.minimapJumps)},
        {"edge_pans", static_cast<double>(stats.edgePans)},
        {"total_transitions", static_cast<double>(transitions(stats))},
        {"transitions_per_minute", transitionsPerMinute(stats)},
    };
    value["worker_macro"] = json::Value::Object{
        {"games_analyzed", static_cast<double>(stats.workerGamesAnalyzed)},
        {"games_unavailable",
         static_cast<double>(stats.workerGamesUnavailable)},
        {"analyzed_active_seconds", stats.workerAnalyzedActiveSeconds},
        {"cycles", static_cast<double>(stats.workerCycles)},
        {"cycles_per_minute",
         optionalNumber(pooledRatePerMinute(
             stats.workerCycles, stats.workerAnalyzedActiveSeconds,
             stats.workerGamesAnalyzed))},
        {"production_visits", static_cast<double>(stats.workerVisits)},
        {"total_duration_ms", stats.workerDurationMs},
        {"average_duration_ms",
         optionalNumber(averageDuration(stats.workerDurationMs,
                                        stats.workerCycles))},
        {"gap_durations_ms", numberArray(stats.workerGapDurationsMs)},
        {"median_gap_ms", optionalNumber(
                              medianMacroGapMs(stats.workerGapDurationsMs))},
        {"p90_gap_ms", optionalNumber(
                           p90MacroGapMs(stats.workerGapDurationsMs))},
        {"longest_gap_ms",
         optionalNumber(longestGap(stats.workerGapDurationsMs))},
        {"gaps_over_10s_per_game",
         optionalNumber(gapsOverPerGame(stats.workerGapDurationsMs, 10000.0,
                                        stats.workerGamesAnalyzed))},
        {"gaps_over_20s_per_game",
         optionalNumber(gapsOverPerGame(stats.workerGapDurationsMs, 20000.0,
                                        stats.workerGamesAnalyzed))},
    };
    value["army_macro"] = json::Value::Object{
        {"games_analyzed", static_cast<double>(stats.armyGamesAnalyzed)},
        {"games_unavailable",
         static_cast<double>(stats.armyGamesUnavailable)},
        {"analyzed_active_seconds", stats.armyAnalyzedActiveSeconds},
        {"cycles", static_cast<double>(stats.armyCycles)},
        {"cycles_per_minute",
         optionalNumber(pooledRatePerMinute(
             stats.armyCycles, stats.armyAnalyzedActiveSeconds,
             stats.armyGamesAnalyzed))},
        {"production_visits", static_cast<double>(stats.armyVisits)},
        {"total_duration_ms", stats.armyDurationMs},
        {"average_duration_ms",
         optionalNumber(averageDuration(stats.armyDurationMs,
                                        stats.armyCycles))},
        {"gap_durations_ms", numberArray(stats.armyGapDurationsMs)},
        {"median_gap_ms", optionalNumber(
                              medianMacroGapMs(stats.armyGapDurationsMs))},
        {"p90_gap_ms", optionalNumber(
                           p90MacroGapMs(stats.armyGapDurationsMs))},
        {"longest_gap_ms",
         optionalNumber(longestGap(stats.armyGapDurationsMs))},
        {"gaps_over_10s_per_game",
         optionalNumber(gapsOverPerGame(stats.armyGapDurationsMs, 10000.0,
                                        stats.armyGamesAnalyzed))},
        {"gaps_over_20s_per_game",
         optionalNumber(gapsOverPerGame(stats.armyGapDurationsMs, 20000.0,
                                        stats.armyGamesAnalyzed))},
    };
    value["army_control_groups"] = json::Value::Object{
        {"games_analyzed",
         static_cast<double>(stats.armyControlGroupGamesAnalyzed)},
        {"games_unavailable",
         static_cast<double>(stats.armyControlGroupGamesUnavailable)},
        {"active_seconds", stats.armyControlGroupActiveSeconds},
        {"assignments", static_cast<double>(stats.assignments)},
        {"additions", static_cast<double>(stats.additions)},
        {"edits_per_minute",
         optionalNumber(pooledRatePerMinute(
             stats.assignments + stats.additions,
             stats.armyControlGroupActiveSeconds,
             stats.armyControlGroupGamesAnalyzed))},
    };
    value["army_commands"] = json::Value::Object{
        {"games_analyzed",
         static_cast<double>(stats.armyCommandGamesAnalyzed)},
        {"games_unavailable",
         static_cast<double>(stats.armyCommandGamesUnavailable)},
        {"analyzed_active_seconds", stats.armyCommandAnalyzedActiveSeconds},
        {"command_count", static_cast<double>(stats.armyCommandCount)},
        {"gap_durations_ms", numberArray(stats.armyCommandGapDurationsMs)},
        {"commands_per_minute",
         optionalNumber(pooledRatePerMinute(
             stats.armyCommandCount, stats.armyCommandAnalyzedActiveSeconds,
             stats.armyCommandGamesAnalyzed))},
        {"median_gap_ms",
         optionalNumber(medianMacroGapMs(stats.armyCommandGapDurationsMs))},
        {"p90_gap_ms",
         optionalNumber(p90MacroGapMs(stats.armyCommandGapDurationsMs))},
        {"longest_gap_ms",
         optionalNumber(longestGap(stats.armyCommandGapDurationsMs))},
    };
    value["ability_activity"] = json::Value::Object{
        {"games_analyzed", static_cast<double>(stats.abilityGamesAnalyzed)},
        {"games_unavailable",
         static_cast<double>(stats.abilityGamesUnavailable)},
        {"analyzed_active_seconds", stats.abilityAnalyzedActiveSeconds},
        {"total_uses", static_cast<double>(stats.totalAbilityUses)},
        {"abilities_per_minute",
         optionalNumber(pooledRatePerMinute(
             stats.totalAbilityUses, stats.abilityAnalyzedActiveSeconds,
             stats.abilityGamesAnalyzed))},
    };
    value["multitasking"] = json::Value::Object{
        {"games_analyzed",
         static_cast<double>(stats.multitaskingGamesAnalyzed)},
        {"games_unavailable",
         static_cast<double>(stats.multitaskingGamesUnavailable)},
        {"total_diversity_across_active_windows",
         static_cast<double>(stats.totalDiversityAcrossActiveWindows)},
        {"active_window_count", static_cast<double>(stats.activeWindowCount)},
        {"peak_diversity", optionalNumber(peakDiversity(stats))},
        {"average_active_diversity",
         optionalNumber(averageActiveDiversity(stats))},
    };
    value["scouting"] = json::Value::Object{
        {"confirmed_units", static_cast<double>(stats.scoutingUnits)},
    };
    return value;
}

PersistedSessionStats persistedStatsFromJson(const json::Value& value) {
    PersistedSessionStats stats;
    stats.games =
        static_cast<std::uint64_t>(value["games"].asNumber());
    stats.activeSeconds = value["active_seconds"].asNumber();
    stats.controlGroupJumps = static_cast<std::uint64_t>(
        value["navigation"]["control_group_jumps"].asNumber());
    stats.locationHotkeyJumps = static_cast<std::uint64_t>(
        value["navigation"]["location_hotkey_jumps"].asNumber());
    stats.minimapJumps = static_cast<std::uint64_t>(
        value["navigation"]["minimap_jumps"].asNumber());
    stats.edgePans = static_cast<std::uint64_t>(
        value["navigation"]["edge_pans"].asNumber());
    stats.workerGamesAnalyzed = static_cast<std::uint64_t>(
        value["worker_macro"]["games_analyzed"].asNumber());
    stats.workerGamesUnavailable = static_cast<std::uint64_t>(
        value["worker_macro"]["games_unavailable"].asNumber());
    stats.workerAnalyzedActiveSeconds =
        value["worker_macro"]["analyzed_active_seconds"].asNumber();
    stats.workerCycles = static_cast<std::uint64_t>(
        value["worker_macro"]["cycles"].asNumber());
    stats.workerVisits = static_cast<std::uint64_t>(
        value["worker_macro"]["production_visits"].asNumber());
    stats.workerDurationMs =
        value["worker_macro"]["total_duration_ms"].asNumber();
    stats.workerGapDurationsMs =
        numberArray(value["worker_macro"]["gap_durations_ms"]);
    stats.armyGamesAnalyzed = static_cast<std::uint64_t>(
        value["army_macro"]["games_analyzed"].asNumber());
    stats.armyGamesUnavailable = static_cast<std::uint64_t>(
        value["army_macro"]["games_unavailable"].asNumber());
    stats.armyAnalyzedActiveSeconds =
        value["army_macro"]["analyzed_active_seconds"].asNumber();
    stats.armyCycles = static_cast<std::uint64_t>(
        value["army_macro"]["cycles"].asNumber());
    stats.armyVisits = static_cast<std::uint64_t>(
        value["army_macro"]["production_visits"].asNumber());
    stats.armyDurationMs =
        value["army_macro"]["total_duration_ms"].asNumber();
    stats.armyGapDurationsMs =
        numberArray(value["army_macro"]["gap_durations_ms"]);
    stats.armyControlGroupGamesAnalyzed = static_cast<std::uint64_t>(
        value["army_control_groups"]["games_analyzed"].asNumber());
    stats.armyControlGroupGamesUnavailable = static_cast<std::uint64_t>(
        value["army_control_groups"]["games_unavailable"].asNumber());
    stats.armyControlGroupActiveSeconds =
        value["army_control_groups"]["active_seconds"].asNumber();
    stats.assignments = static_cast<std::uint64_t>(
        value["army_control_groups"]["assignments"].asNumber());
    stats.additions = static_cast<std::uint64_t>(
        value["army_control_groups"]["additions"].asNumber());
    stats.scoutingUnits = static_cast<std::uint64_t>(
        value["scouting"]["confirmed_units"].asNumber());
    stats.armyCommandGamesAnalyzed = static_cast<std::uint64_t>(
        value["army_commands"]["games_analyzed"].asNumber());
    stats.armyCommandGamesUnavailable = static_cast<std::uint64_t>(
        value["army_commands"]["games_unavailable"].asNumber());
    stats.armyCommandAnalyzedActiveSeconds =
        value["army_commands"]["analyzed_active_seconds"].asNumber();
    stats.armyCommandCount = static_cast<std::uint64_t>(
        value["army_commands"]["command_count"].asNumber());
    stats.armyCommandGapDurationsMs =
        numberArray(value["army_commands"]["gap_durations_ms"]);
    stats.abilityGamesAnalyzed = static_cast<std::uint64_t>(
        value["ability_activity"]["games_analyzed"].asNumber());
    stats.abilityGamesUnavailable = static_cast<std::uint64_t>(
        value["ability_activity"]["games_unavailable"].asNumber());
    stats.abilityAnalyzedActiveSeconds =
        value["ability_activity"]["analyzed_active_seconds"].asNumber();
    stats.totalAbilityUses = static_cast<std::uint64_t>(
        value["ability_activity"]["total_uses"].asNumber());
    stats.multitaskingGamesAnalyzed = static_cast<std::uint64_t>(
        value["multitasking"]["games_analyzed"].asNumber());
    stats.multitaskingGamesUnavailable = static_cast<std::uint64_t>(
        value["multitasking"]["games_unavailable"].asNumber());
    stats.totalDiversityAcrossActiveWindows = static_cast<std::uint64_t>(
        value["multitasking"]["total_diversity_across_active_windows"]
            .asNumber());
    stats.activeWindowCount = static_cast<std::uint64_t>(
        value["multitasking"]["active_window_count"].asNumber());
    stats.peakDiversity =
        value["multitasking"]["peak_diversity"].asInt();
    return stats;
}

char raceLetter(std::string race) {
    race.erase(
        std::remove_if(race.begin(), race.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }),
        race.end());
    if (race.empty())
        return '\0';
    const char first = static_cast<char>(
        std::toupper(static_cast<unsigned char>(race.front())));
    return first == 'P' || first == 'T' || first == 'Z' ? first : '\0';
}

std::string deriveLatestMatchup(const AutomaticSessionState& session) {
    if (!session.lastGame() || !session.lastGameProduction())
        return "Unknown";
    try {
        const auto extraction =
            extractReplayWithBundledScrep(defaultLastReplayPath());
        if (!extraction.available)
            return "Unknown";

        int playerId =
            session.lastGameProduction()->replayCorrelation.playerId;
        if (playerId < 0) {
            const auto match = identifyReplayPlayer(
                session.lastGame()->mechanicalEvents, extraction.replay);
            if (!match.available)
                return "Unknown";
            playerId = match.playerId;
        }

        const auto player = std::find_if(
            extraction.replay.players.begin(),
            extraction.replay.players.end(),
            [&](const ReplayPlayer& candidate) {
                return candidate.id == playerId;
            });
        if (player == extraction.replay.players.end())
            return "Unknown";
        const char ownRace = raceLetter(player->race);
        if (!ownRace)
            return "Unknown";

        char opponentRace = '\0';
        int opponents = 0;
        for (const auto& candidate : extraction.replay.players) {
            if (candidate.id == playerId || candidate.slotId < 0)
                continue;
            const bool occupied = std::any_of(
                extraction.replay.startLocations.begin(),
                extraction.replay.startLocations.end(),
                [&](const ReplayStartLocation& start) {
                    return start.slotId == candidate.slotId;
                });
            if (!occupied)
                continue;
            const char race = raceLetter(candidate.race);
            if (!race)
                continue;
            opponentRace = race;
            ++opponents;
        }
        if (opponents != 1)
            return "Unknown";

        std::string matchup;
        matchup.push_back(ownRace);
        matchup.push_back('v');
        matchup.push_back(opponentRace);
        return matchup;
    } catch (...) {
        return "Unknown";
    }
}

void writeAtomicText(const std::filesystem::path& path,
                     const std::string& text,
                     const char* createError,
                     const char* writeError,
                     const char* finalizeError) {
    std::filesystem::create_directories(path.parent_path());
    const auto temporary =
        std::filesystem::path(path.string() + ".tmp");
    try {
        std::ofstream output(
            temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error(createError);
        output.write(text.data(),
                     static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output)
            throw std::runtime_error(writeError);
        output.close();
        if (!MoveFileExW(
                temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error(
                std::string(finalizeError) + ": " +
                std::system_category().message(
                    static_cast<int>(GetLastError())));
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::string formatSeconds(double seconds) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(1) << seconds << " s";
    return output.str();
}

std::string formatRate(double rate) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(1) << rate;
    return output.str();
}

std::string formatDurationMs(std::optional<double> ms) {
    if (!ms)
        return "N/A";
    std::ostringstream output;
    output << std::fixed << std::setprecision(2)
           << (*ms / 1000.0) << " s";
    return output.str();
}

void writeMatchupRow(std::ostringstream& output,
                     const std::string& label,
                     const std::string& value) {
    output << std::left << std::setw(40) << label << std::right
           << std::setw(12) << value << '\n';
}

void appendMatchupSummary(
    std::ostringstream& output,
    const std::map<std::string, PersistedSessionStats>& matchups,
    const ReportGroupVisibility& visibility) {
    if (matchups.empty())
        return;

    output << "\n============================================================\n"
           << "MATCHUP BREAKDOWN\n"
           << "============================================================\n";
    for (const auto& [matchup, stats] : matchups) {
        output << "\n" << matchup << "\n\n";
        writeMatchupRow(output, "Games", std::to_string(stats.games));
        writeMatchupRow(output, "Active time",
                        formatSeconds(stats.activeSeconds));

        if (visibility.cameraNavigation) {
            output << "\nNAVIGATION\n\n";
            writeMatchupRow(
                output, "Navigation transitions/min",
                formatRate(transitionsPerMinute(stats)));
            writeMatchupRow(
                output, "Control-group jumps",
                std::to_string(stats.controlGroupJumps));
            writeMatchupRow(
                output, "Location-hotkey jumps",
                std::to_string(stats.locationHotkeyJumps));
            writeMatchupRow(output, "Minimap jumps",
                            std::to_string(stats.minimapJumps));
            writeMatchupRow(output, "Edge pans",
                            std::to_string(stats.edgePans));
        }

        if (visibility.workerMacroCycles) {
            output << "\nWORKER MACRO\n\n";
            writeMatchupRow(output, "Cycles",
                            std::to_string(stats.workerCycles));
            writeMatchupRow(
                output, "Average",
                formatDurationMs(averageDuration(
                    stats.workerDurationMs, stats.workerCycles)));
            writeMatchupRow(output, "Production visits",
                            std::to_string(stats.workerVisits));
        }

        if (visibility.armyMacroCycles) {
            output << "\nARMY MACRO\n\n";
            writeMatchupRow(output, "Cycles",
                            std::to_string(stats.armyCycles));
            writeMatchupRow(
                output, "Average",
                formatDurationMs(averageDuration(
                    stats.armyDurationMs, stats.armyCycles)));
            writeMatchupRow(output, "Production visits",
                            std::to_string(stats.armyVisits));
        }

        if (visibility.armyControlGroupManagement) {
            output << "\nARMY CONTROL-GROUP MANAGEMENT\n\n";
            writeMatchupRow(output, "Assignments",
                            std::to_string(stats.assignments));
            writeMatchupRow(output, "Additions",
                            std::to_string(stats.additions));
            writeMatchupRow(output, "Edits / min",
                            formatRate(editsPerMinute(stats)));
        }

        if (visibility.scoutingUnitActivity) {
            output << "\nSCOUTING\n\n";
            writeMatchupRow(output, "Confirmed scouting units",
                            std::to_string(stats.scoutingUnits));
        }
    }
}

void writeSessionData(const std::filesystem::path& summaryPath,
                      const AutomaticSessionState& session) {
    const auto dataPath = sessionDataPath(summaryPath);
    json::Value root(json::Value::Object{});
    json::Value::Array games;
    if (std::filesystem::is_regular_file(dataPath)) {
        try {
            const auto previous = json::parseFile(dataPath);
            if (previous["games"].isArray())
                games = previous["games"].asArray();
        } catch (...) {
        }
    }

    const auto totalGames =
        static_cast<std::size_t>(session.stats().games);
    if (games.size() < totalGames && session.lastGame() &&
        session.lastGameProduction()) {
        const auto oneGame = automaticSessionStatsForGame(
            *session.lastGame(), *session.lastGameProduction());
        while (games.size() + 1 < totalGames) {
            json::Value missing(json::Value::Object{});
            missing["ordinal"] =
                static_cast<double>(games.size() + 1);
            missing["matchup"] = "Unknown";
            missing["stats"] = persistedStatsToJson({});
            games.push_back(std::move(missing));
        }
        json::Value game(json::Value::Object{});
        game["ordinal"] = static_cast<double>(totalGames);
        game["matchup"] = deriveLatestMatchup(session);
        game["stats"] =
            persistedStatsToJson(persistedStats(oneGame));
        games.push_back(std::move(game));
    } else if (games.size() > totalGames) {
        games.resize(totalGames);
    }

    std::map<std::string, PersistedSessionStats> matchups;
    for (const auto& game : games) {
        const std::string matchup =
            game["matchup"].asString("Unknown");
        merge(matchups[matchup],
              persistedStatsFromJson(game["stats"]));
    }

    root["schema_version"] = 3;
    root["session_id"] = summarySortKey(summaryPath);
    root["overall"] =
        persistedStatsToJson(persistedStats(session.stats()));
    root["games"] = std::move(games);
    json::Value matchupObject(json::Value::Object{});
    for (const auto& [name, stats] : matchups)
        matchupObject[name] = persistedStatsToJson(stats);
    root["matchups"] = std::move(matchupObject);

    writeAtomicText(
        dataPath, json::stringify(root, 2) + "\n",
        "Unable to create automatic session data",
        "Unable to write automatic session data",
        "Unable to finalize automatic session data");
}

std::map<std::string, PersistedSessionStats>
readMatchups(const std::filesystem::path& summaryPath) {
    std::map<std::string, PersistedSessionStats> result;
    try {
        const auto root = json::parseFile(sessionDataPath(summaryPath));
        for (const auto& [name, value] :
             root["matchups"].asObject()) {
            result.emplace(name, persistedStatsFromJson(value));
        }
    } catch (...) {
    }
    return result;
}

} // namespace

AutomaticRecordingDiscardResult discardAbortedAutomaticRecordingFiles(
    const AutomaticRecordingArtifacts& artifacts) {
    AutomaticRecordingDiscardResult result;
    std::set<std::filesystem::path> paths;
    addArtifact(paths, artifacts.navPath);
    addArtifact(paths, artifacts.jsonPath);
    addArtifact(paths, artifacts.rawPath);
    if (!artifacts.navPath.empty()) {
        addArtifact(
            paths,
            std::filesystem::path(artifacts.navPath.string() + ".tmp"));
        auto derivedJson = artifacts.navPath;
        derivedJson.replace_extension(".json");
        addArtifact(paths, derivedJson);
        addArtifact(
            paths,
            std::filesystem::path(derivedJson.string() + ".tmp"));
    }
    if (!artifacts.jsonPath.empty())
        addArtifact(
            paths,
            std::filesystem::path(artifacts.jsonPath.string() + ".tmp"));

    for (const auto& path : paths) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            result.failedPaths.push_back(path);
            continue;
        }
        if (!exists)
            continue;
        if (!std::filesystem::is_regular_file(path, error) || error ||
            !std::filesystem::remove(path, error) || error) {
            result.failedPaths.push_back(path);
            continue;
        }
        ++result.removedFiles;
    }
    return result;
}

std::filesystem::path makeAutomaticSessionSummaryPath(
    const std::filesystem::path& sessionsRoot,
    std::chrono::system_clock::time_point sessionStart) {
    const auto base = automaticSessionStartId(sessionStart);
    auto candidate = sessionsRoot /
                     (base + std::string(automaticSessionSummarySuffix));
    for (std::size_t suffix = 1;
         std::filesystem::exists(candidate); ++suffix) {
        candidate = sessionsRoot /
                    (base + "_" + std::to_string(suffix) +
                     std::string(automaticSessionSummarySuffix));
    }
    return candidate;
}

void writeAutomaticSessionSummary(
    const std::filesystem::path& summaryPath,
    const std::string& report) {
    writeAtomicText(
        summaryPath, report,
        "Unable to create automatic session summary",
        "Unable to write automatic session summary",
        "Unable to finalize automatic session summary");
}

void writeAutomaticSessionSummary(
    const std::filesystem::path& summaryPath,
    const AutomaticSessionState& session,
    const ReportVisibilityProvider& currentReportVisibility) {
    const ReportGroupVisibility visibility =
        currentReportVisibility ? currentReportVisibility()
                                : ReportGroupVisibility{};
    writeSessionData(summaryPath, session);
    std::ostringstream report;
    report << formatAutomaticSessionReport(session, visibility);
    appendMatchupSummary(report, readMatchups(summaryPath), visibility);
    writeAutomaticSessionSummary(summaryPath, report.str());
}

std::optional<std::filesystem::path>
findLatestAutomaticSessionSummary(
    const std::filesystem::path& sessionsRoot) {
    std::error_code error;
    if (!std::filesystem::is_directory(sessionsRoot, error) || error)
        return std::nullopt;

    std::optional<std::filesystem::path> latest;
    std::string latestKey;
    std::filesystem::recursive_directory_iterator iterator(
        sessionsRoot,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto& entry = *iterator;
        std::error_code entryError;
        if (entry.is_regular_file(entryError) && !entryError &&
            isAutomaticSessionSummary(entry.path())) {
            const auto key = summarySortKey(entry.path());
            if (!latest || key > latestKey ||
                (key == latestKey && entry.path() > *latest)) {
                latest = entry.path();
                latestKey = key;
            }
        }
        iterator.increment(error);
    }
    return latest;
}

} // namespace smp
