#pragma once

#include "app/gui_preferences.h"
#include "cli/automatic_session_stats.h"
#include "util/json.h"

#include <filesystem>
#include <string>
#include <vector>

namespace smp {

void printSummary(const json::Value& summary, const std::filesystem::path& sessionPath = {});
[[nodiscard]] std::string formatAutomaticSessionReport(const AutomaticSessionState& session);
[[nodiscard]] std::string formatAutomaticSessionReport(
    const AutomaticSessionState& session,
    const ReportGroupVisibility& visibility);
void printAutomaticSessionReport(const AutomaticSessionState& session);
void printComparison(const json::Value& latest, const std::vector<json::Value>& baselines);

} // namespace smp
