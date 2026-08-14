#include "platform/screen_region_capture.h"

#include <stdexcept>
#include <windows.h>

namespace smp {

struct ScreenRegionCapture::Impl {
    ~Impl() {
        release();
    }

    void ensureBuffer(int requestedWidth, int requestedHeight) {
        if (memory && requestedWidth == width && requestedHeight == height)
            return;
        release();

        memory = CreateCompatibleDC(nullptr);
        if (!memory)
            throw std::runtime_error("Unable to create the minimap capture device context");

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = requestedWidth;
        info.bmiHeader.biHeight = -requestedHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap || !bits) {
            release();
            throw std::runtime_error("Unable to create the minimap capture pixel buffer");
        }
        previousBitmap = SelectObject(memory, bitmap);
        if (!previousBitmap || previousBitmap == HGDI_ERROR) {
            previousBitmap = nullptr;
            release();
            throw std::runtime_error("Unable to initialize the minimap capture pixel buffer");
        }
        width = requestedWidth;
        height = requestedHeight;
        stride = width * 4;
    }

    BgraImageView capture(const ScreenRect& rectangle) {
        if (!rectangle.valid())
            throw std::runtime_error("The minimap capture rectangle is invalid");
        ensureBuffer(rectangle.width(), rectangle.height());
        const HDC screen = GetDC(nullptr);
        if (!screen)
            throw std::runtime_error("Unable to access the Windows desktop for minimap capture");
        const BOOL copied = BitBlt(memory, 0, 0, width, height, screen, rectangle.left, rectangle.top,
                                   SRCCOPY | CAPTUREBLT);
        ReleaseDC(nullptr, screen);
        if (!copied)
            throw std::runtime_error("Windows failed to capture the minimap rectangle");
        const auto* pixels = static_cast<const std::uint8_t*>(bits);
        return {width, height, stride,
                std::span<const std::uint8_t>(pixels, static_cast<std::size_t>(stride) * height)};
    }

    void release() noexcept {
        if (memory && previousBitmap)
            SelectObject(memory, previousBitmap);
        previousBitmap = nullptr;
        if (bitmap)
            DeleteObject(bitmap);
        bitmap = nullptr;
        bits = nullptr;
        if (memory)
            DeleteDC(memory);
        memory = nullptr;
        width = 0;
        height = 0;
        stride = 0;
    }

    HDC memory{};
    HBITMAP bitmap{};
    HGDIOBJ previousBitmap{};
    void* bits{};
    int width{};
    int height{};
    int stride{};
};

ScreenRegionCapture::ScreenRegionCapture() : impl_(std::make_unique<Impl>()) {}
ScreenRegionCapture::~ScreenRegionCapture() = default;

BgraImageView ScreenRegionCapture::capture(const ScreenRect& rectangle) {
    return impl_->capture(rectangle);
}

} // namespace smp
