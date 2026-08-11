#include "analysis/production_visit.h"
#include "analysis/replay_analysis.h"
#include "storage/session.h"

#include <array>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: replay_integration_smoke <session.nav> <LastReplay.rep>\n";
        return 2;
    }
    try {
        const auto session = smp::readNavSession(std::filesystem::path(argv[1]));
        const auto hotkeys = smp::loadStarCraftHotkeyProfile();
        auto production =
            smp::analyzeProductionVisits(session.analysis, hotkeys, session.qpcFrequency);
        const auto heuristicVisits = production.productionVisits.size();
        const auto executableDirectory =
            std::filesystem::absolute(std::filesystem::path(argv[0])).parent_path();
        const auto parserDestination =
            executableDirectory / "validation-tools" / "screp-v1.13.3.exe";
        const auto replay = smp::extractReplayWithBundledScrepForValidation(
            std::filesystem::path(argv[2]), parserDestination);
        if (!replay.available) {
            std::cerr << "parser=" << replay.parser << "\nerror="
                      << replay.unavailableReason << '\n';
            return 3;
        }
        production = smp::correlateProductionVisitsWithReplay(
            session.analysis, hotkeys, session.qpcFrequency, std::move(production),
            replay.replay, replay.parser);
        std::array<std::size_t, 4> access{};
        std::size_t confirmed = 0;
        for (const auto& visit : production.productionVisits) {
            confirmed += visit.replayConfirmed ? 1U : 0U;
            ++access[static_cast<std::size_t>(visit.accessMethod)];
        }
        std::cout << "parser=" << replay.parser << '\n'
                  << "heuristic_production_visits=" << heuristicVisits << '\n'
                  << "replay_confirmed_production_visits=" << confirmed << '\n'
                  << "replay_created_control_group_visits="
                  << production.replayCorrelation.replayCreatedControlGroupVisits << '\n'
                  << "click_visits=" << production.replayCorrelation.matchedClickVisits << '\n'
                  << "worker_cycles=" << production.workerMacroCycles.cycles.size() << '\n'
                  << "army_cycles=" << production.armyMacroCycles.cycles.size() << '\n'
                  << "access_control_group=" << access[0] << '\n'
                  << "access_location_click=" << access[1] << '\n'
                  << "access_minimap_click=" << access[2] << '\n'
                  << "access_screen_click=" << access[3] << '\n'
                  << "unmatched_replay_production_events="
                  << production.replayCorrelation.unmatchedReplayProductionEvents << '\n';
        return production.replayCorrelation.available ? 0 : 4;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
