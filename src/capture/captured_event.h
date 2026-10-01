#pragma once

#include "capture/raw_event.h"
#include "platform/screen_regions.h"

#include <optional>
#include <type_traits>

namespace smp {

// Runtime context only: raw storage continues to persist event verbatim.
// A missing snapshot is evidence of unavailable geometry, never a request to
// look up geometry when this event is eventually consumed.
struct CapturedInputEvent {
    RawInputEvent event;
    std::optional<ScreenRegions> screenRegions;
};

static_assert(std::is_trivially_copyable_v<CapturedInputEvent>);

} // namespace smp
