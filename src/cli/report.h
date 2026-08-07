#pragma once

#include "util/json.h"

#include <filesystem>
#include <string>
#include <vector>

namespace scm {

void printSummary(const json::Value& summary, const std::filesystem::path& sessionDirectory = {});
void printComparison(const json::Value& latest, const std::vector<json::Value>& baselines);

} // namespace scm
