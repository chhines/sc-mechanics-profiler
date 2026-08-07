#pragma once

#include "analysis/logical_event.h"
#include "analysis/statistics.h"
#include "capture/raw_event.h"
#include "config/config.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace scm {

struct TimedMeasurement {
    double activeTimeMs{};
    double valueMs{};
    double loadEapm{};
};

struct PacRecord {
    double startActiveMs{};
    double endActiveMs{};
    NavigationMethod transition{NavigationMethod::None};
    std::optional<double> firstActionMs;
    std::optional<double> firstCompletedCommandMs;
    double lastActionActiveMs{};
    std::uint32_t qualifyingActionCount{};
    std::uint32_t rawActionCount{};
    bool actionless{true};
};

struct NavigationRecord {
    NavigationMethod method{NavigationMethod::None};
    double completionActiveMs{};
    double durationMs{};
    std::optional<double> firstActionLatencyMs;
    std::optional<double> firstSelectionLatencyMs;
    std::optional<double> cursorRecoveryDistance;
    int cursorX{};
    int cursorY{};
};

struct CommandTargetRecord {
    LogicalEventType command{LogicalEventType::KeyAction};
    double activeTimeMs{};
    double latencyMs{};
    double loadEapm{};
};

struct BoxRecord {
    double startActiveMs{};
    double endActiveMs{};
    int startX{};
    int startY{};
    int endX{};
    int endY{};
    int width{};
    int height{};
    double area{};
    double diagonal{};
    double pathLength{};
    double pathEfficiency{};
    std::string direction;
    std::optional<double> commandLatencyMs;
    std::optional<double> contextStartLatencyMs;
    std::optional<double> contextCompleteLatencyMs;
    std::optional<double> reselectionGapMs;
    std::optional<double> reselectionIou;
    bool probableReselection{};
};

struct MacroAttempt {
    bool worker{};
    int group{};
    std::uint16_t trainKey{};
    double activeTimeMs{};
    double loadEapm{};
};

struct MacroEpisode {
    double startActiveMs{};
    double endActiveMs{};
    int workerAttempts{};
    int armyAttempts{};
    std::set<int> productionGroups;
    std::optional<double> firstWorkerActiveMs;
    std::optional<double> firstArmyActiveMs;
    double loadEapm{};
};

struct SequenceSummary {
    std::string sequence;
    int length{};
    std::size_t count{};
    Distribution duration;
    std::vector<double> meanTransitionMs;
    std::vector<TimedMeasurement> observations;
};

struct LoadBinSummary {
    std::string label;
    double lowerEdge{};
    std::size_t observations{};
    Distribution pacLatency;
    Distribution switchLatency;
    Distribution commandTargetLatency;
    Distribution workerInterval;
    Distribution armyRevisitInterval;
};

struct AnalysisResult {
    double activeDurationSeconds{};
    double pausedDurationSeconds{};
    std::uint64_t droppedEventCount{};
    std::uint64_t rawInputCount{};
    std::uint64_t effectiveActionCount{};
    double rawApm{};
    double effectiveApm{};

    std::vector<double> effectiveActionTimesMs;
    std::vector<double> interActionLatenciesMs;
    std::vector<PacRecord> pacs;
    std::vector<TimedMeasurement> pacFirstActionLatencies;
    std::vector<TimedMeasurement> controlGroupSwitchLatencies;
    std::vector<double> controlGroupCompletedCommandLatenciesMs;
    std::vector<double> returnLatenciesMs;
    std::vector<double> returnToActionLatenciesMs;
    std::size_t controlGroupSwitchCount{};
    std::size_t productiveSelections{};
    std::size_t totalSelections{};
    std::array<std::size_t, 5> productiveWithin{};

    std::vector<NavigationRecord> navigation;
    std::vector<CommandTargetRecord> commandTargets;
    std::vector<BoxRecord> boxes;
    std::vector<double> boxCycleDurationsMs;
    std::vector<MacroAttempt> macroAttempts;
    std::vector<MacroEpisode> macroEpisodes;
    std::vector<double> microMacroReturnMs;
    std::vector<SequenceSummary> sequences;
    std::vector<double> rollingEapm;
    std::vector<LoadBinSummary> loadBins;
    std::optional<double> capacityBreakpointEapm;

    Distribution interAction;
    Distribution pacFirstAction;
    Distribution pacCompletedCommand;
    Distribution pacDuration;
    Distribution pacActions;
    Distribution controlGroupSwitch;
    Distribution commandTarget;
    Distribution boxDuration;
    Distribution boxCommand;
    Distribution workerInterval;
    Distribution armyRevisit;
    Distribution armyEpisodeDuration;
    Distribution macroEpisodeDuration;
    Distribution microMacroReturn;
    Distribution boxCycle;

    double pacRate{};
    double switchesPerMinute{};
    std::optional<double> productiveSelectionRatio;
    std::optional<double> boxReselectionRate;
    std::optional<double> meanBoxPathEfficiency;
    std::optional<double> productionGroupCoverage;
    std::optional<double> armyProductionGroupCoverage;
    std::optional<double> combinedMacroBurstRatio;
    std::optional<double> workerArmyOffsetMedianMs;
    std::optional<double> workerHighLoadChangePct;
    std::optional<double> armyHighLoadChangePct;
    std::optional<double> macroDurationHighLoadChangePct;
    std::map<std::string, double> lapsesPerMinute;
    std::map<std::string, std::optional<double>> lateSessionChangePct;
};

class Analyzer {
  public:
    Analyzer(Config config, std::uint64_t ticksPerSecond);

