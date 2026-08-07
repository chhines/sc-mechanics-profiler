#pragma once

#include "config/config.h"

#include <filesystem>

namespace scm {

int runCalibration(Config& config, const std::filesystem::path& configPath);

} // namespace scm
