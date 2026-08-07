#include "storage/session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace scm {
namespace {

struct BinaryHeader {
    char magic[8]{};
    std::uint32_t schemaVersion{1};
    std::uint32_t eventSize{};
    std::uint64_t qpcFrequency{};
};

std::string makeSessionId() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d_%H%M%S");
    return output.str();
}

json::Value optionalJson(const std::optional<double>& value) {
    return value ? json::Value(*value) : json::Value(nullptr);
}

json::Value distributionJson(const Distribution& value) {
    return json::Value::Object{{"count", static_cast<double>(value.count)},
                               {"median", optionalJson(value.median)},
                               {"p75", optionalJson(value.p75)},
                               {"p90", optionalJson(value.p90)},
                               {"p95", optionalJson(value.p95)},
                               {"mad", optionalJson(value.mad)},
                               {"mean", optionalJson(value.mean)},
                               {"maximum", optionalJson(value.maximum)}};
}

std::vector<double> navigationCosts(const AnalysisResult& result, NavigationMethod method) {
    std::vector<double> values;
    for (const auto& item : result.navigation) {
        if (item.method == method && item.firstActionLatencyMs)
            values.push_back(item.durationMs + *item.firstActionLatencyMs);
    }
    return values;
}

json::Value navigationJson(const AnalysisResult& result, NavigationMethod method) {
    std::size_t count = 0;
    std::vector<double> durations, firstActions, firstSelections, recoveries;
    json::Value::Array observations;
    for (const auto& item : result.navigation) {
        if (item.method != method)
            continue;
        ++count;
        durations.push_back(item.durationMs);
        if (item.firstActionLatencyMs)
            firstActions.push_back(*item.firstActionLatencyMs);
        if (item.firstSelectionLatencyMs)
            firstSelections.push_back(*item.firstSelectionLatencyMs);
        if (item.cursorRecoveryDistance)
            recoveries.push_back(*item.cursorRecoveryDistance);
        observations.emplace_back(
            json::Value::Object{{"completion_active_ms", item.completionActiveMs},
                                {"duration_ms", item.durationMs},
                                {"first_action_latency_ms", optionalJson(item.firstActionLatencyMs)},
                                {"first_selection_latency_ms", optionalJson(item.firstSelectionLatencyMs)},
                                {"cursor_recovery_distance_px", optionalJson(item.cursorRecoveryDistance)},
                                {"cursor_x", item.cursorX},
                                {"cursor_y", item.cursorY}});
    }
    const double minutes = result.activeDurationSeconds / 60.0;
    return json::Value::Object{{"count", static_cast<double>(count)},
                               {"events_per_minute", minutes > 0.0 ? static_cast<double>(count) / minutes : 0.0},
                               {"duration_ms", distributionJson(describe(durations))},
                               {"first_action_latency_ms", distributionJson(describe(firstActions))},
                               {"first_selection_latency_ms", distributionJson(describe(firstSelections))},
                               {"transition_cost_ms", distributionJson(describe(navigationCosts(result, method)))},
                               {"cursor_recovery_distance_px", distributionJson(describe(recoveries))},
                               {"observations", std::move(observations)}};
}

std::string csvValue(const std::optional<double>& value) {
    if (!value)
        return {};
    std::ostringstream output;
    output << std::fixed << std::setprecision(3) << *value;
    return output.str();
}

std::string csvValue(double value) {
    return csvValue(std::optional<double>(value));
}

