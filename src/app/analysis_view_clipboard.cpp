#include "app/analysis_view.h"
#include "app/session_trends.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>
#include <windows.h>

namespace smp {
namespace {

bool copyVisibleAnalysisPaneToClipboard(HWND window) noexcept {
    if (!window)
        return false;

    RECT client{};
    if (!GetClientRect(window, &client))
        return false;

    const ImVec2 panePosition = ImGui::GetWindowPos();
    const ImVec2 paneSize = ImGui::GetWindowSize();
    const int clientWidth = std::max(0L, client.right - client.left);
    const int clientHeight = std::max(0L, client.bottom - client.top);
    const int left = std::clamp(static_cast<int>(std::floor(panePosition.x)),
                                0, clientWidth);
    const int top = std::clamp(static_cast<int>(std::floor(panePosition.y)),
                               0, clientHeight);
    const int right = std::clamp(
        static_cast<int>(std::ceil(panePosition.x + paneSize.x)), 0,
        clientWidth);
    const int bottom = std::clamp(
        static_cast<int>(std::ceil(panePosition.y + paneSize.y)), 0,
        clientHeight);
    const int width = right - left;
    const int height = bottom - top;
    if (width <= 0 || height <= 0)
        return false;

    POINT source{left, top};
    if (!ClientToScreen(window, &source))
        return false;

    HDC screenDc = GetDC(nullptr);
    if (!screenDc)
        return false;
    HDC memoryDc = CreateCompatibleDC(screenDc);
    if (!memoryDc) {
        ReleaseDC(nullptr, screenDc);
        return false;
    }
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, width, height);
    if (!bitmap) {
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    const HGDIOBJ previous = SelectObject(memoryDc, bitmap);
    const BOOL copied = BitBlt(memoryDc, 0, 0, width, height, screenDc,
                               source.x, source.y, SRCCOPY | CAPTUREBLT);
    SelectObject(memoryDc, previous);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
    if (!copied) {
        DeleteObject(bitmap);
        return false;
    }

    if (!OpenClipboard(window)) {
        DeleteObject(bitmap);
        return false;
    }

    bool success = false;
    if (EmptyClipboard() && SetClipboardData(CF_BITMAP, bitmap)) {
        // Clipboard ownership transfers to Windows after SetClipboardData succeeds.
        bitmap = nullptr;
        success = true;
    }
    CloseClipboard();
    if (bitmap)
        DeleteObject(bitmap);
    return success;
}

std::filesystem::path sessionsDirectory() {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return std::filesystem::current_path() / "sessions";
    return std::filesystem::path(
               std::wstring(buffer.data(), static_cast<std::size_t>(length)))
               .parent_path() /
           "sessions";
}

} // namespace

void drawAnalysisViewWithClipboard(const GameAnalysisVisualizationModel& model,
                                   AnalysisViewState& state) {
    const ImVec2 panePosition = ImGui::GetWindowPos();
    const ImVec2 paneSize = ImGui::GetWindowSize();

    static int view = 0;
    ImGui::TextDisabled("View");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    const char* currentView = view == 0 ? "Latest game" : "Session trends";
    if (ImGui::BeginCombo("##AnalysisViewMode", currentView)) {
        if (ImGui::Selectable("Latest game", view == 0))
            view = 0;
        if (ImGui::Selectable("Session trends", view == 1))
            view = 1;
        ImGui::EndCombo();
    }
    ImGui::Spacing();

    if (view == 0)
        drawAnalysisView(model, state);
    else
        drawSessionTrends(sessionsDirectory());

    static double feedbackUntil{};
    static bool lastCopySucceeded{};
    const double now = ImGui::GetTime();
    const char* label = "Copy visible analysis";
    if (now < feedbackUntil)
        label = lastCopySucceeded ? "Copied to clipboard" : "Copy failed";

    constexpr float buttonWidth = 190.0f;
    const float rightPadding = ImGui::GetStyle().ScrollbarSize + 12.0f;
    const float buttonX = std::max(
        panePosition.x + 8.0f,
        panePosition.x + paneSize.x - buttonWidth - rightPadding);
    const float buttonY = panePosition.y + 10.0f;
    const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(buttonX, buttonY));
    if (ImGui::Button(label, ImVec2(buttonWidth, 0.0f))) {
        HWND window = GetActiveWindow();
        if (!window)
            window = GetForegroundWindow();
        lastCopySucceeded = copyVisibleAnalysisPaneToClipboard(window);
        feedbackUntil = now + 2.0;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Copy the currently visible Analysis pane as an image.");
    ImGui::SetCursorScreenPos(savedCursor);
}

} // namespace smp
