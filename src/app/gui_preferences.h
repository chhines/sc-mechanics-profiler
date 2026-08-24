#pragma once

#include "app/session_kpi.h"

#include <filesystem>
#include <functional>
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
    bool gameTimeline{true};
    bool cameraNavigation{true};
    bool workerMacroCycles{true};
    bool armyMacroCycles{true};
    bool macroGaps{true};
    bool macroDurationDistribution{true};
    bool macroAccessStyles{true};
    bool armyControlGroupManagement{true};
    bool armyCommandActivity{true};
    bool abilityActivity{true};
    bool navigationTransitionRate{true};
    bool multitaskingDensity{true};
    bool scoutingUnitActivity{true};

    void selectAll() noexcept;
    void clearAll() noexcept;

    [[nodiscard]] bool hasMacroAnalysisSections() const noexcept;
    [[nodiscard]] bool hasArmyManagementAnalysisSections() const noexcept;
    [[nodiscard]] bool hasMultitaskingAnalysisSections() const noexcept;
    bool operator==(const ReportGroupVisibility&) const noexcept = default;
};

using ReportVisibilityProvider = std::function<ReportGroupVisibility()>;

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
    SessionReportVisibility sessionReports;
    bool minimizeToTray{true};
    std::optional<GuiWindowPlacement> window;

    [[nodiscard]] static GuiPreferences defaults() noexcept;
    [[nodiscard]] static GuiPreferences load(const std::filesystem::path& path) noexcept;
    void save(const std::filesystem::path& path) const;
    bool operator==(const GuiPreferences&) const noexcept = default;
};

} // namespace smp
