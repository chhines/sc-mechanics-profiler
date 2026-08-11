#include "analysis/production_visit.h"
#include "analysis/replay_analysis.h"
#include "storage/session.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>

namespace {

const char* mechanicalTypeName(smp::MechanicalInputType type) {
    switch (type) {
    case smp::MechanicalInputType::KeyPress:
        return "key";
    case smp::MechanicalInputType::ControlGroupSelect:
        return "control_group_select";
    case smp::MechanicalInputType::ControlGroupAssign:
        return "control_group_assign";
    case smp::MechanicalInputType::LocationRecall:
        return "location_recall";
    case smp::MechanicalInputType::LocationAssign:
        return "location_assign";
    case smp::MechanicalInputType::MouseLeftDown:
        return "mouse_left_down";
    case smp::MechanicalInputType::MouseLeftUp:
        return "mouse_left_up";
    case smp::MechanicalInputType::MouseRightDown:
        return "mouse_right_down";
    case smp::MechanicalInputType::MouseRightUp:
        return "mouse_right_up";
    case smp::MechanicalInputType::MouseMiddleDown:
        return "mouse_middle_down";
    case smp::MechanicalInputType::MouseMiddleUp:
        return "mouse_middle_up";
    case smp::MechanicalInputType::MouseWheel:
        return "mouse_wheel";
    }
    return "unknown";
}

bool isStandaloneModifier(std::uint16_t virtualKey) {
    return virtualKey == VK_CONTROL || virtualKey == VK_LCONTROL || virtualKey == VK_RCONTROL ||
           virtualKey == VK_SHIFT || virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT ||
           virtualKey == VK_MENU || virtualKey == VK_LMENU || virtualKey == VK_RMENU;
}

} // namespace

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
        std::size_t physicalPresses = 0;
        double maximumVisitDurationMs = 0.0;
        const smp::ProductionVisit* firstControlGroupFour = nullptr;
        for (const auto& visit : production.productionVisits) {
            confirmed += visit.replayConfirmed ? 1U : 0U;
            ++access[static_cast<std::size_t>(visit.accessMethod)];
            physicalPresses += static_cast<std::size_t>(visit.physicalProductionPresses);
            maximumVisitDurationMs = std::max(maximumVisitDurationMs, visit.durationMs);
            if (!firstControlGroupFour && visit.controlGroup == 4 && visit.replayConfirmed)
                firstControlGroupFour = &visit;
        }
        std::cout << "parser=" << replay.parser << '\n'
                  << "heuristic_production_visits=" << heuristicVisits << '\n'
                  << "replay_confirmed_production_visits=" << confirmed << '\n'
                  << "replay_created_control_group_visits="
                  << production.replayCorrelation.replayCreatedControlGroupVisits << '\n'
                  << "click_visits=" << production.replayCorrelation.matchedClickVisits << '\n'
                  << "worker_cycles=" << production.workerMacroCycles.cycles.size() << '\n'
                  << "army_cycles=" << production.armyMacroCycles.cycles.size() << '\n'
                  << "production_visits=" << production.productionVisits.size() << '\n'
                  << "physical_production_presses=" << physicalPresses << '\n'
                  << "maximum_production_visit_duration_ms=" << maximumVisitDurationMs << '\n'
                  << "matched_replay_production_events="
                  << production.replayCorrelation.matchedReplayProductionEvents << '\n'
                  << "access_control_group=" << access[0] << '\n'
                  << "access_location_click=" << access[1] << '\n'
                  << "access_minimap_click=" << access[2] << '\n'
                  << "access_screen_click=" << access[3] << '\n'
                  << "unmatched_replay_production_events="
                  << production.replayCorrelation.unmatchedReplayProductionEvents << '\n'
                  << "extended_production_visits="
                  << production.replayCorrelation.extendedProductionVisits << '\n'
                  << "extended_physical_production_presses="
                  << production.replayCorrelation.extendedPhysicalProductionPresses << '\n';
        if (firstControlGroupFour) {
            std::cout << "first_cg4_start_active_ms=" << firstControlGroupFour->startActiveMs << '\n'
                      << "first_cg4_end_active_ms=" << firstControlGroupFour->endActiveMs << '\n'
                      << "first_cg4_duration_ms=" << firstControlGroupFour->durationMs << '\n'
                      << "first_cg4_physical_presses="
                      << firstControlGroupFour->physicalProductionPresses << '\n'
                      << "first_cg4_replay_commands="
                      << firstControlGroupFour->replayProductionCommands << '\n';
            const auto following = std::find_if(
                session.analysis.mechanicalEvents.begin(),
                session.analysis.mechanicalEvents.end(), [&](const auto& event) {
                    if (event.timestampTicks <= firstControlGroupFour->endTimestampTicks)
                        return false;
                    return (event.type == smp::MechanicalInputType::KeyPress &&
                            !isStandaloneModifier(event.virtualKey)) ||
                           event.type == smp::MechanicalInputType::ControlGroupSelect ||
                           event.type == smp::MechanicalInputType::ControlGroupAssign ||
                           event.type == smp::MechanicalInputType::LocationRecall ||
                           event.type == smp::MechanicalInputType::LocationAssign ||
                           event.type == smp::MechanicalInputType::MouseLeftDown;
                });
            if (following != session.analysis.mechanicalEvents.end()) {
                std::cout << "first_cg4_following_event_type="
                          << mechanicalTypeName(following->type) << '\n'
                          << "first_cg4_following_event_active_ms=" << following->activeMs << '\n'
                          << "first_cg4_following_event_virtual_key=" << following->virtualKey << '\n'
                          << "first_cg4_following_event_value=" << following->value << '\n';
            }
        }
        return production.replayCorrelation.available ? 0 : 4;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
