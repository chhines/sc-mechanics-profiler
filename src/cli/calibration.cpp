#include "cli/calibration.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace scm {
namespace {

POINT capturePoint(const char* prompt) {
    std::cout << prompt << " and press Enter." << std::flush;
    std::string line;
    std::getline(std::cin, line);
    POINT point{};
    if (!GetCursorPos(&point))
        throw std::runtime_error("Unable to read the cursor position");
    std::cout << "  captured (" << point.x << ", " << point.y << ")\n";
    return point;
}

Rect captureRect(const char* name) {
    const auto first = capturePoint((std::string("Move the cursor to the top-left of ") + name).c_str());
    const auto second = capturePoint((std::string("Move the cursor to the bottom-right of ") + name).c_str());
    return {std::min(first.x, second.x), std::min(first.y, second.y), std::max(first.x, second.x),
            std::max(first.y, second.y)};
}

} // namespace

int runCalibration(Config& config, const std::filesystem::path& configPath) {
    std::cout << "scmechanics calibration\n\n"
              << "This records cursor coordinates only; it does not capture the screen.\n\n";
    config.minimap = captureRect("the minimap");
    config.viewport = captureRect("the gameplay viewport");
    config.commandCard = captureRect("the command card");
    config.save(configPath);
    std::cout << "\nCalibration saved to " << configPath.string() << "\n";
    return 0;
}

} // namespace scm
