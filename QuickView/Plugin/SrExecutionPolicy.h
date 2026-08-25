#pragma once

#include <algorithm>
#include <cstdint>

namespace QuickView::Sr2 {

// Policy only: backends (NCNN fallback or the future DirectML backend) receive
// the same work shape.  This deliberately separates UX/cache decisions from
// a particular neural-runtime implementation.
enum class WorkMode : uint8_t {
    FullFrame,
    ViewportTiles,
};

struct WorkPlan {
    WorkMode mode = WorkMode::ViewportTiles;
    uint32_t sourceTileSize = 512;
    uint32_t halo = 16;
    uint64_t outputBytes = 0;
};

inline WorkPlan SelectWorkPlan(uint32_t width, uint32_t height, float scale) noexcept {
    constexpr uint64_t kFullFrameInputPixels = 4ull * 1024 * 1024;
    constexpr uint64_t kFullFrameOutputBudget = 512ull * 1024 * 1024;
    constexpr uint32_t kDefaultTile = 512;
    constexpr uint32_t kHalo = 16;

    if (width == 0 || height == 0 || scale < 1.0f) return {};
    const uint64_t inputPixels = static_cast<uint64_t>(width) * height;
    const uint64_t scaledWidth = static_cast<uint64_t>(width * scale);
    const uint64_t scaledHeight = static_cast<uint64_t>(height * scale);
    const uint64_t outputBytes = scaledWidth * scaledHeight * 4ull;

    WorkPlan plan;
    plan.outputBytes = outputBytes;
    plan.halo = kHalo;
    plan.sourceTileSize = kDefaultTile;
    plan.mode = (inputPixels <= kFullFrameInputPixels && outputBytes <= kFullFrameOutputBudget)
                    ? WorkMode::FullFrame
                    : WorkMode::ViewportTiles;
    return plan;
}

} // namespace QuickView::Sr2
