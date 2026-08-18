#include "cli/session_history_writer.h"

#include "analysis/replay_analysis.h"
#include "platform/automatic_lifecycle.h"
#include "util/json.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <windows.h>

namespace smp {
namespace {

constexpr std::string_view sessionJsonSuffix = "_session.json";

struct PersistedSessionStats {
    std::uint64_t games{};
    double activeSeconds{};
    std::uint64_t controlGroupJumps{};
    std::uint64_t locationHotkeyJumps{};
    std::uint64_t minimapJumps{};
    std::uint64_t edgePans{};
    std::uint64_t workerCycles{};
    std::uint64_t workerVisits{};
    double workerDurationMs{};
    std::uint64_t armyCycles{};
    std::uint64_t armyVisits{};
    double armyDurationMs{};
    double armyControlGroupActiveSeconds{};
    std::uint64_t assignments{};
    std::uint64_t additions{};
    std::uint64_t scoutingUnits{};
};

std::string sessionIdFromPath(const std::filesystem::path& dataPath) {
    const auto filename = dataPath.filename().string();
    if (filename.size() <= sessionJsonSuffix.size() ||
        !filename.ends_with(sessionJsonSuffix))
        return dataPath.stem().string();
    return filename.substr(0, filename.size() - sessionJsonSuffix.size());
}

PersistedSessionStats persistedStats(const AutomaticSessionStats& stats) {
    PersistedSessionStats result;
    result.games = stats.games;
    result.activeSeconds = stats.activeSeconds;
    result.controlGroupJumps = stats.controlGroupJumps;
    result.locationHotkeyJumps = stats.locationHotkeyJumps;
    result.minimapJumps = stats.minimapJumps;
    result.edgePans = stats.edgePans;
    result.workerCycles = stats.workerMacro.cycles;
    result.workerVisits = stats.workerMacro.productionVisits;
    result.workerDurationMs = stats.workerMacro.totalDurationMs;
    result.armyCycles = stats.armyMacro.cycles;
    result.armyVisits = stats.armyMacro.productionVisits;
    result.armyDurationMs = stats.armyMacro.totalDurationMs;
    result.armyControlGroupActiveSeconds =
        stats.armyControlGroups.activeDurationSeconds;
    result.assignments = stats.armyControlGroups.assignments;
    result.additions = stats.armyControlGroups.additions;
    result.scoutingUnits = stats.armyControlGroups.scoutingUnitActivities.size();
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
    target.workerCycles += source.workerCycles;
    target.workerVisits += source.workerVisits;
    target.workerDurationMs += source.workerDurationMs;
    target.armyCycles += source.armyCycles;
    target.armyVisits += source.armyVisits;
    target.armyDurationMs += source.armyDurationMs;
    target.armyControlGroupActiveSeconds += source.armyControlGroupActiveSeconds;
    target.assignments += source.assignments;
    target.additions += source.additions;
    target.scoutingUnits += source.scoutingUnits;
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

std::optional<double> averageDuration(double totalMs,
                                      std::uint64_t cycles) {
    return cycles > 0
               ? std::optional<double>(totalMs / static_cast<double>(cycles))
               : std::nullopt;
}

json::Value optionalNumber(const std::optional<double>& value) {
    return value ? json::Value(*value) : json::Value(nullptr);
}

json::Value persistedStatsToJson(const PersistedSessionStats& stats) {
    json::Value value(json::Value::Object{});
    value["games"] = static_cast<double>(stats.games);
    value["active_seconds"] = stats.activeSeconds;
    value["navigation"] = json::Value::Object{
        {"control_group_jumps", static_cast<double>(stats.controlGroupJumps)},
        {"location_hotkey_jumps", static_cast<double>(stats.locationHotkeyJumps)},
        {"minimap_jumps", static_cast<double>(stats.minimapJumps)},
        {"edge_pans", static_cast<double>(stats.edgePans)},
        {"total_transitions", static_cast<double>(transitions(stats))},
        {"transitions_per_minute", transitionsPerMinute(stats)},
    };
    value["worker_macro"] = json::Value::Object{
        {"cycles", static_cast<double>(stats.workerCycles)},
        {"production_visits", static_cast<double>(stats.workerVisits)},
        {"total_duration_ms", stats.workerDurationMs},
        {"average_duration_ms",
         optionalNumber(averageDuration(stats.workerDurationMs,
                                        stats.workerCycles))},
    };
    value["army_macro"] = json::Value::Object{
        {"cycles", static_cast<double>(stats.armyCycles)},
        {"production_visits", static_cast<double>(stats.armyVisits)},
        {"total_duration_ms", stats.armyDurationMs},
        {"average_duration_ms",
         optionalNumber(averageDuration(stats.armyDurationMs,
                                        stats.armyCycles))},
    };
    value["army_control_groups"] = json::Value::Object{
        {"active_seconds", stats.armyControlGroupActiveSeconds},
        {"assignments", static_cast<double>(stats.assignments)},
        {"additions", static_cast<double>(stats.additions)},
        {"edits_per_minute", editsPerMinute(stats)},
    };
    value["scouting"] = json::Value::Object{
        {"confirmed_units", static_cast<double>(stats.scoutingUnits)},
    };
    return value;
}

PersistedSessionStats persistedStatsFromJson(const json::Value& value) {
    PersistedSessionStats stats;
    stats.games = static_cast<std::uint64_t>(value["games"].asNumber());
    stats.activeSeconds = value["active_seconds"].asNumber();
    stats.controlGroupJumps = static_cast<std::uint64_t>(
        value["navigation"]["control_group_jumps"].asNumber());
    stats.locationHotkeyJumps = static_cast<std::uint64_t>(
        value["navigation"]["location_hotkey_jumps"].asNumber());
    stats.minimapJumps = static_cast<std::uint64_t>(
        value["navigation"]["minimap_jumps"].asNumber());
    stats.edgePans = static_cast<std::uint64_t>(
        value["navigation"]["edge_pans"].asNumber());
    stats.workerCycles = static_cast<std::uint64_t>(
        value["worker_macro"]["cycles"].asNumber());
    stats.workerVisits = static_cast<std::uint64_t>(
        value["worker_macro"]["production_visits"].asNumber());
    stats.workerDurationMs = value["worker_macro"]["total_duration_ms"].asNumber();
    stats.armyCycles = static_cast<std::uint64_t>(
        value["army_macro"]["cycles"].asNumber());
    stats.armyVisits = static_cast<std::uint64_t>(
        value["army_macro"]["production_visits"].asNumber());
    stats.armyDurationMs = value["army_macro"]["total_duration_ms"].asNumber();
    stats.armyControlGroupActiveSeconds =
        value["army_control_groups"]["active_seconds"].asNumber();
    stats.assignments = static_cast<std::uint64_t>(
        value["army_control_groups"]["assignments"].asNumber());
    stats.additions = static_cast<std::uint64_t>(
        value["army_control_groups"]["additions"].asNumber());
    stats.scoutingUnits = static_cast<std::uint64_t>(
        value["scouting"]["confirmed_units"].asNumber());
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

        int playerId = session.lastGameProduction()->replayCorrelation.playerId;
        if (playerId < 0) {
            const auto match = identifyReplayPlayer(
                session.lastGame()->mechanicalEvents, extraction.replay);
            if (!match.available)
                return "Unknown";
            playerId = match.playerId;
        }

        const auto player = std::find_if(
            extraction.replay.players.begin(), extraction.replay.players.end(),
            [&](const ReplayPlayer& candidate) { return candidate.id == playerId; });
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

void writeAtomicJson(const std::filesystem::path& path,
                     const json::Value& root) {
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Unable to create automatic session data");
        const auto encoded = json::stringify(root, 2) + "\n";
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        output.flush();
        if (!output)
            throw std::runtime_error("Unable to write automatic session data");
        output.close();
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error(
                "Unable to finalize automatic session data: " +
                std::system_category().message(static_cast<int>(GetLastError())));
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace

void writeAutomaticSessionHistoryJson(
    const std::filesystem::path& dataPath,
    const AutomaticSessionState& session) {
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

    const auto totalGames = static_cast<std::size_t>(session.stats().games);
    if (games.size() < totalGames && session.lastGame() &&
        session.lastGameProduction()) {
        const auto oneGame = automaticSessionStatsForGame(
            *session.lastGame(), *session.lastGameProduction());
        while (games.size() + 1 < totalGames) {
            json::Value missing(json::Value::Object{});
            missing["ordinal"] = static_cast<double>(games.size() + 1);
            missing["matchup"] = "Unknown";
            missing["stats"] = persistedStatsToJson({});
            games.push_back(std::move(missing));
        }
        json::Value game(json::Value::Object{});
        game["ordinal"] = static_cast<double>(totalGames);
        game["matchup"] = deriveLatestMatchup(session);
        game["stats"] = persistedStatsToJson(persistedStats(oneGame));
        games.push_back(std::move(game));
    } else if (games.size() > totalGames) {
        games.resize(totalGames);
    }

    std::map<std::string, PersistedSessionStats> matchups;
    for (const auto& game : games) {
        const std::string matchup = game["matchup"].asString("Unknown");
        merge(matchups[matchup], persistedStatsFromJson(game["stats"]));
    }

    root["schema_version"] = 1;
    root["session_id"] = sessionIdFromPath(dataPath);
    root["overall"] = persistedStatsToJson(persistedStats(session.stats()));
    root["games"] = std::move(games);
    json::Value matchupObject(json::Value::Object{});
    for (const auto& [name, stats] : matchups)
        matchupObject[name] = persistedStatsToJson(stats);
    root["matchups"] = std::move(matchupObject);

    writeAtomicJson(dataPath, root);
}

} // namespace smp
