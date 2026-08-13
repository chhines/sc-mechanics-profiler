#pragma once

#include "app/application_paths.h"

#include <windows.h>

namespace smp {

int runWindowsApplication(HINSTANCE instance,
                          const GuiApplicationPaths& paths,
                          int showCommand);

} // namespace smp
