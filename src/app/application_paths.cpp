#include "app/application_paths.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace smp {

std::filesystem::path executableDirectory(
    const std::filesystem::path& executablePath) {
    if (executablePath.empty() || !executablePath.has_filename())
        throw std::invalid_argument("The executable path is invalid");
    return executablePath.parent_path().lexically_normal();
}

GuiApplicationPaths guiApplicationPaths(
    const std::filesystem::path& executablePath) {
    const auto root = executableDirectory(executablePath);
    return {root, root / "config.json", root / "gui-config.json",
            root / "sessions", root / "exports"};
}

GuiApplicationPaths currentGuiApplicationPaths() {
    std::wstring buffer(260, L'\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            throw std::runtime_error("Unable to determine the application executable path");
        if (length < buffer.size()) {
            buffer.resize(length);
            return guiApplicationPaths(std::filesystem::path(buffer));
        }
        if (buffer.size() >= 32768)
            throw std::runtime_error("The application executable path is too long");
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
}

} // namespace smp
