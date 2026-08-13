#pragma once

#include <filesystem>

namespace smp {

struct GuiApplicationPaths {
    std::filesystem::path dataRoot;
    std::filesystem::path config;
    std::filesystem::path preferences;
    std::filesystem::path sessions;
    std::filesystem::path exports;

    bool operator==(const GuiApplicationPaths&) const noexcept = default;
};

[[nodiscard]] std::filesystem::path executableDirectory(
    const std::filesystem::path& executablePath);
[[nodiscard]] GuiApplicationPaths guiApplicationPaths(
    const std::filesystem::path& executablePath);
[[nodiscard]] GuiApplicationPaths currentGuiApplicationPaths();

} // namespace smp
