#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace scm {

int runCommand(const std::vector<std::string>& arguments, const std::filesystem::path& workingDirectory);
void printUsage();

} // namespace scm
