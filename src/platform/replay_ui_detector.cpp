#include "platform/replay_ui_detector.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace smp {
ScreenRect replayUiProbeRect(const ScreenRect& gameArea) noexcept {
    if (!gameArea.valid()) return {};
    return {gameArea.left, gameArea.top + gameArea.height() * 65 / 100,
            gameArea.right, gameArea.bottom};
}

namespace {
struct Feature {
    int left, top, right, bottom, count{}, core{};
    int width() const { return right - left + 1; }
    int height() const { return bottom - top + 1; }
    double x() const { return (left + right) * 0.5; }
    double y() const { return (top + bottom) * 0.5; }
};

unsigned char color(const BgraImageView& image, int x, int y) {
    const auto i = static_cast<std::size_t>(y) * image.stride + static_cast<std::size_t>(x) * 4;
    const int b = image.pixels[i], g = image.pixels[i + 1], r = image.pixels[i + 2];
    if (r >= 90 && g >= 75 && std::min(r, g) > b * 1.5 && std::abs(r - g) < std::max(r, g) * 0.6)
        return 1;
    return 0;
}
}

bool containsReplayTransportPanel(const BgraImageView& image) noexcept {
    if (!image.valid()) return false;
    try {
        // One byte per probe pixel and a reusable flood-fill queue. No work at idle.
        std::vector<unsigned char> mask(static_cast<std::size_t>(image.width) * image.height);
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x)
                mask[static_cast<std::size_t>(y) * image.width + x] = color(image, x, y);
        std::vector<int> queue;
        std::vector<Feature> rings;
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                const int seed = y * image.width + x;
                const auto kind = mask[seed];
                if (!kind) continue;
                queue.clear(); queue.push_back(seed); mask[seed] = 0;
                Feature f{x, y, x, y};
                for (std::size_t head = 0; head < queue.size(); ++head) {
                    const int px = queue[head] % image.width, py = queue[head] / image.width;
                    f.left = std::min(f.left, px); f.right = std::max(f.right, px);
                    f.top = std::min(f.top, py); f.bottom = std::max(f.bottom, py);
                    // Eight-connected to retain antialiased diagonal ring edges.
                    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = px + dx, ny = py + dy;
                        if (nx < 0 || ny < 0 || nx >= image.width || ny >= image.height) continue;
                        const int index = ny * image.width + nx;
                        if (mask[index] == kind) { mask[index] = 0; queue.push_back(index); }
                    }
                }
                f.count = static_cast<int>(queue.size());
                const double w = f.width(), h = f.height();
                if (kind != 1 || w < std::max(5.0, image.height * 0.035) || w > image.height * 0.40 ||
                    w / h < 0.75 || w / h > 1.33 || f.count < w * h * 0.16 || f.count > w * h * 0.70)
                    continue;
                int corners = 0;
                for (const int index : queue) {
                    const double rx = std::abs(index % image.width - f.x()) / w;
                    const double ry = std::abs(index / image.width - f.y()) / h;
                    if (rx < 0.22 && ry < 0.22) ++f.core;
                    if (rx > 0.34 && ry > 0.34) ++corners;
                }
                // Reject filled icons and square outlines; allow transport glyphs inside rings.
                if (f.core <= w * h * 0.12 && corners <= f.count * 0.10) rings.push_back(f);
            }
        }
        std::sort(rings.begin(), rings.end(), [](const auto& a, const auto& b) { return a.x() < b.x(); });
        for (std::size_t a = 0; a < rings.size(); ++a)
            for (std::size_t b = a + 1; b < rings.size(); ++b)
                for (std::size_t c = b + 1; c < rings.size(); ++c) {
                    const auto& first = rings[a]; const auto& middle = rings[b]; const auto& last = rings[c];
                    const double size = first.width();
                    const double gap1 = middle.x() - first.x(), gap2 = last.x() - middle.x();
                    // The probe spans the whole lower HUD. Restrict positive evidence
                    // to the right-hand replay controls, away from minimap/portrait UI.
                    if ((first.x() + middle.x() + last.x()) / 3 < image.width * 0.55 ||
                        middle.width() < size * 0.8 || middle.width() > size * 1.25 ||
                        last.width() < size * 0.8 || last.width() > size * 1.25 ||
                        middle.height() < first.height() * 0.8 || middle.height() > first.height() * 1.25 ||
                        last.height() < first.height() * 0.8 || last.height() > first.height() * 1.25 ||
                        std::abs(first.y() - middle.y()) > size * 0.2 || std::abs(first.y() - last.y()) > size * 0.2 ||
                        gap1 < size * 1.2 || gap1 > size * 2.2 ||
                        gap2 < size * 1.2 || gap2 > size * 2.2 || std::abs(gap1 - gap2) > gap1 * 0.25)
                        continue;
                    // Strong rings and their relationships suffice; the replay
                    // progress indicator need not be visible or detected.
                    return true;
                }
    } catch (...) {
        // Allocation failure is unavailable evidence, never a reason to block live play.
    }
    return false;
}
} // namespace smp