    void process(const RawInputEvent& event);
    void finalize(std::uint64_t endingTicks, std::uint64_t droppedEventCount);
    void setDroppedEventCount(std::uint64_t count) noexcept {
        result_.droppedEventCount = count;
    }
    [[nodiscard]] const AnalysisResult& result() const noexcept {
        return result_;
    }
    [[nodiscard]] const std::vector<LogicalEvent>& logicalEvents() const noexcept {
        return logicalEvents_;
    }
    std::vector<LogicalEvent> takeEmittedEvents();

  private:
    struct PendingCommand {
        LogicalEventType type{LogicalEventType::KeyAction};
        double startActiveMs{};
    };
    struct PendingSelection {
        double startActiveMs{};
        bool productive{};
        std::optional<double> firstActionLatencyMs;
    };
    struct PendingSwitch {
        double startActiveMs{};
    };
    struct DragState {
        double downAbsoluteMs{};
        double downActiveMs{};
        std::uint64_t sourceSequence{};
        int startX{};
        int startY{};
        int lastX{};
        int lastY{};
        double pathLength{};
        bool dragging{};
        std::optional<double> contextStartLatencyMs;
    };
    struct WorkingEpisode {
        MacroEpisode episode;
        double lastAttemptActiveMs{};
    };
    struct SequenceToken {
        std::string name;
        double absoluteMs{};
        double activeMs{};
    };
    struct SequenceAggregate {
        int length{};
        std::size_t count{};
        std::vector<double> durationSamples;
        std::vector<double> activeTimeSamples;
        std::vector<double> transitionSums;
    };

    double ticksToMs(std::uint64_t ticks) const;
    double activeTimeAt(double absoluteMs) const;
    double currentLoadEapm(double activeMs) const;
    void emit(LogicalEventType type, const RawInputEvent& source, Confidence confidence = Confidence::Observed,
              int data1 = 0, int data2 = 0, double value1 = 0.0, double value2 = 0.0);
    void emitAt(LogicalEventType type, double absoluteMs, std::uint64_t sourceSequence, Confidence confidence,
                int data1 = 0, int data2 = 0, double value1 = 0.0, double value2 = 0.0);
    void handleKeyDown(const RawInputEvent& event, double absoluteMs, double activeMs);
    void handleMouseMove(const RawInputEvent& event, double absoluteMs, double activeMs);
    void handleLeftDown(const RawInputEvent& event, double absoluteMs, double activeMs);
    void handleLeftUp(const RawInputEvent& event, double absoluteMs, double activeMs);
    void handleRightDown(const RawInputEvent& event, double absoluteMs, double activeMs);
    void startNavigation(NavigationMethod method, const RawInputEvent& event, double absoluteMs, double activeMs,
                         double durationMs, int cursorX, int cursorY, bool closeSelection = true);
    void closePac(double activeMs, double absoluteMs, std::uint64_t sequence);
    void qualifyingAction(double activeMs, double absoluteMs, std::uint64_t sequence, bool selection,
                          bool completedCommand, bool substantiveCommand);
    void effectiveAction(double activeMs);
    void viewportInteraction(int x, int y, double absoluteMs);
    bool recognizeMacro(std::uint16_t key, double activeMs, double absoluteMs, const RawInputEvent& event);
    void addMacroAttempt(bool worker, int group, std::uint16_t key, double activeMs, double absoluteMs,
                         const RawInputEvent& event);
    void finishMacroEpisode(double absoluteMs, std::uint64_t sequence);
    void microActivity(double activeMs, double absoluteMs, std::uint64_t sequence);
    void advanceMicro(double activeMs, double absoluteMs, std::uint64_t sequence);
    void finalizeSelection(double nextTransitionActiveMs);
    void computeDerivedMetrics();
    void observeSequence(const LogicalEvent& event, double activeMs, double absoluteMs);
    void computeSequences();
    void computeLoadMetrics();
    void computeConsistency();

    Config config_;
    std::uint64_t frequency_{};
    AnalysisResult result_;
    std::vector<LogicalEvent> logicalEvents_;
    std::size_t emittedCursor_{};
    std::deque<SequenceToken> sequenceWindow_;
    std::map<std::string, SequenceAggregate> sequenceAggregates_;

    std::array<bool, 256> keysDown_{};
    bool active_{};
    bool finalized_{};
    bool seenSession_{};
    double sessionStartAbsoluteMs{};
    double activeSegmentStartAbsoluteMs{};
    double pauseStartAbsoluteMs{};
    double accumulatedActiveMs{};
    double accumulatedPausedMs{};

    int currentGroup_{-1};
    double currentGroupSelectedActiveMs_{-1.0};
    std::array<double, 10> lastGroupTapAbsoluteMs_{};
    std::deque<std::pair<int, double>> groupHistory_;
    std::optional<PendingSelection> pendingSelection_;
    std::optional<PendingSwitch> pendingSwitch_;
    std::optional<double> pendingSwitchCompletedStartActiveMs_;
    std::optional<double> pendingReturnActiveMs_;
    std::optional<PendingCommand> pendingCommand_;
    std::optional<std::size_t> pendingNavigationIndex_;
    std::optional<PacRecord> currentPac_;

    std::optional<DragState> drag_;
    std::optional<std::size_t> pendingBoxIndex_;
    std::optional<WorkingEpisode> workingEpisode_;
    std::deque<double> microEventsActiveMs_;
    bool microActive_{};
    double microLastActivityActiveMs_{};
    std::optional<double> pendingMicroEndActiveMs_;

    int candidateEdge_{};
    int activeEdge_{};
    double candidateEdgeStartAbsoluteMs_{};
    double activeEdgeStartAbsoluteMs_{};
    double lastViewportInteractionAbsoluteMs_{};
};

} // namespace scm
