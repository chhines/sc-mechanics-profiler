#pragma once

#include "cli/automatic_session_stats.h"
#include "util/json.h"

#include <filesystem>
#include <string>
#include <vector>

namespace smp {

void printSummary(const json::Value& summary, const std::filesystem::path& sessionPath = {});
void printAutomaticSessionReport(const AutomaticSessionState& session);
void printComparison(const json::Value& latest, const std::vector<json::Value>& baselines);

} // namespace smp
