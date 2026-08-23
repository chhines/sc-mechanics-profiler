#pragma once

#include "app/gui_preferences.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <utility>

namespace smp {

enum class SessionTrendsPresentation {
    Interactive,
    Capture,
};

void drawSessionTrends(const std::filesystem::path& sessionsRoot,
                       const ReportGroupVisibility& visibility,
                       SessionTrendsPresentation presentation =
                           SessionTrendsPresentation::Interactive);

} // namespace smp
