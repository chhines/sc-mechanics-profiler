#include "analysis/production_visit.h"
#include "analysis/replay_analysis.h"
#include "storage/session.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <sstream>

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

std::string contextName(const smp::ProductionContextId& context) {
    std::ostringstream output;
    output << smp::productionContextKindName(context.kind);
    if (context.kind == smp::ProductionContextKind::ReplaySelection) {
        output << ':';
        for (std::size_t index = 0; index < context.unitTags.size(); ++index) {
            if (index > 0)
                output << ',';
            output << context.unitTags[index];
        }
    } else if (context.kind == smp::ProductionContextKind::ControlGroup) {
        output << ':' << context.controlGroup << ":g" << context.assignmentGeneration;
    } else if (context.kind == smp::ProductionContextKind::LocationHotkey) {
        output << ':' << context.locationHotkey << ":g" << context.assignmentGeneration;
    }
    return output.str();
}

void printRepeatedContextSplits(const smp::ProductMacroCycleAnalysis& cycles,
                                const std::vector<smp::ProductionVisit>& visits,
                                smp::MacroProductType productType) {
    for (const auto splitVisitIndex : cycles.repeatedContextSplitVisitIndices) {
        if (splitVisitIndex >= visits.size())
            continue;
        const auto cycle = std::find_if(
            cycles.cycles.begin(), cycles.cycles.end(), [&](const smp::MacroCycle& candidate) {
                return std::find(candidate.visitIndices.begin(), candidate.visitIndices.end(),
                                 splitVisitIndex) != candidate.visitIndices.end();
            });
        if (cycle == cycles.cycles.end() || cycle == cycles.cycles.begin())
            continue;
        const auto& previousCycle = *(cycle - 1);
        const auto& repeatedVisit = visits[splitVisitIndex];
        const smp::ProductionVisit* previousSameContext = nullptr;
        std::ostringstream previousContexts;
        for (std::size_t position = 0; position < previousCycle.visitIndices.size(); ++position) {
            const auto previousVisitIndex = previousCycle.visitIndices[position];
            if (previousVisitIndex >= visits.size())
                continue;
            const auto& previousVisit = visits[previousVisitIndex];
            if (position > 0)
                previousContexts << '|';
            previousContexts << contextName(previousVisit.productionContext);
            if (smp::sameProductionContext(previousVisit.productionContext,
                                           repeatedVisit.productionContext))
                previousSameContext = &previousVisit;
        }
        std::cout << "repeated_context_split"
                  << " product=" << smp::macroProductTypeName(productType)
                  << " timestamp_ms=" << repeatedVisit.contextActiveMs
                  << " previous_cycle_contexts=" << previousContexts.str()
                  << " repeated_context=" << contextName(repeatedVisit.productionContext)
                  << " access_method="
                  << smp::productionAccessMethodName(repeatedVisit.accessMethod)
                  << " context_kind="
                  << smp::productionContextKindName(repeatedVisit.productionContext.kind)
                  << " old_grouping=merged new_grouping=split";
        if (previousSameContext) {
            std::cout << " previous_context_ms=" << previousSameContext->contextActiveMs
                      << " previous_end_ms=" << previousSameContext->endActiveMs
                      << " gap_ms="
                      << repeatedVisit.contextActiveMs - previousSameContext->endActiveMs;
        }
        std::cout << '\n';
    }
}

