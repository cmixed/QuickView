#pragma once
// ============================================================================
// TileGeometry.h - High-Performance Tile & Halo Geometry Math for Super-Resolution
// ============================================================================
// Features:
// 1. Data-Oriented Design (DOD) with zero heap allocations on hot calculation paths.
// 2. Viewport-Aware Culling: Generates only tiles intersecting the current visible view.
// 3. Symmetric & Asymmetric Boundary Halo Padding calculation to eliminate CNN seams.
// 4. C++23 constexpr pure functions for compile-time or runtime calculation.
// ============================================================================

#include <cstdint>
#include <algorithm>
#include <vector>

namespace QuickView::Sr2 {

struct ViewportRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    constexpr bool IsEmpty() const noexcept {
        return right <= left || bottom <= top;
    }
};

struct TileCoordinate {
    uint32_t tileIndexX = 0;
    uint32_t tileIndexY = 0;

    // Source region in original image (Excluding Halo)
    uint32_t srcX = 0;
    uint32_t srcY = 0;
    uint32_t srcW = 0;
    uint32_t srcH = 0;

    // Actual Tensor input crop from source image (Including clamped Halo)
    uint32_t inX = 0;
    uint32_t inY = 0;
    uint32_t inW = 0;
    uint32_t inH = 0;

    // Active Halo dimensions on each edge (in input pixel space)
    uint32_t haloLeft = 0;
    uint32_t haloTop = 0;
    uint32_t haloRight = 0;
    uint32_t haloBottom = 0;

    // Destination placement coordinates in final upscaled texture (scale applied)
    uint32_t dstX = 0;
    uint32_t dstY = 0;
    uint32_t dstW = 0;
    uint32_t dstH = 0;
};

class TileGeometry {
public:
    static constexpr uint32_t ComputeTileCount(uint32_t dimension, uint32_t tileSize) noexcept {
        if (dimension == 0 || tileSize == 0) return 0;
        return (dimension + tileSize - 1) / tileSize;
    }

    static constexpr TileCoordinate ComputeTile(
        uint32_t tileX,
        uint32_t tileY,
        uint32_t imgW,
        uint32_t imgH,
        uint32_t tileSize = 512,
        uint32_t halo = 16,
        float scale = 4.0f
    ) noexcept {
        TileCoordinate tile{};
        tile.tileIndexX = tileX;
        tile.tileIndexY = tileY;

        // 1. Calculate Source non-halo region
        tile.srcX = tileX * tileSize;
        tile.srcY = tileY * tileSize;
        tile.srcW = (tile.srcX + tileSize <= imgW) ? tileSize : (imgW > tile.srcX ? imgW - tile.srcX : 0);
        tile.srcH = (tile.srcY + tileSize <= imgH) ? tileSize : (imgH > tile.srcY ? imgH - tile.srcY : 0);

        if (tile.srcW == 0 || tile.srcH == 0) {
            return tile;
        }

        // 2. Calculate Halo bounds clamped to image borders
        uint32_t leftExtend = (tile.srcX >= halo) ? halo : tile.srcX;
        uint32_t topExtend = (tile.srcY >= halo) ? halo : tile.srcY;
        uint32_t rightExtend = (tile.srcX + tile.srcW + halo <= imgW) ? halo : (imgW - (tile.srcX + tile.srcW));
        uint32_t bottomExtend = (tile.srcY + tile.srcH + halo <= imgH) ? halo : (imgH - (tile.srcY + tile.srcH));

        tile.haloLeft = leftExtend;
        tile.haloTop = topExtend;
        tile.haloRight = rightExtend;
        tile.haloBottom = bottomExtend;

        tile.inX = tile.srcX - leftExtend;
        tile.inY = tile.srcY - topExtend;
        tile.inW = tile.srcW + leftExtend + rightExtend;
        tile.inH = tile.srcH + topExtend + bottomExtend;

        // 3. Destination placement in upscaled surface
        tile.dstX = static_cast<uint32_t>(tile.srcX * scale);
        tile.dstY = static_cast<uint32_t>(tile.srcY * scale);
        tile.dstW = static_cast<uint32_t>(tile.srcW * scale);
        tile.dstH = static_cast<uint32_t>(tile.srcH * scale);

        return tile;
    }

    static std::vector<TileCoordinate> ComputeVisibleTiles(
        uint32_t imgW,
        uint32_t imgH,
        const ViewportRect& visibleSrcRect,
        uint32_t tileSize = 512,
        uint32_t halo = 16,
        float scale = 4.0f
    ) {
        std::vector<TileCoordinate> tiles;
        if (imgW == 0 || imgH == 0 || tileSize == 0) return tiles;

        uint32_t countX = ComputeTileCount(imgW, tileSize);
        uint32_t countY = ComputeTileCount(imgH, tileSize);

        // Clamp visible rectangle to image dimensions
        float vLeft = std::clamp(visibleSrcRect.left, 0.0f, static_cast<float>(imgW));
        float vTop = std::clamp(visibleSrcRect.top, 0.0f, static_cast<float>(imgH));
        float vRight = std::clamp(visibleSrcRect.right, 0.0f, static_cast<float>(imgW));
        float vBottom = std::clamp(visibleSrcRect.bottom, 0.0f, static_cast<float>(imgH));

        uint32_t minTileX = (visibleSrcRect.IsEmpty()) ? 0 : static_cast<uint32_t>(vLeft) / tileSize;
        uint32_t minTileY = (visibleSrcRect.IsEmpty()) ? 0 : static_cast<uint32_t>(vTop) / tileSize;
        uint32_t maxTileX = (visibleSrcRect.IsEmpty()) ? countX : (std::min)(countX, ComputeTileCount(static_cast<uint32_t>(vRight), tileSize));
        uint32_t maxTileY = (visibleSrcRect.IsEmpty()) ? countY : (std::min)(countY, ComputeTileCount(static_cast<uint32_t>(vBottom), tileSize));

        if (maxTileX < minTileX) maxTileX = minTileX;
        if (maxTileY < minTileY) maxTileY = minTileY;

        tiles.reserve(static_cast<size_t>(maxTileX - minTileX) * (maxTileY - minTileY));

        for (uint32_t ty = minTileY; ty < maxTileY; ++ty) {
            for (uint32_t tx = minTileX; tx < maxTileX; ++tx) {
                tiles.push_back(ComputeTile(tx, ty, imgW, imgH, tileSize, halo, scale));
            }
        }

        return tiles;
    }
};

} // namespace QuickView::Sr2
