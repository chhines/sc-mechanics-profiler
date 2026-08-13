#include "app/windows_application.h"
#include "app/application_paths.h"
#include "cli/commands.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include <shellapi.h>

namespace {

std::string utf8(const std::wstring& value) {
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

void attachParentConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && GetLastError() != ERROR_ACCESS_DENIED)
        return;
    FILE* stream = nullptr;
    (void)freopen_s(&stream, "CONOUT$", "w", stdout);
    (void)freopen_s(&stream, "CONOUT$", "w", stderr);
    (void)freopen_s(&stream, "CONIN$", "r", stdin);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Starcraft Mechanics Profiler");
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argumentCount = 0;
    LPWSTR* wideArguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    std::vector<std::string> arguments;
    for (int index = 1; index < argumentCount; ++index)
        arguments.push_back(utf8(wideArguments[index]));
    if (wideArguments)
        LocalFree(wideArguments);

    try {
        if (!arguments.empty()) {
            attachParentConsole();
            return smp::runCommand(arguments, std::filesystem::current_path());
        }
        return smp::runWindowsApplication(instance, smp::currentGuiApplicationPaths(),
                                          showCommand);
    } catch (const std::exception& error) {
        const auto message = std::string("Starcraft Mechanics Profiler: ") + error.what();
        if (!arguments.empty())
            std::cerr << message << '\n';
        MessageBoxA(nullptr, message.c_str(), "Starcraft Mechanics Profiler",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}
