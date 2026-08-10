#pragma once

#include "config/config.h"
#include "platform/minimap_viewport_detector.h"

#include <memory>

namespace smp {

class ScreenRegionCapture {
  public:
    ScreenRegionCapture();
    ~ScreenRegionCapture();
    ScreenRegionCapture(const ScreenRegionCapture&) = delete;
    ScreenRegionCapture& operator=(const ScreenRegionCapture&) = delete;

    BgraImageView capture(const ScreenRect& rectangle);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace smp