void printAssignmentInterruptionSplits(
    const smp::ProductMacroCycleAnalysis& cycles,
    const std::vector<smp::ProductionVisit>& visits,
    smp::MacroProductType productType) {
    for (const auto& split : cycles.assignmentInterruptionSplitDetails) {
        if (split.previousVisitIndex >= visits.size() || split.nextVisitIndex >= visits.size())
            continue;
        const auto& previous = visits[split.previousVisitIndex];
        const auto& next = visits[split.nextVisitIndex];
        std::cout << "assignment_interruption_split"
                  << " product=" << smp::macroProductTypeName(productType)
                  << " previous_visit=" << split.previousVisitIndex
                  << " next_visit=" << split.nextVisitIndex
                  << " interrupt_type=" << mechanicalTypeName(split.interruptionType)
                  << " interrupt_timestamp_ms=" << split.interruptionActiveMs
                  << " previous_start_ms=" << previous.startActiveMs
                  << " previous_first_production_ms="
                  << previous.firstProductionActiveMs
                  << " previous_end_ms=" << previous.endActiveMs
                  << " previous_execution_duration_ms="
                  << previous.executionDurationMs
                  << " next_start_ms=" << next.startActiveMs
                  << " next_context_ms=" << next.contextActiveMs
                  << " next_first_production_ms=" << next.firstProductionActiveMs
                  << " next_end_ms=" << next.endActiveMs
                  << " next_execution_duration_ms=" << next.executionDurationMs
                  << '\n';
    }
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
        const smp::ProductionVisit* workerSpamVisit = nullptr;
        for (const auto& visit : production.productionVisits) {
            confirmed += visit.replayConfirmed ? 1U : 0U;
            ++access[static_cast<std::size_t>(visit.accessMethod)];
            physicalPresses += static_cast<std::size_t>(visit.physicalProductionPresses);
            maximumVisitDurationMs = std::max(maximumVisitDurationMs, visit.durationMs);
            if (!firstControlGroupFour && visit.controlGroup == 4 && visit.replayConfirmed)
                firstControlGroupFour = &visit;
            if (visit.productType == smp::MacroProductType::Worker &&
                visit.physicalProductionPresses >= 30 &&
                (!workerSpamVisit ||
                 visit.physicalProductionPresses > workerSpamVisit->physicalProductionPresses))
                workerSpamVisit = &visit;
        }
        std::cout << "parser=" << replay.parser << '\n'
                  << "heuristic_production_visits=" << heuristicVisits << '\n'
                  << "replay_confirmed_production_visits=" << confirmed << '\n'
                  << "replay_created_control_group_visits="
                  << production.replayCorrelation.replayCreatedControlGroupVisits << '\n'
                  << "click_visits=" << production.replayCorrelation.matchedClickVisits << '\n'
                  << "worker_cycles=" << production.workerMacroCycles.cycles.size() << '\n'
                  << "worker_average_duration_ms="
                  << production.workerMacroCycles.averageDurationMs.value_or(0.0) << '\n'
                  << "worker_best_duration_ms="
                  << production.workerMacroCycles.bestDurationMs.value_or(0.0) << '\n'
                  << "worker_slowest_duration_ms="
                  << production.workerMacroCycles.slowestDurationMs.value_or(0.0) << '\n'
                  << "army_cycles=" << production.armyMacroCycles.cycles.size() << '\n'
                  << "army_average_duration_ms="
                  << production.armyMacroCycles.averageDurationMs.value_or(0.0) << '\n'
                  << "army_best_duration_ms="
                  << production.armyMacroCycles.bestDurationMs.value_or(0.0) << '\n'
                  << "army_slowest_duration_ms="
                  << production.armyMacroCycles.slowestDurationMs.value_or(0.0) << '\n'
                  << "worker_repeated_context_splits="
                  << production.workerMacroCycles.repeatedContextSplits << '\n'
                  << "army_repeated_context_splits="
                  << production.armyMacroCycles.repeatedContextSplits << '\n'
                  << "worker_assignment_interruption_splits="
                  << production.workerMacroCycles.assignmentInterruptionSplits << '\n'
                  << "army_assignment_interruption_splits="
                  << production.armyMacroCycles.assignmentInterruptionSplits << '\n'
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
        printRepeatedContextSplits(production.workerMacroCycles,
                                   production.productionVisits,
                                   smp::MacroProductType::Worker);
        printRepeatedContextSplits(production.armyMacroCycles,
                                   production.productionVisits,
                                   smp::MacroProductType::Army);
        printAssignmentInterruptionSplits(production.workerMacroCycles,
                                          production.productionVisits,
                                          smp::MacroProductType::Worker);
        printAssignmentInterruptionSplits(production.armyMacroCycles,
                                          production.productionVisits,
                                          smp::MacroProductType::Army);
        if (workerSpamVisit) {
            std::cout << "worker_spam_visit"
                      << " start_ms=" << workerSpamVisit->startActiveMs
                      << " context_ms=" << workerSpamVisit->contextActiveMs
                      << " first_production_ms="
                      << workerSpamVisit->firstProductionActiveMs
                      << " end_ms=" << workerSpamVisit->endActiveMs
                      << " execution_duration_ms="
                      << workerSpamVisit->executionDurationMs
                      << " burst_span_ms=" << workerSpamVisit->productionBurstSpanMs
                      << " full_span_ms=" << workerSpamVisit->durationMs
                      << " physical_presses="
                      << workerSpamVisit->physicalProductionPresses
                      << " replay_commands=" << workerSpamVisit->replayProductionCommands
                      << '\n';
        }
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
