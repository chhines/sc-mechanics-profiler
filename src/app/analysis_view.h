#pragma once

#include "app/game_analysis_visualization_model.h"

namespace smp {

struct AnalysisViewState {
    bool fitTimeline{true};
    bool resetTimeline{};
    bool showNavigation{true};
    bool showWorker{true};
    bool showArmy{true};
    bool showProductionVisits{true};
    bool showControlGroupEdits{true};
    bool showScouting{true};
};

void drawAnalysisView(const GameAnalysisVisualizationModel& model,
                      AnalysisViewState& state);

} // namespace smp