void writeMetricsCsv(const std::filesystem::path& path, const AnalysisResult& result, const std::string& sessionId) {
    const auto navMedian = [&](NavigationMethod method) { return describe(navigationCosts(result, method)).median; };
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("Unable to write metrics.csv");
    output << "session_id,active_duration_seconds,raw_apm,effective_apm,pac_rate,pac_median_first_action_ms,pac_p90_"
              "first_action_ms,"
              "inter_action_median_ms,inter_action_p90_ms,control_group_switch_median_ms,control_group_switch_p90_ms,"
              "location_hotkey_transition_cost_ms,control_group_jump_transition_cost_ms,minimap_transition_cost_ms,"
              "edge_scroll_transition_cost_ms,"
              "command_target_median_ms,command_target_p90_ms,box_duration_median_ms,box_command_median_ms,box_"
              "reselection_rate,"
              "worker_interval_median_ms,worker_interval_p90_ms,army_revisit_median_ms,army_revisit_p90_ms,macro_"
              "episode_duration_median_ms,"
              "macro_group_coverage,worker_high_load_change_pct,army_high_load_change_pct,micro_macro_return_median_ms,"
              "capacity_breakpoint_eapm,dropped_event_count\n";
    output << sessionId << ',' << csvValue(result.activeDurationSeconds) << ',' << csvValue(result.rawApm) << ','
           << csvValue(result.effectiveApm) << ',' << csvValue(result.pacRate) << ','
           << csvValue(result.pacFirstAction.median) << ',' << csvValue(result.pacFirstAction.p90) << ','
           << csvValue(result.interAction.median) << ',' << csvValue(result.interAction.p90) << ','
           << csvValue(result.controlGroupSwitch.median) << ',' << csvValue(result.controlGroupSwitch.p90) << ','
           << csvValue(navMedian(NavigationMethod::LocationHotkey)) << ','
           << csvValue(navMedian(NavigationMethod::ControlGroupJump)) << ','
           << csvValue(navMedian(NavigationMethod::MinimapJump)) << ','
           << csvValue(navMedian(NavigationMethod::EdgeScroll)) << ',' << csvValue(result.commandTarget.median) << ','
           << csvValue(result.commandTarget.p90) << ',' << csvValue(result.boxDuration.median) << ','
           << csvValue(result.boxCommand.median) << ',' << csvValue(result.boxReselectionRate) << ','
           << csvValue(result.workerInterval.median) << ',' << csvValue(result.workerInterval.p90) << ','
           << csvValue(result.armyRevisit.median) << ',' << csvValue(result.armyRevisit.p90) << ','
           << csvValue(result.macroEpisodeDuration.median) << ',' << csvValue(result.productionGroupCoverage) << ','
           << csvValue(result.workerHighLoadChangePct) << ',' << csvValue(result.armyHighLoadChangePct) << ','
           << csvValue(result.microMacroReturn.median) << ',' << csvValue(result.capacityBreakpointEapm) << ','
           << result.droppedEventCount << '\n';
}

} // namespace

SessionWriter::SessionWriter(const std::filesystem::path& sessionsRoot, std::uint64_t qpcFrequency,
                             bool writeLogicalEvents, int flushIntervalMs)
    : writeLogicalEvents_(writeLogicalEvents), flushIntervalMs_(flushIntervalMs) {
    std::filesystem::create_directories(sessionsRoot);
    sessionId_ = makeSessionId();
    directory_ = sessionsRoot / sessionId_;
    for (int suffix = 1; std::filesystem::exists(directory_); ++suffix) {
        directory_ = sessionsRoot / (sessionId_ + "_" + std::to_string(suffix));
    }
    sessionId_ = directory_.filename().string();
    std::filesystem::create_directories(directory_);
    rawFile_.open(directory_ / "events.bin", std::ios::binary | std::ios::trunc);
    if (!rawFile_)
        throw std::runtime_error("Unable to create events.bin");
    BinaryHeader rawHeader{};
    std::memcpy(rawHeader.magic, "SCMRAW1", 7);
    rawHeader.eventSize = sizeof(RawInputEvent);
    rawHeader.qpcFrequency = qpcFrequency;
    rawFile_.write(reinterpret_cast<const char*>(&rawHeader), sizeof(rawHeader));

    if (writeLogicalEvents_) {
        logicalFile_.open(directory_ / "logical_events.bin", std::ios::binary | std::ios::trunc);
        if (!logicalFile_)
            throw std::runtime_error("Unable to create logical_events.bin");
        BinaryHeader logicalHeader{};
        std::memcpy(logicalHeader.magic, "SCMLOG1", 7);
        logicalHeader.eventSize = sizeof(LogicalEvent);
        logicalHeader.qpcFrequency = qpcFrequency;
        logicalFile_.write(reinterpret_cast<const char*>(&logicalHeader), sizeof(logicalHeader));
    } else {
        std::ofstream placeholder(directory_ / "logical_events.bin", std::ios::binary | std::ios::trunc);
    }
    thread_ = std::thread(&SessionWriter::run, this);
}

