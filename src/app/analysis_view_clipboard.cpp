#include "app/analysis_view.h"
#include "app/full_content_capture.h"
#include "app/session_summary_export.h"
#include "app/session_trends.h"
#include "cli/session_summary_paths.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <d3d11.h>
#include <filesystem>
#include <limits>
#include <optional>
#include <utility>
#include <vector>
#include <windows.h>

namespace smp {
namespace {

constexpr std::size_t maximumCaptureBytes = 256U * 1024U * 1024U;
constexpr float captureDeltaSeconds = 1.0f / 60.0f;

struct PendingFullContentCapture {
    int width{};
    FullContentRenderer renderer;
};

std::optional<PendingFullContentCapture> pendingCapture;
std::optional<bool> completedCapture;

template <typename T> void release(T*& resource) noexcept {
    if (resource) {
        resource->Release();
        resource = nullptr;
    }
}

int maximumTextureDimension(ID3D11Device* device) noexcept {
    if (!device)
        return 0;
    return device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? 16384 : 8192;
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

std::filesystem::path sessionSummariesDirectory() {
    static const std::filesystem::path summaries = [] {
        const auto sessions = sessionsDirectory();
        migrateLegacyAutomaticSessionSummaries(sessions);
        return sessionSummariesRootFromSessions(sessions);
    }();
    return summaries;
}

struct CaptureFrameResult {
    float contentHeight{};
    float scrollY{};
};

CaptureFrameResult drawCaptureFrame(
    int width,
    int height,
    int scrollY,
    std::optional<int> measuredContentHeight,
    const FullContentRenderer& renderer) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize =
        ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DeltaTime = captureDeltaSeconds;
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    if (measuredContentHeight) {
        ImGui::SetNextWindowContentSize(
            ImVec2(0.0f, static_cast<float>(*measuredContentHeight)));
    }
    ImGui::SetNextWindowScroll(ImVec2(0.0f, static_cast<float>(scrollY)));
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("Full Analysis Capture##Offscreen", nullptr, flags);
    const float actualScrollY = ImGui::GetScrollY();
    renderer();
    const float contentHeight =
        ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::Render();
    return {contentHeight, actualScrollY};
}

bool renderTile(ID3D11Device* device,
                ID3D11DeviceContext* context,
                ImDrawData* drawData,
                int width,
                const FullContentCaptureTile& tile,
                std::vector<std::uint8_t>& pixels) noexcept {
    ID3D11Texture2D* renderTexture = nullptr;
    ID3D11RenderTargetView* renderTarget = nullptr;
    ID3D11Texture2D* stagingTexture = nullptr;

    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(width);
    description.Height = static_cast<UINT>(tile.height);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;

    bool success = false;
    if (SUCCEEDED(device->CreateTexture2D(&description, nullptr,
                                          &renderTexture)) &&
        SUCCEEDED(device->CreateRenderTargetView(renderTexture, nullptr,
                                                 &renderTarget))) {
        auto stagingDescription = description;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(device->CreateTexture2D(&stagingDescription, nullptr,
                                              &stagingTexture))) {
            constexpr float clearColor[4]{0.075f, 0.086f, 0.105f, 1.0f};
            context->OMSetRenderTargets(1, &renderTarget, nullptr);
            context->ClearRenderTargetView(renderTarget, clearColor);
            ImGui_ImplDX11_RenderDrawData(drawData);
            context->CopyResource(stagingTexture, renderTexture);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(stagingTexture, 0, D3D11_MAP_READ, 0,
                                       &mapped))) {
                const std::size_t rowBytes =
                    static_cast<std::size_t>(width) * 4U;
                for (int row = 0; row < tile.height; ++row) {
                    const auto* source =
                        static_cast<const std::uint8_t*>(mapped.pData) +
                        static_cast<std::size_t>(row) * mapped.RowPitch;
                    auto* destination =
                        pixels.data() +
                        static_cast<std::size_t>(tile.offsetY + row) * rowBytes;
                    std::memcpy(destination, source, rowBytes);
                }
                context->Unmap(stagingTexture, 0);
                success = true;
            }
        }
    }

    release(stagingTexture);
    release(renderTarget);
    release(renderTexture);
    return success;
}

