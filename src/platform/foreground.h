#pragma once

#include <string>
#include <windows.h>

namespace smp {

class ForegroundMatcher {
  public:
    explicit ForegroundMatcher(std::wstring expectedExecutable);
    bool matchesForeground();
    bool matches(HWND window);

  private:
    bool evaluate(HWND window) const;

    std::wstring expectedExecutable_;
    HWND cachedWindow_{};
    DWORD cachedProcessId_{};
    bool cachedMatch_{};
};

} // namespace smp
