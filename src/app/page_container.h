#pragma once

#include <windows.h>

namespace smp {

inline constexpr wchar_t pageContainerClassName[] =
    L"StarcraftMechanicsProfilerPageContainer";

[[nodiscard]] bool registerPageContainerClass(HINSTANCE instance) noexcept;
[[nodiscard]] HWND createPageContainer(HWND parent, HINSTANCE instance) noexcept;

} // namespace smp
