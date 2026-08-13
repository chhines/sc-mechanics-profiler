#pragma once

#include "config/config.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace smp {

int runCalibration(Config& config, const std::filesystem::path& configPath,
                   const std::function<void(std::string)>& progress = {},
                   const std::atomic<bool>* requested = nullptr);

} // namespace smp
