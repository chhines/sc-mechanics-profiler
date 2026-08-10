#include "platform/foreground.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <filesystem>

namespace smp {

ForegroundMatcher::ForegroundMatcher(std::wstring expectedExecutable)
    : expectedExecutable_(std::filesystem::path(std::move(expectedExecutable)).filename().wstring()) {
    std::transform(expectedExecutable_.begin(), expectedExecutable_.end(), expectedExecutable_.begin(), std::towlower);
}

bool ForegroundMatcher::matchesForeground() {
    return matches(GetForegroundWindow());
}

bool ForegroundMatcher::matches(HWND window) {
    DWORD processId = 0;
    if (window)
        GetWindowThreadProcessId(window, &processId);
    if (window == cachedWindow_ && processId == cachedProcessId_)
        return cachedMatch_;
    cachedWindow_ = window;
    cachedProcessId_ = processId;
    cachedMatch_ = evaluate(window);
    return cachedMatch_;
}

bool ForegroundMatcher::evaluate(HWND window) const {
    if (!window)
        return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0)
        return false;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return false;
    std::array<wchar_t, 32768> path{};
    DWORD size = static_cast<DWORD>(path.size() - 1);
    const bool success = QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE;
    CloseHandle(process);
    if (!success)
        return false;
    path[size] = L'\0';
    const wchar_t* filename = std::wcsrchr(path.data(), L'\\');
    filename = filename ? filename + 1 : path.data();
    return _wcsicmp(filename, expectedExecutable_.c_str()) == 0;
}

} // namespace smp
