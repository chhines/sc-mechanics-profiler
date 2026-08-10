#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace smp {

int runCommand(const std::vector<std::string>& arguments, const std::filesystem::path& workingDirectory);
void printUsage();

} // namespace smp
