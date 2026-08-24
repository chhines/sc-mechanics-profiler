#include "app/windows_application.h"

#include "app/analysis_view.h"
#include "app/application_controller.h"
#include "app/full_content_capture.h"
#include "app/game_analysis_visualization_model.h"
#include "app/gui_preferences.h"
#include "app/gui_single_instance.h"
#include "app/results_view_model.h"
#include "cli/automatic_session_files.h"
#include "config/config.h"
#include "platform/foreground.h"
#include "platform/resource_ids.h"
#include "platform/starcraft_display_mode.h"
#include "storage/nav_retention.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "implot.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <exception>
#include <filesystem>
#include <iterator>
#include <optional>
#include <shellapi.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace smp {
namespace {

constexpr UINT trayMessage = WM_APP + 1;
constexpr UINT controllerChangedMessage = WM_APP + 2;
constexpr UINT uiTimer = 1;
constexpr UINT trayIconId = 1;
using UiClock = std::chrono::steady_clock;
constexpr std::chrono::nanoseconds uiFrameInterval{16'666'667};
constexpr std::chrono::milliseconds occlusionRetryInterval{100};

constexpr COLORREF titleBarBackground = RGB(19, 22, 27);
constexpr COLORREF titleBarForeground = RGB(237, 242, 250);
constexpr COLORREF titleBarBorder = RGB(56, 64, 77);
constexpr float uiFontSize = 18.0f;

enum class ApplicationPage {
    Main,
    Results,
    Analysis,
    Settings,
    About,
};

enum class ResultSource {
    LatestGame,
    CurrentSession,
};

enum TrayCommand : UINT {
    TrayOpen = 1000,
    TrayStatus,
    TrayToggleAutomatic,
    TrayLatest,
    TrayExit,
};

std::wstring wide(std::string_view value) {
    if (value.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

std::string utf8(std::wstring_view value) {
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

std::string pathText(const std::filesystem::path& path) {
    return utf8(path.wstring());
}

std::optional<std::filesystem::path> windowsFontsDirectory() {
    std::wstring windowsDirectory(MAX_PATH, L'\0');
    UINT length = GetWindowsDirectoryW(
        windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
    if (length == 0)
        return std::nullopt;
    if (length >= windowsDirectory.size()) {
        windowsDirectory.resize(static_cast<std::size_t>(length));
        length = GetWindowsDirectoryW(
            windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
        if (length == 0 || length >= windowsDirectory.size())
            return std::nullopt;
    }
    windowsDirectory.resize(length);
    return std::filesystem::path(windowsDirectory) / L"Fonts";
}

void configureFonts(ImGuiIO& io) noexcept {
    try {
        if (const auto fontDirectory = windowsFontsDirectory()) {
            constexpr std::array<std::wstring_view, 2> fontNames{
                L"SegUIVar.ttf", L"segoeui.ttf"};
            ImFontConfig fontConfig{};
            fontConfig.Flags |= ImFontFlags_NoLoadError;
            for (const auto fontName : fontNames) {
                const auto fontPath = *fontDirectory / fontName;
                const DWORD attributes = GetFileAttributesW(fontPath.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    continue;
                const auto encodedPath = pathText(fontPath);
                if (ImFont* font = io.Fonts->AddFontFromFileTTF(
                        encodedPath.c_str(), uiFontSize, &fontConfig)) {
                    io.FontDefault = font;
                    return;
                }
            }
        }
    } catch (...) {
    }
    io.FontDefault = io.Fonts->AddFontDefault();
}

struct D3dResources {
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    IDXGISwapChain* swapChain{};
    ID3D11RenderTargetView* renderTarget{};

    void destroyRenderTarget() noexcept {
        if (renderTarget) {
            renderTarget->Release();
            renderTarget = nullptr;
        }
    }

    void cleanup() noexcept {
        destroyRenderTarget();
        if (swapChain) {
            swapChain->Release();
            swapChain = nullptr;
        }
        if (context) {
            context->Release();
            context = nullptr;
        }
        if (device) {
            device->Release();
            device = nullptr;
        }
    }

    bool createRenderTarget() noexcept {
        ID3D11Texture2D* buffer = nullptr;
        if (!swapChain || !device ||
            FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
            return false;
        const HRESULT result =
            device->CreateRenderTargetView(buffer, nullptr, &renderTarget);
        buffer->Release();
        return SUCCEEDED(result);
    }
};

bool createD3d(HWND window, D3dResources& d3d) noexcept {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr std::array<D3D_FEATURE_LEVEL, 2> featureLevels{
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevels.data(),
        static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION, &description,
        &d3d.swapChain, &d3d.device, &selected, &d3d.context);
    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, featureLevels.data(),
            static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
            &description, &d3d.swapChain, &d3d.device, &selected, &d3d.context);
    }
    if (FAILED(result)) {
        d3d.cleanup();
        return false;
    }
    return d3d.createRenderTarget();
}

bool actionButton(const char* label, bool enabled,
                  ImVec2 size = ImVec2(0.0f, 0.0f)) {
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label, size);
    ImGui::EndDisabled();
    return pressed && enabled;
}

void pageHeading(const char* title, const char* subtitle) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.95f, 0.98f, 1.0f));
    ImGui::SetWindowFontScale(1.28f);
    ImGui::TextUnformatted(title);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    if (subtitle && *subtitle)
        ImGui::TextDisabled("%s", subtitle);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

void configureStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(14.0f, 14.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 9.0f);
    style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    style.CellPadding = ImVec2(9.0f, 7.0f);
    style.ScrollbarSize = 15.0f;

    auto& colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.086f, 0.105f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.105f, 0.119f, 0.143f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.105f, 0.119f, 0.143f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.22f, 0.25f, 0.30f, 0.70f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.19f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.23f, 0.29f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.29f, 0.38f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.15f, 0.18f, 0.22f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.29f, 0.39f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.36f, 0.52f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.16f, 0.31f, 0.43f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.38f, 0.53f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.18f, 0.43f, 0.61f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.27f, 0.68f, 0.91f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.66f, 0.72f, 1.0f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.13f, 0.16f, 0.20f, 1.0f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.12f, 0.14f, 0.17f, 0.65f);
}

void applyNativeTitleBarTheme(HWND window) noexcept {
    constexpr BOOL useDarkMode = TRUE;
    (void)DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                &useDarkMode, sizeof(useDarkMode));
    (void)DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR,
                                &titleBarBackground,
                                sizeof(titleBarBackground));
    (void)DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR,
                                &titleBarForeground,
                                sizeof(titleBarForeground));
    (void)DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &titleBarBorder,
                                sizeof(titleBarBorder));
}

