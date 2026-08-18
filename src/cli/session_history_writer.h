#pragma once

#include "cli/automatic_session_stats.h"

#include <filesystem>
#include <string_view>
#include <utility>

namespace smp {

// Persist the machine-readable automatic-session history directly to the
// supplied *_session.json path. This path never creates a readable .txt file.
void writeAutomaticSessionHistoryJson(
    const std::filesystem::path& dataPath,
    const AutomaticSessionState& session);

} // namespace smp
