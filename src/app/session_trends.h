#pragma once

#include "app/gui_preferences.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <utility>

namespace smp {

void drawSessionTrends(const std::filesystem::path& sessionsRoot,
                       const ReportGroupVisibility& visibility);

} // namespace smp
