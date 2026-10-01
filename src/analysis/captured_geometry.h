#pragma once

#include "analysis/analyzer.h"
#include "capture/captured_event.h"

namespace smp {

// Consumer-owned geometry shared by analysis and event-region diagnostics.
class CapturedGeometry {
  public:
    bool apply(const CapturedInputEvent& captured, const Config& config,
               Analyzer& analyzer) noexcept {
        ScreenRegions selected = captured.screenRegions.value_or(ScreenRegions{});
        ResolvedMinimapRegion resolved{};
        if (captured.screenRegions) {
            resolved = resolveMinimapRegion(
                selected, config.originalAspectMinimapMode,
                config.widescreenMinimapMode, config.calibratedMinimap,
                config.widescreenCalibratedMinimap);
            selected.minimap = resolved.rect;
        }
        const bool changed = !initialized_ ||
            captured.event.type == RawEventType::ForegroundGained ||
            regions_.clientArea != selected.clientArea ||
            regions_.gameArea != selected.gameArea ||
            regions_.viewport != selected.viewport ||
            regions_.minimap != selected.minimap ||
            regions_.commandCard != selected.commandCard ||
            regions_.displayMode != selected.displayMode ||
            minimap_.source != resolved.source;
        if (changed)
            analyzer.setScreenRegions(selected); // Also breaks incompatible edge state.
        regions_ = selected;
        minimap_ = resolved;
        initialized_ = true;
        return changed;
    }

    [[nodiscard]] const ScreenRegions& regions() const noexcept { return regions_; }
    [[nodiscard]] const ResolvedMinimapRegion& minimap() const noexcept { return minimap_; }

  private:
    ScreenRegions regions_{};
    ResolvedMinimapRegion minimap_{};
    bool initialized_{};
};

} // namespace smp
