#pragma once

#include "capture/raw_event.h"
#include "config/config.h"
#include "platform/screen_regions.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace smp {

enum class CameraNavigationType {
    ControlGroupJump,
    LocationHotkey,
    MinimapJump,
    EdgeScroll
};

enum class CameraContextType {
    Unknown,
    ControlGroup,
    LocationHotkey,
    Manual
};

struct CameraContext {
    CameraContextType type{CameraContextType::Unknown};
    int id{-1};
};

enum class CameraRecenterType {
    ControlGroup,
    LocationHotkey
};

enum class MechanicalInputType : std::uint8_t {
    KeyPress,
    ControlGroupSelect,
    ControlGroupAssign,
    LocationRecall,
    LocationAssign,
    MouseLeftDown,
    MouseLeftUp,
    MouseRightDown,
    MouseRightUp,
    MouseMiddleDown,
    MouseMiddleUp,
    MouseWheel,
    ControlGroupAdd
};

enum MechanicalModifier : std::uint16_t {
    ModifierNone = 0,
    ModifierCtrl = 1 << 0,
    ModifierShift = 1 << 1,
    ModifierAlt = 1 << 2
};

struct MechanicalInputEvent {
    std::uint64_t timestampTicks{};
    double activeMs{};
    MechanicalInputType type{MechanicalInputType::KeyPress};
    std::uint16_t virtualKey{};
    std::uint16_t scanCode{};
    std::uint16_t modifiers{};
    int value{-1};
    int cursorX{};
    int cursorY{};
};

struct CameraNavigationEvent {
    std::uint64_t timestampTicks{};
    double activeMs{};
    CameraNavigationType type{CameraNavigationType::ControlGroupJump};
    int id{-1};
    int cursorX{};
    int cursorY{};
    double durationMs{};
    EdgeDirection edgeDirection{EdgeDirection::None};
    int startCursorX{};
    int startCursorY{};
};

struct CameraRecenterEvent {
    std::uint64_t timestampTicks{};
    double activeMs{};
    CameraRecenterType type{CameraRecenterType::ControlGroup};
    int id{-1};
    int cursorX{};
    int cursorY{};
};

struct AnalysisResult {
    double activeDurationSeconds{};
    double pausedDurationSeconds{};
    std::uint64_t droppedEventCount{};
    std::uint64_t locationRecallCount{};
    std::vector<CameraNavigationEvent> navigationEvents;
    std::vector<CameraRecenterEvent> recenters;
    std::vector<MechanicalInputEvent> mechanicalEvents;
};

const char* cameraNavigationTypeName(CameraNavigationType type) noexcept;
const char* cameraRecenterTypeName(CameraRecenterType type) noexcept;

class Analyzer {
  public:
    Analyzer(Config config, std::uint64_t ticksPerSecond);

    void process(const RawInputEvent& event);
    void finalize(std::uint64_t endingTicks, std::uint64_t droppedEventCount);
    void setScreenRegions(const ScreenRegions& regions) noexcept;
    void setDroppedEventCount(std::uint64_t count) noexcept {
        result_.droppedEventCount = count;
    }

    [[nodiscard]] const AnalysisResult& result() const noexcept {
        return result_;
    }
    [[nodiscard]] CameraContext cameraContext() const noexcept {
        return cameraContext_;
    }
    [[nodiscard]] ScreenRegions screenRegions() const noexcept;
    std::vector<CameraNavigationEvent> takeEmittedNavigationEvents();
    std::vector<CameraRecenterEvent> takeEmittedRecenters();

  private:
    double ticksToMs(std::uint64_t ticks) const noexcept;
    double activeTimeAt(double absoluteMs) const noexcept;
    void handleKeyDown(const RawInputEvent& event, double activeMs);
    void handleControlGroupSelect(const RawInputEvent& event, int group, double activeMs);
    void handleLocationRecall(const RawInputEvent& event, int location, double activeMs);
    void handleMouseMove(const RawInputEvent& event, double activeMs);
    void completeEdgeEpisode(const RawInputEvent& event);
    void clearEdgeState() noexcept;
    void emitNavigation(const CameraNavigationEvent& event);
    void emitRecenter(const CameraRecenterEvent& event);
    void emitMechanical(const MechanicalInputEvent& event);
    [[nodiscard]] std::uint16_t mechanicalModifiers() const noexcept;

    Config config_;
    std::uint64_t frequency_{};
    AnalysisResult result_;
    std::vector<CameraNavigationEvent> emittedNavigation_;
    std::vector<CameraRecenterEvent> emittedRecenters_;

    std::array<bool, 256> keysDown_{};
    std::array<std::optional<std::uint64_t>, 10> lastControlGroupSelect_{};
    CameraContext cameraContext_{};

    bool active_{};
    bool seenSession_{};
    bool finalized_{};
    double activeSegmentStartAbsoluteMs_{};
    double pauseStartAbsoluteMs_{};
    double accumulatedActiveMs_{};
    double accumulatedPausedMs_{};

    EdgeDirection candidateEdge_{EdgeDirection::None};
    std::uint64_t candidateEdgeStartTicks_{};
    double candidateEdgeStartActiveMs_{};
    ScreenPoint candidateEdgeStartCursor_{};
    bool edgeActive_{};
    EdgeDirection activeEdgeDirection_{EdgeDirection::None};
    ScreenPoint lastCursor_{};
};

} // namespace smp
