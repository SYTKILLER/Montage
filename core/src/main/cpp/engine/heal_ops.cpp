// M19a 修补工具实现。泊松内核见 poisson_ops（photocraft algo/poisson.rs 翻译）。

#include "engine/heal_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "engine/poisson_ops.h"

namespace montage {
namespace {

constexpr uint32_t kTile = 256;  // kTileSize（document.h 同值；局部常量防头依赖扩散）

int gridColsOf(int w) {
    return (w + static_cast<int>(kTile) - 1) / static_cast<int>(kTile);
}

// 网格局部采样（越界 = nullptr）
const uint8_t* sampleAt(const TileGrid* g, int gx, int gy) {
    if (g == nullptr || gx < 0 || gy < 0 ||
        gx >= static_cast<int>(g->cols * kTile) || gy >= static_cast<int>(g->rows * kTile)) {
        return nullptr;
    }
    const uint32_t key = static_cast<uint32_t>(gy / static_cast<int>(kTile)) * g->cols +
                         static_cast<uint32_t>(gx / static_cast<int>(kTile));
    auto it = g->tiles.find(key);
    if (it == g->tiles.end() || it->second == nullptr) {
        return nullptr;
    }
    const uint8_t* base = it->second->pixels->mapCpu();
    if (base == nullptr) {
        return nullptr;
    }
    // 采样生命周期：瓦片缓冲常驻（EngineBuffer），无 unmap 需求（全仓 mapCpu 同约定）
    return base + static_cast<size_t>(gy % static_cast<int>(kTile)) *
                     it->second->pixels->rowBytes() +
           static_cast<size_t>(gx % static_cast<int>(kTile)) * 4u;
}

}  // namespace

bool patchSelection(Document& doc, int dx, int dy, std::string& err) {
    Layer* layer = nullptr;
    for (Layer& l : doc.layers) {
        if (l.id == doc.activeId) {
            layer = &l;
            break;
        }
    }
    if (layer == nullptr || layer->pixels == nullptr) {
        err = "patch: no active layer pixels";
        return false;
    }
    if (doc.selection == nullptr) {
        err = "patch: no selection";
        return false;
    }
    if (dx == 0 && dy == 0) {
        err = "patch: zero offset";
        return false;
    }
    const TileGrid* grid = layer->pixels.get();
    const TileGrid* sel = doc.selection.get();
    const int gw = static_cast<int>(grid->cols * kTile);
    const int gh = static_cast<int>(grid->rows * kTile);
    const int ox = static_cast<int>(std::lround(layer->transform.originX));
    const int oy = static_cast<int>(std::lround(layer->transform.originY));
    // 选区包围盒（文档域灰度>0；读法走 selectionGrayAt——RGBA8888 ×4 寻址，M17a 坑教训）
    int bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = INT32_MIN, by1 = INT32_MIN;
    for (uint32_t ty = 0; ty < sel->rows; ++ty) {
        for (uint32_t tx = 0; tx < sel->cols; ++tx) {
            auto it = sel->tiles.find(ty * sel->cols + tx);
            if (it == sel->tiles.end() || it->second == nullptr) {
                continue;
            }
            const uint8_t* p = it->second->pixels->mapCpu();
            if (p == nullptr) {
                continue;
            }
            for (uint32_t r = 0; r < kTile; ++r) {
                for (uint32_t c = 0; c < kTile; ++c) {
                    if (p[(static_cast<size_t>(r) * kTile + c) * 4u] > 0) {
                        const int x = static_cast<int>(tx * kTile + c);
                        const int y = static_cast<int>(ty * kTile + r);
                        bx0 = std::min(bx0, x);
                        by0 = std::min(by0, y);
                        bx1 = std::max(bx1, x + 1);
                        by1 = std::max(by1, y + 1);
                    }
                }
            }
        }
    }
    if (bx1 <= bx0) {
        err = "patch: empty selection";
        return false;
    }
    // 求解域 = 选区包围盒外扩 1（Dirichlet 边距）∩ 图层网格（文档→网格局部）
    const int rx0 = std::max(0, bx0 - 1 - ox);
    const int ry0 = std::max(0, by0 - 1 - oy);
    const int rx1 = std::min(gw, bx1 + 1 - ox);
    const int ry1 = std::min(gh, by1 + 1 - oy);
    const int w = rx1 - rx0, h = ry1 - ry0;
    if (w <= 0 || h <= 0) {
        err = "patch: selection outside layer";
        return false;
    }
    if (static_cast<double>(w) * static_cast<double>(h) > 64e6) {
        err = "patch: region too large";
        return false;
    }
    std::vector<float> src(static_cast<size_t>(w) * h * 4), dst(static_cast<size_t>(w) * h * 4);
    std::vector<bool> mask(static_cast<size_t>(w) * h, false);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t k = static_cast<size_t>(y) * w + x;
            const int gx = rx0 + x, gy = ry0 + y;
            const uint8_t* d = sampleAt(grid, gx, gy);
            if (d != nullptr) {
                for (int c = 0; c < 4; ++c) {
                    dst[k * 4 + c] = d[c] / 255.0f;
                }
            }
            const uint8_t* s = sampleAt(grid, gx + dx, gy + dy);
            if (s != nullptr) {
                for (int c = 0; c < 4; ++c) {
                    src[k * 4 + c] = s[c] / 255.0f;
                }
            }
            // 源越界/缺瓦片 = 透明（稀疏网格语义；回退 dst 会零化边界失配——E2E 实证）
            const uint8_t* sg = sampleAt(sel, gx + ox, gy + oy);
            mask[k] = sg != nullptr && sg[0] > 0;
        }
    }
    // RGBA 四通道同解（alpha 参与膜解：源透明区修复后透明）
    const std::vector<float> healed =
        poisson::seamlessClone(static_cast<size_t>(w), static_cast<size_t>(h), 4, src, dst, mask);
    // 写回：选区灰度作 src-over 权重（预乘一致式，与 paintTile 同式）
    auto outGrid = std::make_shared<TileGrid>();
    outGrid->cols = grid->cols;
    outGrid->rows = grid->rows;
    outGrid->revision = grid->revision + 1;
    outGrid->tiles = grid->tiles;
    for (int ty = ry0 / static_cast<int>(kTile); ty <= (ry1 - 1) / static_cast<int>(kTile); ++ty) {
        for (int tx = rx0 / static_cast<int>(kTile); tx <= (rx1 - 1) / static_cast<int>(kTile);
             ++tx) {
            const uint32_t key = static_cast<uint32_t>(ty) * grid->cols + static_cast<uint32_t>(tx);
            std::vector<uint8_t> buf(static_cast<size_t>(kTile) * kTile * 4u, 0);
            const uint8_t* orig = nullptr;
            auto it = grid->tiles.find(key);
            if (it != grid->tiles.end() && it->second != nullptr) {
                orig = it->second->pixels->mapCpu();
                if (orig != nullptr) {
                    std::memcpy(buf.data(), orig, buf.size());
                }
            }
            bool changed = false;
            for (uint32_t r = 0; r < kTile; ++r) {
                const int gy = ty * static_cast<int>(kTile) + static_cast<int>(r);
                if (gy < ry0 || gy >= ry1) {
                    continue;
                }
                for (uint32_t c = 0; c < kTile; ++c) {
                    const int gx = tx * static_cast<int>(kTile) + static_cast<int>(c);
                    if (gx < rx0 || gx >= rx1) {
                        continue;
                    }
                    const size_t k = static_cast<size_t>(gy - ry0) * w + static_cast<size_t>(gx - rx0);
                    if (!mask[k]) {
                        continue;
                    }
                    const uint8_t* sg = sampleAt(sel, gx + ox, gy + oy);
                    const float g = sg != nullptr ? static_cast<float>(sg[0]) / 255.0f : 1.0f;
                    if (g <= 0.0f) {
                        continue;
                    }
                    uint8_t* p = buf.data() + (static_cast<size_t>(r) * kTile + c) * 4u;
                    // RGBA 直混（替换式，含 alpha）：out = healed × g + dst × (1 − g)
                    for (int ch = 0; ch < 4; ++ch) {
                        p[static_cast<size_t>(ch)] = static_cast<uint8_t>(std::lround(
                            (healed[k * 4 + static_cast<size_t>(ch)] * g +
                             p[static_cast<size_t>(ch)] / 255.0f * (1.0f - g)) *
                            255.0f));
                    }
                    changed = true;
                }
            }
            if (orig != nullptr) {
                changed = changed || std::memcmp(orig, buf.data(), buf.size()) != 0;
            }
            if (changed) {
                auto tile = std::make_shared<Tile>();
                auto ebuf = std::make_unique<EngineBuffer>(kTile, kTile);
                std::memcpy(ebuf->mapCpuWrite(), buf.data(), buf.size());
                tile->pixels = std::move(ebuf);
                outGrid->tiles[key] = std::move(tile);
            }
        }
    }
    layer->pixels = outGrid;
    // 蒙版启用层重合成（桥接侧 restyleDocument 亦会覆盖样式派生；此处保持蒙版同步）
    if (layer->mask != nullptr && layer->mask->enabled) {
        layer->render = composeMasked(layer->pixels, *layer->mask);
    }
    return true;
}

}  // namespace montage
