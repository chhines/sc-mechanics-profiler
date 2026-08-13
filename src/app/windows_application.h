#pragma once

#include <filesystem>
#include <windows.h>

namespace smp {

int runWindowsApplication(HINSTANCE instance,
                          const std::filesystem::path& workingDirectory,
                          int showCommand);

} // namespace smp
