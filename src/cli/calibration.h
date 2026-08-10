#pragma once

#include "config/config.h"

#include <filesystem>

namespace smp {

int runCalibration(Config& config, const std::filesystem::path& configPath);

} // namespace smp