bool copyPixelsToClipboard(HWND owner,
                           int width,
                           int height,
                           const std::vector<std::uint8_t>& rgba) noexcept {
    const auto pixelBytes = fullContentCaptureByteSize(width, height);
    if (!owner || !pixelBytes || rgba.size() != *pixelBytes ||
        *pixelBytes > std::numeric_limits<DWORD>::max() ||
        *pixelBytes > std::numeric_limits<std::size_t>::max() -
                          sizeof(BITMAPINFOHEADER)) {
        return false;
    }

    const std::size_t allocationBytes = sizeof(BITMAPINFOHEADER) + *pixelBytes;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, allocationBytes);
    if (!memory)
        return false;
    auto* data = static_cast<std::uint8_t*>(GlobalLock(memory));
    if (!data) {
        GlobalFree(memory);
        return false;
    }

    auto* header = reinterpret_cast<BITMAPINFOHEADER*>(data);
    *header = {};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = width;
    header->biHeight = -height;
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(*pixelBytes);

    auto* bgra = data + sizeof(BITMAPINFOHEADER);
    for (std::size_t offset = 0; offset < *pixelBytes; offset += 4U) {
        bgra[offset] = rgba[offset + 2U];
        bgra[offset + 1U] = rgba[offset + 1U];
        bgra[offset + 2U] = rgba[offset];
        bgra[offset + 3U] = 0xffU;
    }
    GlobalUnlock(memory);

    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }
    bool success = false;
    if (EmptyClipboard() && SetClipboardData(CF_DIB, memory)) {
        memory = nullptr;
        success = true;
    }
    CloseClipboard();
    if (memory)
        GlobalFree(memory);
    return success;
}

bool captureFullContent(ID3D11Device* device,
                        ID3D11DeviceContext* context,
                        ID3D11RenderTargetView* restoreRenderTarget,
                        HWND owner,
                        PendingFullContentCapture request) {
    if (!device || !context || !restoreRenderTarget || !owner ||
        !request.renderer || request.width <= 0) {
        return false;
    }

    ImGuiContext* primaryGui = ImGui::GetCurrentContext();
    ImPlotContext* primaryPlot = ImPlot::GetCurrentContext();
    if (!primaryGui || !primaryPlot)
        return false;

    ImFontAtlas* sharedFonts = ImGui::GetIO().Fonts;
    ImFont* defaultFont = ImGui::GetIO().FontDefault;
    const ImGuiBackendFlags backendFlags = ImGui::GetIO().BackendFlags;
    const ImGuiStyle primaryGuiStyle = ImGui::GetStyle();
    const ImPlotStyle primaryPlotStyle = ImPlot::GetStyle();
    ImGuiContext* captureGui = ImGui::CreateContext(sharedFonts);
    if (!captureGui)
        return false;
    ImPlotContext* capturePlot = ImPlot::CreateContext();
    if (!capturePlot) {
        ImGui::DestroyContext(captureGui);
        ImGui::SetCurrentContext(primaryGui);
        ImPlot::SetCurrentContext(primaryPlot);
        return false;
    }

    bool success = false;
    try {
        ImGui::GetStyle() = primaryGuiStyle;
        ImPlot::GetStyle() = primaryPlotStyle;
        ImGui::GetIO().FontDefault = defaultFont;
        ImGui::GetIO().BackendFlags = backendFlags;
        ImGui::GetIO().DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().LogFilename = nullptr;

        const int textureLimit = maximumTextureDimension(device);
        if (request.width <= textureLimit) {
            const auto measuredFrame = drawCaptureFrame(
                request.width, textureLimit, 0, std::nullopt,
                request.renderer);
            const float measured = measuredFrame.contentHeight;
            const double roundedHeight =
                std::ceil(static_cast<double>(measured));
            if (std::isfinite(measured) && measured > 0.0f &&
                roundedHeight <=
                    static_cast<double>(std::numeric_limits<int>::max())) {
                const int contentHeight =
                    std::max(1, static_cast<int>(roundedHeight));
                const auto byteSize =
                    fullContentCaptureByteSize(request.width, contentHeight);
                const auto tiles = fullContentCaptureTilePlanForTextureLimit(
                    contentHeight, textureLimit);
                if (byteSize && *byteSize <= maximumCaptureBytes &&
                    !tiles.empty()) {
                    std::vector<std::uint8_t> pixels(*byteSize);
                    success = true;
                    for (const auto& tile : tiles) {
                        const auto frame = drawCaptureFrame(
                            request.width, tile.height, tile.offsetY,
                            contentHeight, request.renderer);
                        if (frame.scrollY != static_cast<float>(tile.offsetY)) {
                            success = false;
                            break;
                        }
                        ImDrawData* drawData = ImGui::GetDrawData();
                        ImGui::SetCurrentContext(primaryGui);
                        ImPlot::SetCurrentContext(primaryPlot);
                        if (!renderTile(device, context, drawData,
                                        request.width, tile, pixels)) {
                            success = false;
                        }
                        ImGui::SetCurrentContext(captureGui);
                        ImPlot::SetCurrentContext(capturePlot);
                        if (!success)
                            break;
                    }
                    if (success) {
                        success = copyPixelsToClipboard(
                            owner, request.width, contentHeight, pixels);
                    }
                }
            }
        }
    } catch (...) {
        success = false;
    }

    ImGui::SetCurrentContext(captureGui);
    ImPlot::SetCurrentContext(capturePlot);
    ImPlot::DestroyContext(capturePlot);
    ImGui::DestroyContext(captureGui);
    ImGui::SetCurrentContext(primaryGui);
    ImPlot::SetCurrentContext(primaryPlot);
    context->OMSetRenderTargets(1, &restoreRenderTarget, nullptr);
    return success;
}

} // namespace

