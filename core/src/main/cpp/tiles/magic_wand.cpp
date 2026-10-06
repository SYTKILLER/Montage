#include "tiles/magic_wand.h"

#include <algorithm>
#include <cstring>
#include <queue>
#include <vector>

namespace montage {
namespace tiles {
namespace {

// 图层瓦片 → 连续 RGBA 平面（魔棒随机访问友好；v1 简化，大画布可改分块访注册记）
std::vector<uint8_t> flatten(const TileGrid& grid, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4u, 0);
    for (const auto& [key, tile] : grid.tiles) {
        const uint32_t tx = key % grid.cols;
        const uint32_t ty = key / grid.cols;
        const int32_t x0 = static_cast<int32_t>(tx * kTileSize);
        const int32_t y0 = static_cast<int32_t>(ty * kTileSize);
        const int32_t copyW = std::min<int32_t>(kTileSize, static_cast<int32_t>(w) - x0);
        const int32_t copyH = std::min<int32_t>(kTileSize, static_cast<int32_t>(h) - y0);
        if (copyW <= 0 || copyH <= 0) {
            continue;
        }
        const uint8_t* src = tile->pixels->mapCpu();
        const uint32_t stride = tile->pixels->rowBytes();
        for (int32_t r = 0; r < copyH; ++r) {
            std::memcpy(out.data() + (static_cast<size_t>(y0 + r) * w + x0) * 4u,
                        src + static_cast<size_t>(r) * stride,
                        static_cast<size_t>(copyW) * 4u);
        }
    }
    return out;
}

inline bool withinTolerance(const uint8_t* a, const uint8_t* b, int tol) {
    const int dr = std::abs(static_cast<int>(a[0]) - static_cast<int>(b[0]));
    const int dg = std::abs(static_cast<int>(a[1]) - static_cast<int>(b[1]));
    const int db = std::abs(static_cast<int>(a[2]) - static_cast<int>(b[2]));
    return dr <= tol && dg <= tol && db <= tol;
}

}  // namespace

std::shared_ptr<const TileGrid> wandMask(const TileGrid& pixels, uint32_t docW, uint32_t docH,
                                         int sampleX, int sampleY, int tolerance,
                                         bool contiguous) {
    if (docW == 0 || docH == 0 || pixels.cols == 0) {
        return nullptr;
    }
    const int w = static_cast<int>(docW);
    const int h = static_cast<int>(docH);
    if (sampleX < 0 || sampleY < 0 || sampleX >= w || sampleY >= h) {
        return nullptr;
    }
    const std::vector<uint8_t> img = flatten(pixels, docW, docH);
    const size_t count = static_cast<size_t>(w) * h;
    std::vector<uint8_t> selected(count, 0);
    const uint8_t* seed = img.data() + (static_cast<size_t>(sampleY) * w + sampleX) * 4u;
    const int tol = std::min(255, std::max(0, tolerance));
    size_t total = 0;
    if (contiguous) {
        std::vector<uint8_t> visited(count, 0);
        std::queue<size_t> q;
        const size_t s0 = static_cast<size_t>(sampleY) * w + sampleX;
        visited[s0] = 1;
        selected[s0] = 255;
        ++total;
        q.push(s0);
        while (!q.empty()) {
            const size_t at = q.front();
            q.pop();
            const int x = static_cast<int>(at % w);
            const int y = static_cast<int>(at / w);
            const int dxs[4] = {1, -1, 0, 0};
            const int dys[4] = {0, 0, 1, -1};
            for (int d = 0; d < 4; ++d) {
                const int nx = x + dxs[d];
                const int ny = y + dys[d];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                    continue;
                }
                const size_t nAt = static_cast<size_t>(ny) * w + nx;
                if (visited[nAt]) {
                    continue;
                }
                visited[nAt] = 1;
                if (withinTolerance(img.data() + nAt * 4u, seed, tol)) {
                    selected[nAt] = 255;
                    ++total;
                    q.push(nAt);
                }
            }
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (withinTolerance(img.data() + i * 4u, seed, tol)) {
                selected[i] = 255;
                ++total;
            }
        }
    }
    if (total == 0) {
        return nullptr;
    }
    // 掩码 → 瓦片网格（255 入选）
    TileGridBuilder builder((docW + kTileSize - 1u) / kTileSize, (docH + kTileSize - 1u) / kTileSize);
    for (uint32_t ty = 0; ty < builder.rows(); ++ty) {
        for (uint32_t tx = 0; tx < builder.cols(); ++tx) {
            EngineBuffer& buf = builder.ensureTile(tx, ty);
            uint8_t* px = buf.mapCpuWrite();
            const uint32_t y0 = ty * kTileSize;
            const uint32_t x0 = tx * kTileSize;
            for (uint32_t r = 0; r < kTileSize; ++r) {
                const uint32_t gy = y0 + r;
                if (gy >= docH) {
                    break;
                }
                uint8_t* row = px + static_cast<size_t>(r) * kTileSize * 4u;
                for (uint32_t c = 0; c < kTileSize; ++c) {
                    const uint32_t gx = x0 + c;
                    if (gx >= docW) {
                        break;
                    }
                    const uint8_t v = selected[static_cast<size_t>(gy) * docW + gx];
                    row[c * 4u] = v;
                    row[c * 4u + 1u] = v;
                    row[c * 4u + 2u] = v;
                    row[c * 4u + 3u] = 255;
                }
            }
        }
    }
    return builder.publish(1);
}

}  // namespace tiles
}  // namespace montage
