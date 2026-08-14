#pragma once

#include <array>
#include <cstddef>
#include <windows.h>

namespace smp {

inline constexpr wchar_t pageContainerClassName[] =
    L"StarcraftMechanicsProfilerPageContainer";

[[nodiscard]] bool registerPageContainerClass(HINSTANCE instance) noexcept;
[[nodiscard]] HWND createPageContainer(HWND parent, HINSTANCE instance) noexcept;

template <std::size_t PageCount, typename CreatePage>
[[nodiscard]] bool createPageContainers(std::array<HWND, PageCount>& pages,
                                        CreatePage createPage) {
    for (auto& page : pages) {
        page = createPage();
        if (!page)
            return false;
    }
    return true;
}

} // namespace smp