bool queueFullContentCapture(int width, FullContentRenderer renderer) {
    if (pendingCapture || !validCaptureDimensions(width, 1) || !renderer)
        return false;
    pendingCapture = PendingFullContentCapture{width, std::move(renderer)};
    return true;
}

std::optional<bool> takeFullContentCaptureResult() noexcept {
    auto result = completedCapture;
    completedCapture.reset();
    return result;
}

void executePendingFullContentCapture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11RenderTargetView* restoreRenderTarget,
    HWND clipboardOwner) noexcept {
    if (!pendingCapture)
        return;
    auto request = std::move(*pendingCapture);
    pendingCapture.reset();
    completedCapture = captureFullContent(
        device, context, restoreRenderTarget, clipboardOwner, std::move(request));
}

void drawAnalysisViewWithClipboard(const GameAnalysisVisualizationModel& model,
                                   AnalysisViewState& state,
                                   const ReportGroupVisibility& visibility) {
    static int view = 0;
    static double feedbackUntil{};
    static bool lastCopySucceeded{};

    const double now = ImGui::GetTime();
    if (const auto result = takeFullContentCaptureResult()) {
        lastCopySucceeded = *result;
        feedbackUntil = now + 2.0;
    }

    const int captureWidth = std::max(
        1, static_cast<int>(std::floor(
               ImGui::GetContentRegionAvail().x +
               ImGui::GetStyle().WindowPadding.x * 2.0f)));
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
    ImGui::SameLine();
    if (ImGui::Button("Copy Analysis")) {
        const ReportGroupVisibility captureVisibility = visibility;
        bool queued = false;
        if (view == 0) {
            AnalysisViewState captureState = state;
            queued = queueFullContentCapture(
                captureWidth,
                [&model, captureState, captureVisibility]() mutable {
                    drawAnalysisView(model, captureState, captureVisibility);
                });
        } else {
            const auto summaries = sessionSummariesDirectory();
            queued = queueFullContentCapture(
                captureWidth, [summaries, captureVisibility] {
                    drawSessionTrends(summaries, captureVisibility,
                                      SessionTrendsPresentation::Capture);
                });
        }
        if (!queued) {
            lastCopySucceeded = false;
            feedbackUntil = now + 2.0;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Copy the complete currently selected Analysis view as one image.");
    }
    if (now < feedbackUntil) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", lastCopySucceeded ? "Copied to clipboard"
                                                    : "Copy failed");
    }
    ImGui::Spacing();

    ImGui::BeginChild(view == 0 ? "##LatestGameAnalysisScroll"
                                : "##SessionTrendsAnalysisScroll",
                      ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_NoSavedSettings);
    if (view == 0) {
        drawAnalysisView(model, state, visibility);
    } else {
        const auto summaries = sessionSummariesDirectory();
        drawSessionSummaryExport(summaries);
        ImGui::Spacing();
        drawSessionTrends(summaries, visibility);
    }
    ImGui::EndChild();
}

} // namespace smp