SessionWriter::~SessionWriter() {
    stop();
}

bool SessionWriter::submitRaw(const RawInputEvent& event) noexcept {
    if (failed_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (rawQueue_.tryPush(event))
        return true;
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool SessionWriter::submitLogical(const LogicalEvent& event) noexcept {
    if (!writeLogicalEvents_)
        return true;
    if (failed_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (logicalQueue_.tryPush(event))
        return true;
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void SessionWriter::stop() {
    if (!thread_.joinable())
        return;
    stopping_.store(true, std::memory_order_release);
    thread_.join();
    rawFile_.flush();
    if (logicalFile_)
        logicalFile_.flush();
}

void SessionWriter::run() {
    auto lastFlush = std::chrono::steady_clock::now();
    while (!stopping_.load(std::memory_order_acquire) || !rawQueue_.empty() || !logicalQueue_.empty()) {
        bool wrote = false;
        RawInputEvent raw{};
        while (rawQueue_.tryPop(raw)) {
            rawFile_.write(reinterpret_cast<const char*>(&raw), sizeof(raw));
            wrote = true;
        }
        LogicalEvent logical{};
        while (logicalQueue_.tryPop(logical)) {
            if (logicalFile_)
                logicalFile_.write(reinterpret_cast<const char*>(&logical), sizeof(logical));
            wrote = true;
        }
        if (!rawFile_ || (logicalFile_.is_open() && !logicalFile_)) {
            failed_.store(true, std::memory_order_release);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastFlush >= std::chrono::milliseconds(flushIntervalMs_)) {
            rawFile_.flush();
            if (logicalFile_)
                logicalFile_.flush();
            lastFlush = now;
        }
        if (!wrote)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

json::Value analysisToJson(const AnalysisResult& result, const std::string& sessionId) {
    json::Value::Array pacObservations;
    for (const auto& pac : result.pacs) {
        pacObservations.emplace_back(
            json::Value::Object{{"start_active_ms", pac.startActiveMs},
                                {"end_active_ms", pac.endActiveMs},
                                {"transition", std::string(navigationMethodName(pac.transition))},
                                {"first_action_ms", optionalJson(pac.firstActionMs)},
                                {"first_completed_command_ms", optionalJson(pac.firstCompletedCommandMs)},
                                {"qualifying_action_count", static_cast<double>(pac.qualifyingActionCount)},
                                {"raw_action_count", static_cast<double>(pac.rawActionCount)},
                                {"actionless", pac.actionless}});
    }
    json::Value::Array switchObservations;
    for (const auto& item : result.controlGroupSwitchLatencies) {
        switchObservations.emplace_back(json::Value::Object{
            {"active_ms", item.activeTimeMs}, {"latency_ms", item.valueMs}, {"rolling_eapm", item.loadEapm}});
    }
    json::Value::Array commandObservations;
    for (const auto& item : result.commandTargets) {
        commandObservations.emplace_back(json::Value::Object{{"command", std::string(logicalEventName(item.command))},
                                                             {"active_ms", item.activeTimeMs},
                                                             {"latency_ms", item.latencyMs},
                                                             {"rolling_eapm", item.loadEapm}});
    }
    json::Value::Array boxObservations;
    for (const auto& box : result.boxes) {
        boxObservations.emplace_back(
            json::Value::Object{{"start_active_ms", box.startActiveMs},
                                {"end_active_ms", box.endActiveMs},
                                {"start_x", box.startX},
                                {"start_y", box.startY},
                                {"end_x", box.endX},
                                {"end_y", box.endY},
                                {"width", box.width},
                                {"height", box.height},
                                {"area", box.area},
                                {"diagonal", box.diagonal},
                                {"path_length", box.pathLength},
                                {"path_efficiency", box.pathEfficiency},
                                {"direction", box.direction},
                                {"box_to_command_ms", optionalJson(box.commandLatencyMs)},
                                {"context_to_box_start_ms", optionalJson(box.contextStartLatencyMs)},
                                {"context_to_box_complete_ms", optionalJson(box.contextCompleteLatencyMs)},
                                {"reselection_gap_ms", optionalJson(box.reselectionGapMs)},
                                {"reselection_iou", optionalJson(box.reselectionIou)},
                                {"probable_reselection", box.probableReselection}});
    }
    json::Value::Array rollingEapm;
    for (const auto value : result.rollingEapm)
        rollingEapm.emplace_back(value);
    json::Value::Array macroAttempts;
    for (const auto& attempt : result.macroAttempts) {
        macroAttempts.emplace_back(json::Value::Object{{"kind", attempt.worker ? "worker" : "army"},
                                                       {"group", attempt.group},
                                                       {"train_key", virtualKeyToName(attempt.trainKey)},
                                                       {"active_ms", attempt.activeTimeMs},
                                                       {"rolling_eapm", attempt.loadEapm}});
    }
    json::Value::Array macroEpisodes;
    for (const auto& episode : result.macroEpisodes) {
        json::Value::Array groups;
        for (const int group : episode.productionGroups)
            groups.emplace_back(group);
        macroEpisodes.emplace_back(json::Value::Object{{"start_active_ms", episode.startActiveMs},
                                                       {"end_active_ms", episode.endActiveMs},
                                                       {"worker_attempts", episode.workerAttempts},
                                                       {"army_attempts", episode.armyAttempts},
                                                       {"production_groups", std::move(groups)},
                                                       {"rolling_eapm", episode.loadEapm}});
    }
    json::Value::Array sequences;
    for (const auto& item : result.sequences) {
        json::Value::Array transitions;
        for (const auto value : item.meanTransitionMs)
            transitions.emplace_back(value);
        sequences.emplace_back(json::Value::Object{{"sequence", item.sequence},
                                                   {"length", item.length},
                                                   {"count", static_cast<double>(item.count)},
                                                   {"duration_ms", distributionJson(item.duration)},
                                                   {"mean_transition_ms", std::move(transitions)}});
    }

    json::Value::Array loadBins;
    for (const auto& bin : result.loadBins) {
        loadBins.emplace_back(
            json::Value::Object{{"label", bin.label},
                                {"lower_edge_eapm", bin.lowerEdge},
                                {"observations", static_cast<double>(bin.observations)},
                                {"pac_latency_ms", distributionJson(bin.pacLatency)},
                                {"switch_latency_ms", distributionJson(bin.switchLatency)},
                                {"command_target_latency_ms", distributionJson(bin.commandTargetLatency)},
                                {"worker_interval_ms", distributionJson(bin.workerInterval)},
                                {"army_revisit_interval_ms", distributionJson(bin.armyRevisitInterval)}});
    }

    json::Value::Object lapses;
    for (const auto& [name, value] : result.lapsesPerMinute)
        lapses[name] = value;
    json::Value::Object late;
    for (const auto& [name, value] : result.lateSessionChangePct)
        late[name] = optionalJson(value);

    json::Value root(json::Value::Object{});
    root["schema_version"] = 1;
    root["analysis_version"] = SCMECHANICS_VERSION;
    root["session"] = json::Value::Object{{"id", sessionId},
                                          {"active_duration_seconds", result.activeDurationSeconds},
                                          {"paused_duration_seconds", result.pausedDurationSeconds},
                                          {"dropped_event_count", static_cast<double>(result.droppedEventCount)}};
    root["pace"] = json::Value::Object{{"raw_input_count", static_cast<double>(result.rawInputCount)},
                                       {"effective_action_count", static_cast<double>(result.effectiveActionCount)},
                                       {"raw_apm", result.rawApm},
                                       {"input_derived_eapm", result.effectiveApm},
                                       {"effective_inter_action_ms", distributionJson(result.interAction)},
                                       {"rolling_eapm_10s", std::move(rollingEapm)}};
    root["pac"] = json::Value::Object{
        {"count", static_cast<double>(result.pacs.size())},
        {"rate_per_minute", result.pacRate},
        {"first_action_latency_ms", distributionJson(result.pacFirstAction)},
        {"completed_command_latency_ms", distributionJson(result.pacCompletedCommand)},
        {"duration_ms", distributionJson(result.pacDuration)},
        {"actions_per_pac", distributionJson(result.pacActions)},
        {"actionless_ratio",
         optionalJson(result.pacs.size() >= 5
                          ? ratio(static_cast<double>(std::count_if(result.pacs.begin(), result.pacs.end(),
                                                                    [](const auto& pac) { return pac.actionless; })),
                                  static_cast<double>(result.pacs.size()))
                          : std::nullopt)},
        {"observations", std::move(pacObservations)}};
    json::Value productiveThresholds(json::Value::Object{});
    constexpr std::array<const char*, 5> thresholdNames{"150_ms", "250_ms", "500_ms", "750_ms", "1000_ms"};
    for (std::size_t i = 0; i < thresholdNames.size(); ++i) {
        productiveThresholds[thresholdNames[i]] =
            optionalJson(result.totalSelections >= 5 ? ratio(static_cast<double>(result.productiveWithin[i]),
                                                             static_cast<double>(result.totalSelections))
                                                     : std::nullopt);
    }
    root["control_groups"] = json::Value::Object{
        {"switch_count", static_cast<double>(result.controlGroupSwitchCount)},
        {"switches_per_minute", result.switchesPerMinute},
        {"switch_to_action_ms", distributionJson(result.controlGroupSwitch)},
        {"productive_selection_ratio", optionalJson(result.productiveSelectionRatio)},
        {"return_latency_ms", distributionJson(describe(result.returnLatenciesMs))},
        {"return_to_action_ms", distributionJson(describe(result.returnToActionLatenciesMs))},
        {"productive_within", std::move(productiveThresholds)},
        {"switch_observations", std::move(switchObservations)},
        {"switch_to_completed_command_ms", distributionJson(describe(result.controlGroupCompletedCommandLatenciesMs))}};
    root["camera_navigation"] =
        json::Value::Object{{"control_group_jump", navigationJson(result, NavigationMethod::ControlGroupJump)},
                            {"location_hotkey", navigationJson(result, NavigationMethod::LocationHotkey)},
                            {"minimap_jump", navigationJson(result, NavigationMethod::MinimapJump)},
                            {"edge_scroll", navigationJson(result, NavigationMethod::EdgeScroll)}};
    root["commands"] = json::Value::Object{{"command_to_target_ms", distributionJson(result.commandTarget)},
                                           {"observations", std::move(commandObservations)}};
    std::vector<double> contextBoxStart, contextBoxComplete;
    for (const auto& box : result.boxes) {
        if (box.contextStartLatencyMs)
            contextBoxStart.push_back(*box.contextStartLatencyMs);
        if (box.contextCompleteLatencyMs)
            contextBoxComplete.push_back(*box.contextCompleteLatencyMs);
    }
    root["box_selection"] =
        json::Value::Object{{"count", static_cast<double>(result.boxes.size())},
                            {"duration_ms", distributionJson(result.boxDuration)},
                            {"box_to_command_ms", distributionJson(result.boxCommand)},
                            {"probable_reselection_rate", optionalJson(result.boxReselectionRate)},
                            {"mean_path_efficiency", optionalJson(result.meanBoxPathEfficiency)},
                            {"context_to_box_start_ms", distributionJson(describe(contextBoxStart))},
                            {"context_to_box_complete_ms", distributionJson(describe(contextBoxComplete))},
                            {"selection_command_cycle_ms", distributionJson(result.boxCycle)},
                            {"observations", std::move(boxObservations)}};
    root["macro"] = json::Value::Object{
        {"worker", json::Value::Object{{"attempt_interval_ms", distributionJson(result.workerInterval)}}},
        {"army", json::Value::Object{{"revisit_interval_ms", distributionJson(result.armyRevisit)},
                                     {"episode_duration_ms", distributionJson(result.armyEpisodeDuration)},
                                     {"production_group_coverage", optionalJson(result.armyProductionGroupCoverage)}}},
        {"combined",
         json::Value::Object{{"episode_duration_ms", distributionJson(result.macroEpisodeDuration)},
                             {"production_group_coverage", optionalJson(result.productionGroupCoverage)},
                             {"combined_burst_ratio", optionalJson(result.combinedMacroBurstRatio)},
                             {"worker_army_offset_median_ms", optionalJson(result.workerArmyOffsetMedianMs)}}},
        {"under_load",
         json::Value::Object{{"worker_interval_change_pct", optionalJson(result.workerHighLoadChangePct)},
                             {"army_revisit_change_pct", optionalJson(result.armyHighLoadChangePct)},
                             {"episode_duration_change_pct", optionalJson(result.macroDurationHighLoadChangePct)}}},
        {"micro_to_macro_return_ms", distributionJson(result.microMacroReturn)},
        {"attempts", std::move(macroAttempts)},
        {"episodes", std::move(macroEpisodes)}};
    root["sequences"] = std::move(sequences);
    root["capacity"] = json::Value::Object{{"estimated_breakpoint_eapm", optionalJson(result.capacityBreakpointEapm)},
                                           {"load_bins", std::move(loadBins)}};
    root["consistency"] = json::Value::Object{{"mechanical_lapses_per_minute", std::move(lapses)},
                                              {"late_session_change_pct", std::move(late)}};
    return root;
}

void writeSessionSummary(const std::filesystem::path& directory, const AnalysisResult& result,
                         const std::string& sessionId) {
    json::writeFile(directory / "summary.json", analysisToJson(result, sessionId));
    writeMetricsCsv(directory / "metrics.csv", result, sessionId);
}

std::vector<std::filesystem::path> listSessionSummaries(const std::filesystem::path& sessionsRoot) {
    std::vector<std::filesystem::path> result;
    if (!std::filesystem::exists(sessionsRoot))
        return result;
    for (const auto& entry : std::filesystem::directory_iterator(sessionsRoot)) {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "summary.json"))
            result.push_back(entry.path() / "summary.json");
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::filesystem::path resolveSessionSummary(const std::filesystem::path& sessionsRoot, const std::string& selector) {
    if (selector == "latest") {
        const auto summaries = listSessionSummaries(sessionsRoot);
        if (summaries.empty())
            throw std::runtime_error("No recorded sessions were found");
        return summaries.back();
    }
    const auto path = sessionsRoot / selector / "summary.json";
    if (!std::filesystem::exists(path))
        throw std::runtime_error("Session not found: " + selector);
    return path;
}

std::filesystem::path exportSessionCsv(const std::filesystem::path& sessionsRoot,
                                       const std::filesystem::path& exportRoot, const std::string& selector) {
    const auto summary = resolveSessionSummary(sessionsRoot, selector);
    const auto sessionDirectory = summary.parent_path();
    const auto source = sessionDirectory / "metrics.csv";
    if (!std::filesystem::exists(source))
        throw std::runtime_error("Session metrics.csv is missing");
    std::filesystem::create_directories(exportRoot);
    const auto destination = exportRoot / ("scmechanics_" + sessionDirectory.filename().string() + ".csv");
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
    return destination;
}

} // namespace scm
