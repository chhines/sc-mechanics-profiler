#include "cli/commands.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>

int main(int argc, char** argv) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        std::vector<std::string> arguments;
        for (int i = 1; i < argc; ++i)
            arguments.emplace_back(argv[i]);
        return scm::runCommand(arguments, std::filesystem::current_path());
    } catch (const std::exception& error) {
        std::cerr << "scmechanics: " << error.what() << '\n';
        return 1;
    }
}
