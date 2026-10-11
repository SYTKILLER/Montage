#include "tiles/shape_mask.h"

#include <algorithm>
#include <cmath>

namespace montage {
namespace tiles {
namespace {

constexpr int kTile = static_cast<int>(kTileSize);
// 2×2 覆盖采样偏移（与 selection_ops 同族）
constexpr double kSubOff[4][2] = {{0.25, 0.25}, {0.75, 0.25}, {0.25, 0.75}, {0.75, 0.75}};

uint8_t coverageByte(int hits) {
    switch (hits) {
        case 1:
            return 77;   // 0.25
        case 2:
            return 128;
        case 3:
            return 179;  // 0.75
        default:
            return 255;
    }
}

// 灰度写入（0 不落瓦片，保持稀疏；与 selection_ops::setMaskGray 同构）
void writeGray(TileGridBuilder& builder, uint32_t docX, uint32_t docY, uint8_t gray) {
    if (gray == 0) {
        return;
    }
    const uint32_t tx = docX / kTileSize;
    const uint32_t ty = docY / kTileSize;
    uint8_t* p = builder.ensureTile(tx, ty).mapCpuWrite();
    const size_t off = (static_cast<size_t>(docY % kTileSize) * kTileSize +
                        static_cast<size_t>(docX % kTileSize)) * 4u;
    p[off] = p[off + 1] = p[off + 2] = gray;
    p[off + 3] = 255;
}

// 采样点 (sx,sy) 的圆角矩形 SDF（≤0 内部）；r=0 退化为普通矩形
double roundedRectSdf(double sx, double sy, double rx0, double ry0, double rx1, double ry1,
                      double r) {
    const double cx = (rx0 + rx1) / 2.0;
    const double cy = (ry0 + ry1) / 2.0;
    const double hx = (rx1 - rx0) / 2.0 - r;
    const double hy = (ry1 - ry0) / 2.0 - r;
    const double qx = std::abs(sx - cx) - hx;
    const double qy = std::abs(sy - cy) - hy;
    if (r <= 0.0) {
        return std::max(qx, qy);  // 矩形 SDF
    }
    const double ax = std::max(qx, 0.0);
    const double ay = std::max(qy, 0.0);
    return std::sqrt(ax * ax + ay * ay) + std::min(std::max(qx, qy), 0.0) - r;
}

}  // namespace

std::shared_ptr<const TileGrid> roundedRectMask(double x0, double y0, double x1, double y1,
                                                double radius, uint32_t docW, uint32_t docH) {
    if (docW == 0 || docH == 0) {
        return nullptr;
    }
    const double rx0 = std::min(x0, x1);
    const double ry0 = std::min(y0, y1);
    const double rx1 = std::max(x0, x1);
    const double ry1 = std::max(y0, y1);
    if (rx1 - rx0 < 1.0 || ry1 - ry0 < 1.0) {
        return nullptr;
    }
    const double halfMin = std::min(rx1 - rx0, ry1 - ry0) / 2.0;
    const double r = std::min(std::max(0.0, radius), halfMin);
    // 包围盒外扩 1px 捕捉 AA 边缘
    const int ix0 = std::max(0, static_cast<int>(std::floor(rx0)) - 1);
    const int iy0 = std::max(0, static_cast<int>(std::floor(ry0)) - 1);
    const int ix1 = std::min(static_cast<int>(docW) - 1, static_cast<int>(std::ceil(rx1)) + 1);
    const int iy1 = std::min(static_cast<int>(docH) - 1, static_cast<int>(std::ceil(ry1)) + 1);
    TileGridBuilder builder((docW + kTileSize - 1u) / kTileSize,
                            (docH + kTileSize - 1u) / kTileSize);
    bool any = false;
    for (int py = iy0; py <= iy1; ++py) {
        for (int px = ix0; px <= ix1; ++px) {
            int hits = 0;
            for (int q = 0; q < 4; ++q) {
                if (roundedRectSdf(static_cast<double>(px) + kSubOff[q][0],
                                   static_cast<double>(py) + kSubOff[q][1],
                                   rx0, ry0, rx1, ry1, r) <= 0.0) {
                    ++hits;
                }
            }
            if (hits > 0) {
                writeGray(builder, static_cast<uint32_t>(px), static_cast<uint32_t>(py),
                          coverageByte(hits));
                any = true;
            }
        }
    }
    return any ? builder.publish(1) : nullptr;
}

std::shared_ptr<const TileGrid> capsuleMask(double x0, double y0, double x1, double y1,
                                            double width, uint32_t docW, uint32_t docH) {
    if (docW == 0 || docH == 0 || width < 1.0) {
        return nullptr;
    }
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double len2 = dx * dx + dy * dy;
    if (len2 < 1e-9) {
        return nullptr;
    }
    const double halfW = width / 2.0;
    const int ix0 = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - halfW)) - 1);
    const int iy0 = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - halfW)) - 1);
    const int ix1 = std::min(static_cast<int>(docW) - 1,
                             static_cast<int>(std::ceil(std::max(x0, x1) + halfW)) + 1);
    const int iy1 = std::min(static_cast<int>(docH) - 1,
                             static_cast<int>(std::ceil(std::max(y0, y1) + halfW)) + 1);
    TileGridBuilder builder((docW + kTileSize - 1u) / kTileSize,
                            (docH + kTileSize - 1u) / kTileSize);
    bool any = false;
    for (int py = iy0; py <= iy1; ++py) {
        for (int px = ix0; px <= ix1; ++px) {
            int hits = 0;
            for (int q = 0; q < 4; ++q) {
                const double sx = static_cast<double>(px) + kSubOff[q][0];
                const double sy = static_cast<double>(py) + kSubOff[q][1];
                double t = ((sx - x0) * dx + (sy - y0) * dy) / len2;
                t = std::min(1.0, std::max(0.0, t));
                const double cx = x0 + t * dx;
                const double cy = y0 + t * dy;
                if (std::hypot(sx - cx, sy - cy) <= halfW) {
                    ++hits;
                }
            }
            if (hits > 0) {
                writeGray(builder, static_cast<uint32_t>(px), static_cast<uint32_t>(py),
                          coverageByte(hits));
                any = true;
            }
        }
    }
    return any ? builder.publish(1) : nullptr;
}

}  // namespace tiles
}  // namespace montage
