#pragma once

#include <filesystem>
#include <optional>

namespace smp {

enum class MainWindowAction {
    HideToTray,
    MinimizeNormally,
    Exit,
};

[[nodiscard]] MainWindowAction minimizeAction(bool minimizeToTray) noexcept;
[[nodiscard]] MainWindowAction closeAction(bool minimizeToTray) noexcept;

struct ReportGroupVisibility {
    bool cameraNavigation{true};
    bool workerMacroCycles{true};
    bool armyMacroCycles{true};
    bool macroAccessStyles{true};
    bool armyControlGroupManagement{true};
    bool scoutingUnitActivity{true};

    void selectAll() noexcept;
    bool operator==(const ReportGroupVisibility&) const noexcept = default;
};

struct GuiWindowPlacement {
    int x{};
    int y{};
    int width{760};
    int height{640};

    [[nodiscard]] bool valid() const noexcept;
    bool operator==(const GuiWindowPlacement&) const noexcept = default;
};

struct GuiPreferences {
    ReportGroupVisibility reports;
    bool minimizeToTray{true};
    std::optional<GuiWindowPlacement> window;

    [[nodiscard]] static GuiPreferences defaults() noexcept;
    [[nodiscard]] static GuiPreferences load(const std::filesystem::path& path) noexcept;
    void save(const std::filesystem::path& path) const;
    bool operator==(const GuiPreferences&) const noexcept = default;
};

} // namespace smp