void waitForMessagesUntil(UiClock::time_point deadline,
                          HANDLE waitTimer) noexcept {
    const auto remaining = deadline - UiClock::now();
    if (remaining <= UiClock::duration::zero())
        return;
    if (waitTimer) {
        const auto nanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(remaining)
                .count();
        LARGE_INTEGER dueTime{};
        dueTime.QuadPart =
            -std::max<LONGLONG>(1, (nanoseconds + 99) / 100);
        if (SetWaitableTimer(waitTimer, &dueTime, 0, nullptr, nullptr, FALSE)) {
            const HANDLE handles[]{waitTimer};
            (void)MsgWaitForMultipleObjectsEx(
                1, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            return;
        }
    }
    const auto timeout =
        std::chrono::ceil<std::chrono::milliseconds>(remaining);
    (void)MsgWaitForMultipleObjectsEx(
        0, nullptr,
        static_cast<DWORD>(std::clamp<long long>(timeout.count(), 1, 1000)),
        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

class ApplicationWindow {
  public:
    ApplicationWindow(HINSTANCE instance, GuiApplicationPaths paths)
        : instance_(instance), paths_(std::move(paths)),
          config_(Config::loadOrCreate(paths_.config)),
          navRetentionDraft_(config_.navRetention),
          preferences_(GuiPreferences::load(paths_.preferences)),
          settingsDraft_(preferences_), controller_(paths_.dataRoot),
          displayModeWatcher_(defaultStarcraftSettingsPath()) {
        (void)displayModeWatcher_.start();
        controller_.setReportVisibility(preferences_.reports);
        (void)applyManagedNavRetention(paths_.sessions,
                                       config_.navRetention);
        snapshot_ = controller_.snapshot();
    }

    ~ApplicationWindow() {
        controller_.setStateChanged({});
        controller_.shutdown();
        shutdownRenderer();
    }

    bool create(int showCommand) {
        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;
        int width = 1080;
        int height = 760;
        if (preferences_.window) {
            x = preferences_.window->x;
            y = preferences_.window->y;
            width = preferences_.window->width;
            height = preferences_.window->height;
        }
        window_ = CreateWindowExW(
            0, guiMainWindowClassName, L"Starcraft Mechanics Profiler",
            WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr, instance_,
            this);
        if (!window_)
            return false;
        applyNativeTitleBarTheme(window_);
        if (!createD3d(window_, d3d_) || !initializeRenderer()) {
            MessageBoxW(window_,
                        L"The DirectX 11 interface could not be initialized.",
                        L"Starcraft Mechanics Profiler", MB_OK | MB_ICONERROR);
            shutdownRenderer();
            DestroyWindow(window_);
            window_ = nullptr;
            return false;
        }
        controller_.setStateChanged([window = window_]() {
            if (IsWindow(window))
                PostMessageW(window, controllerChangedMessage, 0, 0);
        });
        refreshState();
        refreshStarCraftStatus();
        if (preferences_.minimizeToTray)
            addTrayIcon();
        SetTimer(window_, uiTimer, 1000, nullptr);
        ShowWindow(window_, showCommand == SW_HIDE ? SW_SHOWNORMAL : showCommand);
        UpdateWindow(window_);
        return true;
    }

    int run() {
        MSG message{};
        bool done = false;
        bool occluded = false;
        HANDLE waitTimer = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!waitTimer)
            waitTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        auto nextFrameAt = UiClock::now();
        auto nextOcclusionTestAt = nextFrameAt;
        while (!done) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    done = true;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (done)
                break;
            if (exitRequested_) {
                beginExit();
                continue;
            }
            if (!window_ || !IsWindowVisible(window_) || IsIconic(window_)) {
                nextFrameAt = UiClock::now();
                WaitMessage();
                continue;
            }

            auto now = UiClock::now();
            if (occluded) {
                if (now < nextOcclusionTestAt) {
                    waitForMessagesUntil(nextOcclusionTestAt, waitTimer);
                    continue;
                }
                const HRESULT testResult =
                    d3d_.swapChain
                        ? d3d_.swapChain->Present(0, DXGI_PRESENT_TEST)
                        : S_OK;
                if (testResult == DXGI_STATUS_OCCLUDED) {
                    nextOcclusionTestAt =
                        UiClock::now() + occlusionRetryInterval;
                    continue;
                }
                occluded = false;
                nextFrameAt = UiClock::now();
            }

            now = UiClock::now();
            if (now < nextFrameAt) {
                waitForMessagesUntil(nextFrameAt, waitTimer);
                continue;
            }
            const auto frameStartedAt = now;
            if (renderFrame() == DXGI_STATUS_OCCLUDED) {
                occluded = true;
                nextOcclusionTestAt =
                    UiClock::now() + occlusionRetryInterval;
            }
            nextFrameAt = frameStartedAt + uiFrameInterval;
        }
        if (waitTimer)
            CloseHandle(waitTimer);
        return static_cast<int>(message.wParam);
    }

    static LRESULT CALLBACK windowProcedure(HWND window, UINT message,
                                            WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<ApplicationWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<ApplicationWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        if (self && self->imguiReady_ &&
            ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
            return TRUE;
        return self ? self->handleMessage(message, wParam, lParam)
                    : DefWindowProcW(window, message, wParam, lParam);
    }

  private:
    bool initializeRenderer() noexcept {
        IMGUI_CHECKVERSION();
        imguiContextCreated_ = ImGui::CreateContext() != nullptr;
        if (!imguiContextCreated_)
            return false;
        implotContextCreated_ = ImPlot::CreateContext() != nullptr;
        if (!implotContextCreated_)
            return false;
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        configureFonts(io);
        configureStyle();
        win32BackendInitialized_ = ImGui_ImplWin32_Init(window_);
        if (win32BackendInitialized_)
            dx11BackendInitialized_ =
                ImGui_ImplDX11_Init(d3d_.device, d3d_.context);
        imguiReady_ = win32BackendInitialized_ && dx11BackendInitialized_;
        return imguiReady_;
    }

    void shutdownRenderer() noexcept {
        imguiReady_ = false;
        if (dx11BackendInitialized_) {
            ImGui_ImplDX11_Shutdown();
            dx11BackendInitialized_ = false;
        }
        if (win32BackendInitialized_) {
            ImGui_ImplWin32_Shutdown();
            win32BackendInitialized_ = false;
        }
        if (implotContextCreated_) {
            ImPlot::DestroyContext();
            implotContextCreated_ = false;
        }
        if (imguiContextCreated_) {
            ImGui::DestroyContext();
            imguiContextCreated_ = false;
        }
        d3d_.cleanup();
    }

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (showExistingInstanceMessage_ != 0 &&
            message == showExistingInstanceMessage_) {
            restoreWindow();
            return 0;
        }
        if (message == taskbarCreatedMessage_) {
            trayAdded_ = false;
            if (preferences_.minimizeToTray)
                addTrayIcon();
            return 0;
        }
        switch (message) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                if (minimizeAction(preferences_.minimizeToTray) ==
                    MainWindowAction::HideToTray)
                    ShowWindow(window_, SW_HIDE);
                return 0;
            }
            resizeRenderer(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = {780, 600};
            return 0;
        }
        case WM_CLOSE:
            if (!exiting_ && closeAction(preferences_.minimizeToTray) ==
                                 MainWindowAction::HideToTray) {
                ShowWindow(window_, SW_HIDE);
                return 0;
            }
            exitRequested_ = true;
            return 0;
        case WM_TIMER:
            if (wParam == uiTimer) {
                controller_.reapFinished();
                refreshStarCraftStatus();
            }
            return 0;
        case controllerChangedMessage:
            refreshState();
            return 0;
        case trayMessage:
            handleTrayMessage(lParam);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam)
                beginExit();
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU)
                return 0;
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(window_, uiTimer);
            removeTrayIcon();
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    void resizeRenderer(UINT width, UINT height) noexcept {
        if (!d3d_.device || !d3d_.swapChain || width == 0 || height == 0)
            return;
        d3d_.destroyRenderTarget();
        if (SUCCEEDED(d3d_.swapChain->ResizeBuffers(
                0, width, height, DXGI_FORMAT_UNKNOWN, 0)))
            (void)d3d_.createRenderTarget();
    }

    HRESULT renderFrame() {
        if (!imguiReady_ || !d3d_.renderTarget)
            return S_OK;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        drawApplication();
        ImGui::Render();
        constexpr float clearColor[4]{0.055f, 0.064f, 0.078f, 1.0f};
        d3d_.context->OMSetRenderTargets(1, &d3d_.renderTarget, nullptr);
        d3d_.context->ClearRenderTargetView(d3d_.renderTarget, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        executePendingFullContentCapture(d3d_.device, d3d_.context,
                                         d3d_.renderTarget, window_);
        return d3d_.swapChain->Present(1, 0);
    }

    void drawApplication() {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        constexpr ImGuiWindowFlags rootFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Starcraft Mechanics Profiler##Root", nullptr, rootFlags);
        ImGui::PopStyleVar();
        drawSidebar();
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              ImVec4(0.075f, 0.086f, 0.105f, 1.0f));
        ImGui::BeginChild("##Content", ImVec2(0.0f, 0.0f), false,
                          ImGuiWindowFlags_NoSavedSettings);
        switch (page_) {
        case ApplicationPage::Main: drawMainPage(); break;
        case ApplicationPage::Results: drawResultsPage(); break;
        case ApplicationPage::Analysis: drawAnalysisPage(); break;
        case ApplicationPage::Settings: drawSettingsPage(); break;
        case ApplicationPage::About: drawAboutPage(); break;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::End();
    }

    void drawSidebar() {
        constexpr float sidebarWidth = 210.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              ImVec4(0.095f, 0.109f, 0.132f, 1.0f));
        ImGui::BeginChild("##Sidebar", ImVec2(sidebarWidth, 0.0f), false,
                          ImGuiWindowFlags_NoSavedSettings);
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SetWindowFontScale(1.10f);
        ImGui::TextUnformatted("Starcraft Mechanics");
        ImGui::TextUnformatted("Profiler");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("Version %s",
                            STARCRAFT_MECHANICS_PROFILER_VERSION);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        const auto navigationButton = [this](const char* label,
                                              ApplicationPage page) {
            const bool selected = page_ == page;
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.16f, 0.38f, 0.55f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(0.18f, 0.43f, 0.62f, 1.0f));
            }
            if (ImGui::Button(label, ImVec2(-1.0f, 42.0f)))
                selectPage(page);
            if (selected)
                ImGui::PopStyleColor(2);
        };
        navigationButton("Main", ApplicationPage::Main);
        navigationButton("Results", ApplicationPage::Results);
        navigationButton("Analysis", ApplicationPage::Analysis);
        navigationButton("Settings", ApplicationPage::Settings);
        navigationButton("About", ApplicationPage::About);
        ImGui::SetCursorPosY(std::max(
            ImGui::GetCursorPosY() + 12.0f,
            ImGui::GetWindowHeight() - 54.0f));
        ImGui::Separator();
        const char* activityText = profilerActivityName(snapshot_.activity);
        const float textWidth = ImGui::CalcTextSize(activityText).x;
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        if (textWidth < availableWidth) {
            ImGui::SetCursorPosX(
                ImGui::GetCursorPosX() +
                (availableWidth - textWidth) * 0.5f);
        }
        ImGui::TextDisabled("%s", activityText);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void drawMainPage() {
        pageHeading("Main", "Recorder status and controls");
        constexpr ImGuiChildFlags statusCardFlags =
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY;
        constexpr ImGuiWindowFlags statusCardWindowFlags =
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::BeginChild("##StatusCard", ImVec2(0.0f, 0.0f), statusCardFlags,
                          statusCardWindowFlags);
        ImGui::SeparatorText("Profiler status");
        if (ImGui::BeginTable(
                "##StatusTable", 2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed,
                                    160.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch,
                                    1.0f);
            const auto row = [](const char* label, const std::string& value) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", label);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextWrapped("%s", value.c_str());
            };
            row("Mode", applicationModeName(snapshot_.mode));
            row("Activity", profilerActivityName(snapshot_.activity));
            row("StarCraft", starCraftStatus_);
            row("Current session",
                std::to_string(snapshot_.currentSession.games) +
                    " completed game(s)");
            row("Detail", snapshot_.detail);
            row("Data folder", pathText(paths_.dataRoot));
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::Spacing();
        const bool automatic = snapshot_.workerRunning &&
                               snapshot_.mode == ApplicationMode::Automatic;
        const bool debug = snapshot_.workerRunning &&
                           snapshot_.mode == ApplicationMode::Debug;
        if (actionButton(automatic ? "Turn automatic detector off"
                                   : "Turn automatic detector on",
                         !snapshot_.workerRunning || automatic))
            toggleAutomatic();
        ImGui::SameLine();
        if (actionButton(debug ? "Stop live detection"
                               : "Test live detection",
                         !snapshot_.workerRunning || debug))
            toggleDebug();
        ImGui::Spacing();
        if (ImGui::Button("Open data folder"))
            openPath(paths_.dataRoot);
        ImGui::SameLine();
        if (actionButton("View latest results", snapshot_.latestGame.has_value())) {
            resultSource_ = ResultSource::LatestGame;
            resultsDirty_ = true;
            selectPage(ApplicationPage::Results);
        }
        ImGui::SameLine();
        if (ImGui::Button("Exit"))
            exitRequested_ = true;
        ImGui::Spacing();
        ImGui::SeparatorText("Live diagnostic log");
        ImGui::BeginChild("##DiagnosticLog", ImVec2(0.0f, 0.0f), true,
                          ImGuiWindowFlags_HorizontalScrollbar |
                              ImGuiWindowFlags_NoSavedSettings);
        const bool wasAtBottom =
            ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        if (snapshot_.diagnostics.empty()) {
            ImGui::TextDisabled("Live detection events appear here.");
        } else {
            for (const auto& line : snapshot_.diagnostics)
                ImGui::TextUnformatted(line.c_str());
        }
        if (snapshot_.diagnostics.size() != displayedDiagnosticCount_ &&
            (wasAtBottom || displayedDiagnosticCount_ == 0))
            ImGui::SetScrollHereY(1.0f);
        displayedDiagnosticCount_ = snapshot_.diagnostics.size();
        ImGui::EndChild();
    }

    void drawResultsPage() {
        pageHeading("Results", "Latest game and automatic-session summaries");
        updateResultsIfNeeded();
        ImGui::TextDisabled("View");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(190.0f);
        const char* selected = resultSource_ == ResultSource::LatestGame
                                   ? "Latest game"
                                   : "Current session";
        if (ImGui::BeginCombo("##ResultSource", selected)) {
            if (ImGui::Selectable("Latest game",
                                  resultSource_ == ResultSource::LatestGame)) {
                resultSource_ = ResultSource::LatestGame;
                resultsDirty_ = true;
            }
            if (ImGui::Selectable("Current session",
                                  resultSource_ == ResultSource::CurrentSession)) {
                resultSource_ = ResultSource::CurrentSession;
                resultsDirty_ = true;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (actionButton("Open result file", snapshot_.latestGame.has_value()))
            openLatestResult();
        ImGui::Spacing();
        if (ImGui::Button("Open latest session summary"))
            openLatestSessionSummary();
        ImGui::SameLine();
        if (ImGui::Button("Export latest CSV"))
            exportLatestCsv();
        ImGui::Spacing();
        ImGui::BeginChild("##ResultsScroll", ImVec2(0.0f, 0.0f), false,
                          ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(0.94f, 0.97f, 1.0f, 1.0f));
        ImGui::SetWindowFontScale(1.22f);
        ImGui::TextUnformatted(resultsModel_.title.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        if (!resultsModel_.subtitle.empty())
            ImGui::TextDisabled("%s", resultsModel_.subtitle.c_str());
        ImGui::Spacing();
        ImGui::Spacing();
        for (std::size_t index = 0; index < resultsModel_.sections.size(); ++index) {
            const auto& section = resultsModel_.sections[index];
            ImGui::PushID(static_cast<int>(index));
            constexpr ImGuiChildFlags resultCardFlags =
                ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY;
            constexpr ImGuiWindowFlags resultCardWindowFlags =
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse;
            ImGui::BeginChild("##ResultSection", ImVec2(0.0f, 0.0f),
                              resultCardFlags, resultCardWindowFlags);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.82f, 0.91f, 0.98f, 1.0f));
            ImGui::SetWindowFontScale(1.10f);
            ImGui::SeparatorText(section.title.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (ImGui::BeginTable(
                    "##Metrics", 2,
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableSetupColumn("Metric",
                                        ImGuiTableColumnFlags_WidthStretch, 0.60f);
                ImGui::TableSetupColumn("Value",
                                        ImGuiTableColumnFlags_WidthStretch, 0.40f);
                for (const auto& metric : section.metrics) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(
                        ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    bool showTooltip = false;
                    if (metric.tooltip.empty()) {
                        ImGui::TextWrapped("%s", metric.label.c_str());
                    } else {
                        ImGui::TextUnformatted(metric.label.c_str());
                        showTooltip = ImGui::IsItemHovered();
                        ImGui::SameLine(0.0f, 5.0f);
                        ImGui::TextDisabled("(?)");
                        showTooltip = showTooltip || ImGui::IsItemHovered();
                    }
                    ImGui::PopStyleColor();
                    if (showTooltip) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
                        ImGui::TextUnformatted(metric.tooltip.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(0.94f, 0.97f, 1.0f, 1.0f));
                    ImGui::TextWrapped("%s", metric.value.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
            ImGui::Spacing();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    void drawAnalysisPage() {
        pageHeading("Analysis", "Latest-game mechanics and session trends");
        ensureAnalysisLoaded();
        ImGui::BeginChild(
            "##AnalysisPane", ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse);
        if (!snapshot_.latestGame || snapshot_.latestGamePath.empty()) {
            ImGui::TextDisabled(
                "No completed game is available to analyze yet.");
        } else if (!analysisError_.empty()) {
            ImGui::TextColored(ImVec4(0.92f, 0.48f, 0.32f, 1.0f), "%s",
                               analysisError_.c_str());
        } else if (analysisModel_) {
            drawAnalysisViewWithClipboard(
                *analysisModel_, analysisViewState_, preferences_.reports,
                preferences_.sessionReports);
        }
        ImGui::EndChild();
    }

    void drawSettingsPage() {
        pageHeading("Settings", "Analysis display and application preferences");
        ImGui::BeginChild("##SettingsScroll", ImVec2(0.0f, 0.0f), false,
                          ImGuiWindowFlags_NoSavedSettings);
        constexpr ImGuiChildFlags analysisDisplayFlags =
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY;
        constexpr ImGuiWindowFlags analysisDisplayWindowFlags =
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::BeginChild("##AnalysisDisplay", ImVec2(0.0f, 0.0f),
                          analysisDisplayFlags, analysisDisplayWindowFlags);
        ImGui::SeparatorText("Latest game analysis / results");
        ImGui::TextWrapped(
            "Choose which latest-game analysis and numerical results are shown. "
            "Hiding a section does not stop collection or remove saved data.");
        ImGui::Spacing();
        ImGui::Checkbox("Game timeline", &settingsDraft_.reports.gameTimeline);

        ImGui::TextDisabled("Macro");
        ImGui::Indent();
        ImGui::Checkbox("Worker macro cycles",
                        &settingsDraft_.reports.workerMacroCycles);
        ImGui::Checkbox("Army macro cycles",
                        &settingsDraft_.reports.armyMacroCycles);
        ImGui::Checkbox("Macro gaps", &settingsDraft_.reports.macroGaps);
        ImGui::Checkbox("Macro-duration distribution",
                        &settingsDraft_.reports.macroDurationDistribution);
        ImGui::Checkbox("Macro access styles",
                        &settingsDraft_.reports.macroAccessStyles);
        ImGui::Unindent();

        ImGui::TextDisabled("Army Management");
        ImGui::Indent();
        ImGui::Checkbox("Army control-group management",
                        &settingsDraft_.reports.armyControlGroupManagement);
        ImGui::Checkbox("Army Command Activity",
                        &settingsDraft_.reports.armyCommandActivity);
        ImGui::Checkbox("Ability Activity",
                        &settingsDraft_.reports.abilityActivity);
        ImGui::Unindent();

        ImGui::TextDisabled("Multitasking");
        ImGui::Indent();
        ImGui::Checkbox("Navigation transition rate",
                        &settingsDraft_.reports.navigationTransitionRate);
        ImGui::Checkbox("Multitasking Heatmap",
                        &settingsDraft_.reports.multitaskingDensity);
        ImGui::Checkbox("Scouting activity",
                        &settingsDraft_.reports.scoutingUnitActivity);
        ImGui::Checkbox("Camera Navigation Methods",
                        &settingsDraft_.reports.cameraNavigation);
        ImGui::Unindent();
        ImGui::Spacing();
        if (ImGui::Button("Select all"))
            settingsDraft_.reports.selectAll();
        ImGui::SameLine();
        if (ImGui::Button("Clear all"))
            settingsDraft_.reports.clearAll();
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::BeginChild("##SessionAnalysisDisplay", ImVec2(0.0f, 0.0f),
                          analysisDisplayFlags, analysisDisplayWindowFlags);
        ImGui::SeparatorText("Session trends / current session");
        ImGui::TextWrapped(
            "Choose which mechanical KPIs are shown in Session Trends "
            "and Current Session Results. Hiding a KPI does not stop "
            "collection or remove saved data.");
        ImGui::Spacing();
        for (const auto& groupDefinition : sessionKpiGroupDefinitions) {
            const auto group = groupDefinition.group;
            ImGui::TextDisabled("%s", groupDefinition.title);
            ImGui::Indent();
            for (const auto& definition : sessionKpiDefinitions) {
                if (definition.group != group)
                    continue;
                bool enabled =
                    settingsDraft_.sessionReports.visible(definition.kpi);
                const std::string label =
                    std::string(definition.settingsLabel) +
                    "##SessionKpi." + definition.preferenceKey;
                if (ImGui::Checkbox(label.c_str(), &enabled))
                    settingsDraft_.sessionReports.set(definition.kpi,
                                                      enabled);
            }
            ImGui::Unindent();
        }
        ImGui::Spacing();
        if (ImGui::Button("Select all##SessionReports"))
            settingsDraft_.sessionReports.selectAll();
        ImGui::SameLine();
        if (ImGui::Button("Clear all##SessionReports"))
            settingsDraft_.sessionReports.clearAll();
        ImGui::SameLine();
        if (ImGui::Button("Restore defaults##SessionReports"))
            settingsDraft_.sessionReports.restoreDefaults();
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::BeginChild("##ApplicationSettings", ImVec2(0.0f, 110.0f), true,
                          ImGuiWindowFlags_NoSavedSettings);
        ImGui::SeparatorText("Application");
        ImGui::Checkbox("Minimize to tray", &settingsDraft_.minimizeToTray);
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::BeginChild("##NavigationRetention", ImVec2(0.0f, 0.0f),
                          analysisDisplayFlags, analysisDisplayWindowFlags);
        ImGui::SeparatorText("Navigation data retention");
        constexpr const char* retentionPolicies[]{
            "Keep all .nav files", "Keep last N games"};
        int retentionMode =
            navRetentionDraft_.mode == NavRetentionMode::KeepLastGames ? 1 : 0;
        if (ImGui::Combo("Policy", &retentionMode, retentionPolicies,
                         static_cast<int>(std::size(retentionPolicies)))) {
            navRetentionDraft_.mode =
                retentionMode == 1 ? NavRetentionMode::KeepLastGames
                                   : NavRetentionMode::KeepAll;
        }
        if (navRetentionDraft_.mode == NavRetentionMode::KeepLastGames) {
            if (ImGui::InputInt("Games to keep",
                                &navRetentionDraft_.gamesToKeep, 1, 10)) {
                navRetentionDraft_.gamesToKeep =
                    std::max(1, navRetentionDraft_.gamesToKeep);
            }
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.95f, 0.68f, 0.28f, 1.0f));
            ImGui::TextWrapped(
                "Deleting older .nav files saves storage space, but prevents "
                "those games from being re-analyzed if analysis logic is "
                "improved or changed later. Existing derived/session-summary "
                "data will be kept.");
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::BeginChild("##AdvancedSettings", ImVec2(0.0f, 132.0f), true,
                          ImGuiWindowFlags_NoSavedSettings);
        ImGui::SeparatorText("Advanced");
        if (actionButton("Calibrate minimap override", !snapshot_.workerRunning))
            startCalibration();
        ImGui::SameLine();
        if (actionButton("Use automatic minimap", !snapshot_.workerRunning))
            useAutomaticMinimap();
        ImGui::SameLine();
        if (ImGui::Button("Open config.json"))
            openPath(paths_.config);
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::SeparatorText("Settings persistence");
        if (ImGui::Button("Save settings", ImVec2(180.0f, 0.0f)))
            saveSettings();
        ImGui::EndChild();
    }

    void drawAboutPage() {
        pageHeading("About", "Profiler purpose, data model, and statistic definitions");
        ImGui::BeginChild("##AboutScroll", ImVec2(0.0f, 0.0f), false,
                          ImGuiWindowFlags_NoSavedSettings);

        beginAboutCard("##AboutOverview", "Starcraft Mechanics Profiler");
        ImGui::TextDisabled("Version %s", STARCRAFT_MECHANICS_PROFILER_VERSION);
        ImGui::Spacing();
        ImGui::TextWrapped(
            "A lightweight native Windows mechanical profiler for StarCraft: "
            "Remastered. It combines foreground-only Raw Input telemetry with "
            "replay-derived context. Physical QPC timestamps are authoritative "
            "for mechanical timing; replay frames provide semantic identity and "
            "meaning after recording has stopped.");
        endAboutCard();

        beginAboutCard("##AboutStatistics", "What the statistics mean");
        aboutDefinition(
            "Camera navigation",
            "Detected control-group jumps, location-hotkey jumps, minimap jumps, "
            "and qualifying edge-pan episodes. These describe navigation method, "
            "not whether the camera movement was strategically good.");
        aboutDefinition(
            "Production visit",
            "One occasion where a specific production building is accessed and at "
            "least one detected production attempt is made. It is not the number "
            "of units produced. Building identity is retained internally so visits "
            "to the same structure can be distinguished from visits to another one.");
        aboutDefinition(
            "Worker / Army macro cycle",
            "A continuous production pass that can contain multiple production "
            "visits. Cycle execution is timed from access to the first building "
            "through the first production attempt in the final visit. Replay data "
            "identifies product type and building identity; physical QPC provides "
            "the timing.");
        aboutDefinition(
            "Macro access style",
            "How production buildings in a cycle were reached: Control Group Only, "
            "Location Hotkey Click, Control Group Center Click, or Mixed. "
            "Unclassified observations are omitted from the user-facing breakdown.");
        aboutDefinition(
            "Army control-group management",
            "Replay-confirmed Ctrl+number assignments and Shift+number additions "
            "for non-production, non-scouting army groups. Selection formation can "
            "include direct click, box select, type selection, and Shift-modification "
            "methods. Ambiguous edits are excluded from headline statistics.");
        aboutDefinition(
            "Scouting-unit activity",
            "An early singleton worker is confirmed as a scout when replay-attributed "
            "commands by that same unit tag move onto the opponent's side of the map "
            "relative to the occupied starting locations. Tracking follows the unit "
            "tag rather than a hotkey. The observed span ends at a confirmed return "
            "home after the final enemy-side excursion, or otherwise at the unit's "
            "final attributable command. It is not literal survival or death time.");
        endAboutCard();

        beginAboutCard("##AboutData", "Data and timing");
        aboutDefinition(
            "Physical timing",
            "Raw Input is captured only while StarCraft owns the foreground window. "
            "Active game time excludes foreground pauses such as Alt+Tab, while QPC "
            "timestamps preserve precise physical timing for mechanical measurements.");
        aboutDefinition(
            "Replay context",
            "The settled LastReplay replay is parsed only after recording stops. "
            "Replay correlation supplies unit identity, production meaning, starting "
            "locations, and other semantic context without reading game memory.");
        aboutDefinition(
            "Session files",
            "Each completed game stores a compact .nav source-of-truth event stream "
            "and a derived .json analysis. Full raw input is saved only when "
            "--save-raw is explicitly requested; CSV files are created only by export.");
        endAboutCard();

        beginAboutCard("##AboutApplication", "Application behavior");
        aboutDefinition(
            "Automatic recording",
            "While waiting for a game, only the resolved minimap region is sampled "
            "for the viewport-outline start signal. Sampling stops during recording, "
            "and LastReplay metadata is used to finalize the completed game.");
        aboutDefinition(
            "Minimize to tray",
            "When enabled, minimizing or closing hides the profiler to the notification "
            "area. When disabled, minimize behaves normally on the taskbar and Close "
            "or Alt+F4 exits the application.");
        aboutDefinition(
            "Minimap geometry",
            "Built-in geometry supports Original Aspect and Widescreen. Manual minimap "
            "calibration is an optional per-display-mode override and can be replaced "
            "again with automatic geometry from Settings.");
        endAboutCard();

        beginAboutCard("##AboutCommands", "Command-line reference");
        ImGui::TextUnformatted(
            "record [--debug-navigation] [--debug-regions] [--show-raw-events] "
            "[--save-raw] [--verbose] [--quiet]\n"
            "auto [same options as record]\n"
            "debug | calibrate | config\n"
            "summary <latest|session-id>\n"
            "compare <session-id> <session-id> | compare last <N>\n"
            "export <latest|session-id> --csv");
        endAboutCard();

        beginAboutCard("##AboutScope", "Scope");
        ImGui::TextWrapped(
            "The profiler does not read StarCraft process memory, inspect network "
            "traffic, inject code, modify input, or infer strategic quality. It is "
            "intended to describe observable mechanical behavior and timing, not to "
            "decide whether a player's build, tactics, or decisions were correct.");
        endAboutCard();

        ImGui::EndChild();
    }

    static void beginAboutCard(const char* id, const char* title) {
        constexpr ImGuiChildFlags cardFlags =
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY;
        constexpr ImGuiWindowFlags windowFlags =
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), cardFlags, windowFlags);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(0.82f, 0.91f, 0.98f, 1.0f));
        ImGui::SetWindowFontScale(1.10f);
        ImGui::SeparatorText(title);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    static void endAboutCard() {
        ImGui::EndChild();
        ImGui::Spacing();
    }

    static void aboutDefinition(const char* title, const char* description) {
        ImGui::TextDisabled("%s", title);
        ImGui::TextWrapped("%s", description);
        ImGui::Spacing();
    }

    void selectPage(ApplicationPage page) {
        page_ = page;
        if (page == ApplicationPage::Results)
            resultsDirty_ = true;
    }

    void toggleAutomatic() {
        const auto state = controller_.snapshot();
        if (state.workerRunning) {
            if (state.mode == ApplicationMode::Automatic)
                controller_.stopCurrent();
            return;
        }
        try {
            config_ = Config::loadOrCreate(paths_.config);
            controller_.setReportVisibility(preferences_.reports);
            (void)controller_.startAutomatic(config_);
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void toggleDebug() {
        const auto state = controller_.snapshot();
        if (state.workerRunning) {
            if (state.mode == ApplicationMode::Debug)
                controller_.stopCurrent();
            return;
        }
        try {
            config_ = Config::loadOrCreate(paths_.config);
            (void)controller_.startDebug(config_);
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void startCalibration() {
        if (controller_.snapshot().workerRunning)
            return;
        const int answer = MessageBoxW(
            window_,
            L"After starting, switch to StarCraft. Move to the minimap top-left "
            L"and press the configured capture key, then repeat at the "
            L"bottom-right. This will override the automatic minimap "
            L"geometry.\n\nStart calibration?",
            L"Minimap override calibration", MB_YESNO | MB_ICONINFORMATION);
        if (answer != IDYES)
            return;
        try {
            config_ = Config::loadOrCreate(paths_.config);
            (void)controller_.startCalibration(config_, paths_.config);
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void useAutomaticMinimap() {
        if (controller_.snapshot().workerRunning)
            return;
        try {
            config_ = Config::loadOrCreate(paths_.config);
            const auto displayMode = displayModeWatcher_.mode();
            if (displayMode == StarcraftDisplayMode::Widescreen)
                config_.useWidescreenAutomaticMinimap();
            else
                config_.useOriginalAspectAutomaticMinimap();
            config_.save(paths_.config);
            const wchar_t* modeName =
                displayMode == StarcraftDisplayMode::Widescreen
                    ? L"Widescreen"
                    : L"Original Aspect";
            const std::wstring message =
                std::wstring(L"Automatic minimap geometry is now active for ") +
                modeName + L".";
            MessageBoxW(window_, message.c_str(), L"Minimap geometry",
                        MB_OK | MB_ICONINFORMATION);
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void refreshState() {
        const auto previousPath = snapshot_.latestGamePath;
        snapshot_ = controller_.snapshot();
        resultsDirty_ = true;
        updateTrayTooltip(snapshot_);
        if (snapshot_.latestGamePath != previousPath)
            analysisLoadRequested_ = true;
        if (lastMode_ == ApplicationMode::Calibration &&
            snapshot_.mode == ApplicationMode::None) {
            try {
                config_ = Config::loadOrCreate(paths_.config);
            } catch (...) {
            }
        }
        lastMode_ = snapshot_.mode;
    }

    void refreshStarCraftStatus() {
        try {
            ForegroundMatcher foreground(config_.starcraftProcess);
            starCraftStatus_ = foreground.matches(GetForegroundWindow())
                                    ? "Foreground"
                                    : "Not foreground";
        } catch (...) {
            starCraftStatus_ = "Unavailable";
        }
    }

    void updateResultsIfNeeded() {
        if (!resultsDirty_)
            return;
        if (resultSource_ == ResultSource::LatestGame) {
            if (snapshot_.latestGame) {
                ensureAnalysisLoaded();
                resultsModel_ = deriveGameResults(
                    *snapshot_.latestGame, preferences_.reports,
                    analysisModel_ ? &*analysisModel_ : nullptr);
            } else {
                resultsModel_ = {
                    "Latest Game", "No completed result is available",
                    {{"status", "Results", {{"Status", "No result available"}}}}};
            }
        } else {
            resultsModel_ = deriveSessionResults(snapshot_.currentSession,
                                                 preferences_.sessionReports);
        }
        resultsDirty_ = false;
    }

    void ensureAnalysisLoaded() {
        if (!snapshot_.latestGame || snapshot_.latestGamePath.empty()) {
            analysisModel_.reset();
            analysisSourcePath_.clear();
            analysisError_.clear();
            analysisLoadRequested_ = false;
            return;
        }
        if (!analysisLoadRequested_ &&
            analysisSourcePath_ == snapshot_.latestGamePath)
            return;
        const bool sourceChanged =
            analysisSourcePath_ != snapshot_.latestGamePath;
        analysisLoadRequested_ = false;
        analysisSourcePath_ = snapshot_.latestGamePath;
        analysisModel_.reset();
        analysisError_.clear();
        try {
            analysisModel_ =
                loadGameAnalysisVisualizationModel(snapshot_.latestGamePath);
            if (sourceChanged)
                analysisViewState_.fitTimeline = true;
        } catch (const std::exception& error) {
            analysisError_ = error.what();
        } catch (...) {
            analysisError_ = "The latest game analysis could not be loaded.";
        }
    }

    void saveSettings() {
        try {
            const auto previousRetention = config_.navRetention;
            config_.navRetention =
                normalizedNavRetentionPolicy(navRetentionDraft_);
            navRetentionDraft_ = config_.navRetention;
            config_.save(paths_.config);
            preferences_.reports = settingsDraft_.reports;
            preferences_.sessionReports = settingsDraft_.sessionReports;
            preferences_.minimizeToTray = settingsDraft_.minimizeToTray;
            preferences_.save(paths_.preferences);
            controller_.setReportVisibility(preferences_.reports);
            if (preferences_.minimizeToTray) {
                if (!trayAdded_)
                    addTrayIcon();
            } else {
                removeTrayIcon();
            }
            resultsDirty_ = true;
            std::wstring message = L"Settings saved.";
            if (config_.navRetention != previousRetention) {
                const auto retention = applyManagedNavRetention(
                    paths_.sessions, config_.navRetention);
                if (!retention.warning.empty()) {
                    message += L"\n\nNavigation cleanup notice: ";
                    message += wide(retention.warning);
                } else if (!retention.cleanup.failedPaths.empty()) {
                    message += L"\n\nSome older .nav files could not be removed "
                               L"and were left in place.";
                }
            }
            MessageBoxW(window_, message.c_str(),
                        L"Starcraft Mechanics Profiler",
                        MB_OK | MB_ICONINFORMATION);
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void openPath(const std::filesystem::path& path) {
        if (path.empty() || !std::filesystem::exists(path)) {
            const std::wstring message =
                L"The requested file or folder does not exist:\n" + path.wstring();
            MessageBoxW(window_, message.c_str(), L"Unable to open",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                window_, L"open", path.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL)) <= 32) {
            const std::wstring message = L"Windows could not open:\n" + path.wstring();
            MessageBoxW(window_, message.c_str(), L"Unable to open",
                        MB_OK | MB_ICONERROR);
        }
    }

    void openLatestResult() {
        if (!snapshot_.latestGamePath.empty())
            openPath(snapshot_.latestGamePath);
    }

    void openLatestSessionSummary() {
        const auto path = findLatestAutomaticSessionSummary(paths_.sessions);
        if (!path) {
            MessageBoxW(window_,
                        L"No saved automatic session summary is available yet.",
                        L"Session summary", MB_OK | MB_ICONINFORMATION);
            return;
        }
        openPath(*path);
    }

    void exportLatestCsv() {
        try {
            const auto exported =
                exportSessionCsv(paths_.sessions, paths_.exports, "latest");
            const std::wstring message = L"Exported to:\n" + exported.wstring();
            MessageBoxW(window_, message.c_str(), L"CSV export complete",
                        MB_OK | MB_ICONINFORMATION);
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void restoreWindow() {
        if (!window_)
            return;
        if (IsIconic(window_))
            ShowWindow(window_, SW_RESTORE);
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
    }

    void handleTrayMessage(LPARAM lParam) {
        if (!preferences_.minimizeToTray)
            return;
        const UINT event = LOWORD(lParam);
        if (event == WM_LBUTTONDBLCLK || event == NIN_SELECT ||
            event == NIN_KEYSELECT) {
            restoreWindow();
            return;
        }
        if (event != WM_RBUTTONUP && event != WM_CONTEXTMENU)
            return;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | MF_DEFAULT, TrayOpen, L"Open");
        std::wstring status = L"Status: ";
        status += wide(profilerActivityName(snapshot_.activity));
        AppendMenuW(menu, MF_STRING | MF_GRAYED, TrayStatus, status.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const bool automatic = snapshot_.workerRunning &&
                               snapshot_.mode == ApplicationMode::Automatic;
        AppendMenuW(menu,
                    MF_STRING |
                        (snapshot_.workerRunning && !automatic ? MF_GRAYED : 0),
                    TrayToggleAutomatic,
                    automatic ? L"Stop automatic detector"
                              : L"Start automatic detector");
        AppendMenuW(menu,
                    MF_STRING | (snapshot_.latestGame ? 0 : MF_GRAYED),
                    TrayLatest, L"Open latest result");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, TrayExit, L"Exit");
        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenu(
            menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN |
                      TPM_RETURNCMD | TPM_NONOTIFY,
            cursor.x, cursor.y, 0, window_, nullptr);
        DestroyMenu(menu);
        switch (command) {
        case TrayOpen: restoreWindow(); break;
        case TrayToggleAutomatic: toggleAutomatic(); break;
        case TrayLatest:
            restoreWindow();
            resultSource_ = ResultSource::LatestGame;
            selectPage(ApplicationPage::Results);
            break;
        case TrayExit: exitRequested_ = true; break;
        default: break;
        }
        PostMessageW(window_, WM_NULL, 0, 0);
    }

    void addTrayIcon() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = trayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = trayMessage;
        data.hIcon = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16,
            LR_DEFAULTCOLOR | LR_SHARED));
        wcscpy_s(data.szTip, L"Starcraft Mechanics Profiler - Idle");
        Shell_NotifyIconW(NIM_ADD, &data);
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        trayAdded_ = true;
        updateTrayTooltip(snapshot_);
    }

    void updateTrayTooltip(const ApplicationSnapshot& state) {
        if (!trayAdded_)
            return;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = trayIconId;
        data.uFlags = NIF_TIP;
        std::wstring tooltip = L"Starcraft Mechanics Profiler - ";
        tooltip += wide(profilerActivityName(state.activity));
        wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void removeTrayIcon() {
        if (!trayAdded_)
            return;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = trayIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        trayAdded_ = false;
    }

    void persistWindowPlacement() noexcept {
        try {
            if (window_ && !IsIconic(window_)) {
                RECT rect{};
                if (GetWindowRect(window_, &rect)) {
                    GuiWindowPlacement placement{
                        rect.left, rect.top, rect.right - rect.left,
                        rect.bottom - rect.top};
                    if (placement.valid())
                        preferences_.window = placement;
                }
            }
            preferences_.save(paths_.preferences);
        } catch (...) {
        }
    }

    void beginExit() {
        if (exiting_)
            return;
        exiting_ = true;
        exitRequested_ = false;
        persistWindowPlacement();
        controller_.setStateChanged({});
        controller_.shutdown();
        if (window_ && IsWindow(window_))
            DestroyWindow(window_);
    }

    void showError(const std::string& message) {
        const auto converted = wide(message);
        MessageBoxW(window_, converted.c_str(), L"Starcraft Mechanics Profiler",
                    MB_OK | MB_ICONERROR);
    }

    HINSTANCE instance_{};
    GuiApplicationPaths paths_;
    Config config_;
    NavRetentionPolicy navRetentionDraft_;
    GuiPreferences preferences_;
    GuiPreferences settingsDraft_;
    ApplicationController controller_;
    StarcraftDisplayModeWatcher displayModeWatcher_;
    ApplicationSnapshot snapshot_;
    D3dResources d3d_;
    HWND window_{};
    ApplicationPage page_{ApplicationPage::Main};
    ResultSource resultSource_{ResultSource::LatestGame};
    ResultsViewModel resultsModel_{};
    std::optional<GameAnalysisVisualizationModel> analysisModel_;
    AnalysisViewState analysisViewState_;
    std::filesystem::path analysisSourcePath_;
    std::string analysisError_;
    std::string starCraftStatus_{"Not foreground"};
    std::size_t displayedDiagnosticCount_{};
    ApplicationMode lastMode_{ApplicationMode::None};
    bool resultsDirty_{true};
    bool analysisLoadRequested_{true};
    bool trayAdded_{};
    bool exiting_{};
    bool exitRequested_{};
    bool imguiReady_{};
    bool imguiContextCreated_{};
    bool implotContextCreated_{};
    bool win32BackendInitialized_{};
    bool dx11BackendInitialized_{};
    UINT taskbarCreatedMessage_{RegisterWindowMessageW(L"TaskbarCreated")};
    UINT showExistingInstanceMessage_{showExistingGuiInstanceMessage()};
};

} // namespace

int runWindowsApplication(HINSTANCE instance, const GuiApplicationPaths& paths,
                          int showCommand) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = ApplicationWindow::windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32,
        LR_DEFAULTCOLOR | LR_SHARED));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16,
        LR_DEFAULTCOLOR | LR_SHARED));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = guiMainWindowClassName;
    if (!RegisterClassExW(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 1;
    ApplicationWindow application(instance, paths);
    if (!application.create(showCommand))
        return 1;
    return application.run();
}

} // namespace smp
