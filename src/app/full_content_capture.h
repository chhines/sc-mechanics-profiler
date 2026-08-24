#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct HWND__;

namespace smp {

struct FullContentCaptureTile {
    int offsetY{};
    int height{};

    bool operator==(const FullContentCaptureTile&) const noexcept = default;
};

[[nodiscard]] inline bool validCaptureDimensions(int width,
                                                 int height) noexcept {
    return width > 0 && height > 0;
}

[[nodiscard]] inline std::optional<std::size_t>
fullContentCaptureByteSize(int width,
                           int height,
                           int bytesPerPixel = 4) noexcept {
    if (!validCaptureDimensions(width, height) || bytesPerPixel <= 0)
        return std::nullopt;
    const auto widthValue = static_cast<std::size_t>(width);
    const auto pixelSize = static_cast<std::size_t>(bytesPerPixel);
    if (widthValue > std::numeric_limits<std::size_t>::max() / pixelSize)
        return std::nullopt;
    const auto rowBytes = widthValue * pixelSize;
    if (static_cast<std::size_t>(height) >
        std::numeric_limits<std::size_t>::max() / rowBytes) {
        return std::nullopt;
    }
    return rowBytes * static_cast<std::size_t>(height);
}

[[nodiscard]] inline std::vector<FullContentCaptureTile>
fullContentCaptureTilePlan(int totalHeight, int maximumTileHeight) {
    std::vector<FullContentCaptureTile> tiles;
    if (totalHeight <= 0 || maximumTileHeight <= 0)
        return tiles;
    for (int offset = 0; offset < totalHeight;) {
        const int height =
            std::min(maximumTileHeight, totalHeight - offset);
        tiles.push_back({offset, height});
        offset += height;
    }
    return tiles;
}

[[nodiscard]] inline std::vector<FullContentCaptureTile>
fullContentCaptureTilePlanForTextureLimit(int contentHeight,
                                          int textureLimit) {
    if (contentHeight <= 0 || textureLimit <= 0)
        return {};
    const int tileHeight = std::min(contentHeight, textureLimit);
    return fullContentCaptureTilePlan(contentHeight, tileHeight);
}

using FullContentRenderer = std::function<void()>;

[[nodiscard]] bool queueFullContentCapture(int width,
                                           FullContentRenderer renderer);
[[nodiscard]] std::optional<bool> takeFullContentCaptureResult() noexcept;
void executePendingFullContentCapture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11RenderTargetView* restoreRenderTarget,
    HWND__* clipboardOwner) noexcept;

} // namespace smp
