// M19a 修补工具 + M19b 内容感知填充/移动实现。
// 泊松内核见 poisson_ops，受限源补全见 content_aware_ops（photocraft algo crate 翻译）。

#include "engine/heal_ops.h"

#include "engine/auto_sample.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "engine/content_aware_ops.h"
#include "engine/inpaint_ops.h"
#include "engine/poisson_ops.h"
#include "engine/seam_ops.h"
#include "engine/transform_ops.h"

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

// ---- M19b 内容感知填充 / 内容感知移动 ----

namespace {

// 活动层 + 选区 + 选区包围盒（文档域）解析；失败写 err 返回 false。
struct SelContext {
    Layer* layer = nullptr;
    const TileGrid* grid = nullptr;
    const TileGrid* sel = nullptr;
    int ox = 0, oy = 0, gw = 0, gh = 0;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;  // 选区包围盒（文档域）
};

bool resolveSelection(Document& doc, SelContext& ctx, std::string& err) {
    for (Layer& l : doc.layers) {
        if (l.id == doc.activeId) {
            ctx.layer = &l;
            break;
        }
    }
    if (ctx.layer == nullptr || ctx.layer->pixels == nullptr) {
        err = "no active layer pixels";
        return false;
    }
    if (doc.selection == nullptr) {
        err = "no selection";
        return false;
    }
    ctx.grid = ctx.layer->pixels.get();
    ctx.sel = doc.selection.get();
    ctx.gw = static_cast<int>(ctx.grid->cols * kTile);
    ctx.gh = static_cast<int>(ctx.grid->rows * kTile);
    ctx.ox = static_cast<int>(std::lround(ctx.layer->transform.originX));
    ctx.oy = static_cast<int>(std::lround(ctx.layer->transform.originY));
    ctx.bx0 = INT32_MAX;
    ctx.by0 = INT32_MAX;
    ctx.bx1 = INT32_MIN;
    ctx.by1 = INT32_MIN;
    for (uint32_t ty = 0; ty < ctx.sel->rows; ++ty) {
        for (uint32_t tx = 0; tx < ctx.sel->cols; ++tx) {
            auto it = ctx.sel->tiles.find(ty * ctx.sel->cols + tx);
            if (it == ctx.sel->tiles.end() || it->second == nullptr) {
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
                        ctx.bx0 = std::min(ctx.bx0, x);
                        ctx.by0 = std::min(ctx.by0, y);
                        ctx.bx1 = std::max(ctx.bx1, x + 1);
                        ctx.by1 = std::max(ctx.by1, y + 1);
                    }
                }
            }
        }
    }
    if (ctx.bx1 <= ctx.bx0) {
        err = "empty selection";
        return false;
    }
    return true;
}

}  // namespace

bool contentAwareFill(Document& doc, std::string& err) {
    SelContext ctx;
    if (!resolveSelection(doc, ctx, err)) {
        return false;
    }
    // 采样窗口（PS 公式，文档域）→ 网格局部裁剪
    const inpaint::RectI holeDoc{ctx.bx0, ctx.by0, ctx.bx1, ctx.by1};
    inpaint::RectI canvas{0, 0, static_cast<int>(doc.width), static_cast<int>(doc.height)};
    if (canvas.empty()) {
        canvas = inpaint::RectI{0, 0, ctx.gw, ctx.gh};
    }
    // P5④：16MP 预算 + 孔中心锚定向孔收缩（登记项收口；PS 采样域语义）
    inpaint::RectI win = inpaint::shrinkWindowTowardHole(
        inpaint::samplingWindow(holeDoc, canvas), holeDoc, 16.0e6);
    const int rx0 = std::max(0, win.x0 - ctx.ox), ry0 = std::max(0, win.y0 - ctx.oy);
    const int rx1 = std::min(ctx.gw, win.x1 - ctx.ox), ry1 = std::min(ctx.gh, win.y1 - ctx.oy);
    const int w = rx1 - rx0, h = ry1 - ry0;
    if (w <= 0 || h <= 0) {
        err = "selection outside layer";
        return false;
    }
    std::vector<float> img(static_cast<size_t>(w) * h * 4);
    std::vector<bool> hole(static_cast<size_t>(w) * h, false);
    bool anyHole = false;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t k = static_cast<size_t>(y) * w + x;
            const int gx = rx0 + x, gy = ry0 + y;
            const uint8_t* d = sampleAt(ctx.grid, gx, gy);
            if (d != nullptr) {
                for (int c = 0; c < 4; ++c) {
                    img[k * 4 + c] = d[c] / 255.0f;
                }
            }
            const uint8_t* sg = sampleAt(ctx.sel, gx + ctx.ox, gy + ctx.oy);
            if (sg != nullptr && sg[0] > 0) {
                hole[k] = true;
                anyHole = true;
            }
        }
    }
    if (!anyHole) {
        err = "empty selection";
        return false;
    }
    // P5③：Auto 采样区（图割判定与孔周边相似的像素；退化回退 = 孔外全部可用）
    std::vector<bool> source = autosample::autoSampling(static_cast<size_t>(w),
                                                        static_cast<size_t>(h), 4, img, hole);
    if (source.empty()) {
        source.assign(static_cast<size_t>(w) * h, false);
        for (size_t i = 0; i < source.size(); ++i) {
            source[i] = !hole[i];
        }
    }
    const std::vector<float> filled = content_aware::fill(
        static_cast<size_t>(w), static_cast<size_t>(h), 4, img, hole, source,
        content_aware::FillOptions::makeDefault());
    if (filled.empty()) {
        err = "cancelled";
        return false;
    }
    // 写回：RGBA 直混 × 选区灰度（替换式——填充结果透明处要能删除原墨迹）
    auto outGrid = std::make_shared<TileGrid>();
    outGrid->cols = ctx.grid->cols;
    outGrid->rows = ctx.grid->rows;
    outGrid->revision = ctx.grid->revision + 1;
    outGrid->tiles = ctx.grid->tiles;
    for (int ty = ry0 / static_cast<int>(kTile); ty <= (ry1 - 1) / static_cast<int>(kTile); ++ty) {
        for (int tx = rx0 / static_cast<int>(kTile); tx <= (rx1 - 1) / static_cast<int>(kTile);
             ++tx) {
            const uint32_t key = static_cast<uint32_t>(ty) * ctx.grid->cols + static_cast<uint32_t>(tx);
            std::vector<uint8_t> buf(static_cast<size_t>(kTile) * kTile * 4u, 0);
            const uint8_t* orig = nullptr;
            auto it = ctx.grid->tiles.find(key);
            if (it != ctx.grid->tiles.end() && it->second != nullptr) {
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
                    if (!hole[k]) {
                        continue;
                    }
                    const uint8_t* sg = sampleAt(ctx.sel, gx + ctx.ox, gy + ctx.oy);
                    const float g = sg != nullptr ? static_cast<float>(sg[0]) / 255.0f : 1.0f;
                    if (g <= 0.0f) {
                        continue;
                    }
                    uint8_t* p = buf.data() + (static_cast<size_t>(r) * kTile + c) * 4u;
                    for (int ch = 0; ch < 4; ++ch) {
                        p[static_cast<size_t>(ch)] = static_cast<uint8_t>(std::lround(
                            (filled[k * 4 + static_cast<size_t>(ch)] * g +
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
    ctx.layer->pixels = outGrid;
    if (ctx.layer->mask != nullptr && ctx.layer->mask->enabled) {
        ctx.layer->render = composeMasked(ctx.layer->pixels, *ctx.layer->mask);
    }
    return true;
}

bool contentAwareMove(Document& doc, int dx, int dy, bool extend, std::string& err) {
    if (dx == 0 && dy == 0) {
        err = "zero offset";
        return false;
    }
    SelContext ctx;
    if (!resolveSelection(doc, ctx, err)) {
        return false;
    }
    // 内容捕获（选区包围盒+1 环，网格局部）
    const int rx0 = std::max(0, ctx.bx0 - 1 - ctx.ox), ry0 = std::max(0, ctx.by0 - 1 - ctx.oy);
    const int rx1 = std::min(ctx.gw, ctx.bx1 + 1 - ctx.ox);
    const int ry1 = std::min(ctx.gh, ctx.by1 + 1 - ctx.oy);
    const int rw = rx1 - rx0, rh = ry1 - ry0;
    if (rw <= 0 || rh <= 0) {
        err = "selection outside layer";
        return false;
    }
    std::vector<float> content(static_cast<size_t>(rw) * rh * 4);
    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            const size_t k = static_cast<size_t>(y) * rw + x;
            const uint8_t* d = sampleAt(ctx.grid, rx0 + x, ry0 + y);
            if (d != nullptr) {
                for (int c = 0; c < 4; ++c) {
                    content[k * 4 + c] = d[c] / 255.0f;
                }
            }
        }
    }
    auto outGrid = std::make_shared<TileGrid>();
    outGrid->cols = ctx.grid->cols;
    outGrid->rows = ctx.grid->rows;
    outGrid->revision = ctx.grid->revision + 1;
    outGrid->tiles = ctx.grid->tiles;
    // 通用瓦片写回：批收集变更像素（文档域 → 值），末尾一次性应用
    std::vector<uint8_t> merged(static_cast<size_t>(ctx.gw) * ctx.gh * 4u);
    std::vector<bool> touched(ctx.gw * ctx.gh, false);
    auto applyPixel = [&](int gx, int gy, const float* rgba) {
        const size_t idx = static_cast<size_t>(gy) * ctx.gw + gx;
        uint8_t* p = &merged[idx * 4u];
        for (int ch = 0; ch < 4; ++ch) {
            p[static_cast<size_t>(ch)] =
                static_cast<uint8_t>(std::lround(std::min(1.0f, std::max(0.0f, rgba[ch])) * 255.0f));
        }
        touched[idx] = true;
    };
    // 第一步（!extend）：旧址内容感知填充（孔 = 选区，源 = 选区外采样窗口）
    if (!extend) {
        const inpaint::RectI holeDoc{ctx.bx0, ctx.by0, ctx.bx1, ctx.by1};
        inpaint::RectI canvas{0, 0, static_cast<int>(doc.width), static_cast<int>(doc.height)};
        if (canvas.empty()) {
            canvas = inpaint::RectI{0, 0, ctx.gw, ctx.gh};
        }
        inpaint::RectI win = inpaint::shrinkWindowTowardHole(
            inpaint::samplingWindow(holeDoc, canvas), holeDoc, 16.0e6);  // P5④
        const int fx0 = std::max(0, win.x0 - ctx.ox), fy0 = std::max(0, win.y0 - ctx.oy);
        const int fx1 = std::min(ctx.gw, win.x1 - ctx.ox), fy1 = std::min(ctx.gh, win.y1 - ctx.oy);
        const int fw = fx1 - fx0, fh = fy1 - fy0;
        if (fw > 0 && fh > 0) {
            std::vector<float> img(static_cast<size_t>(fw) * fh * 4);
            std::vector<bool> hole(static_cast<size_t>(fw) * fh, false);
            for (int y = 0; y < fh; ++y) {
                for (int x = 0; x < fw; ++x) {
                    const size_t k = static_cast<size_t>(y) * fw + x;
                    const int gx = fx0 + x, gy = fy0 + y;
                    const uint8_t* d = sampleAt(ctx.grid, gx, gy);
                    if (d != nullptr) {
                        for (int c = 0; c < 4; ++c) {
                            img[k * 4 + c] = d[c] / 255.0f;
                        }
                    }
                    const uint8_t* sg = sampleAt(ctx.sel, gx + ctx.ox, gy + ctx.oy);
                    hole[k] = sg != nullptr && sg[0] > 0;
                }
            }
            // P5③：Auto 采样区（同填充；退化回退孔外全部）
            std::vector<bool> source = autosample::autoSampling(
                static_cast<size_t>(fw), static_cast<size_t>(fh), 4, img, hole);
            if (source.empty()) {
                source.assign(static_cast<size_t>(fw) * fh, false);
                for (size_t i = 0; i < source.size(); ++i) {
                    source[i] = !hole[i];
                }
            }
            const std::vector<float> filled = content_aware::fill(
                static_cast<size_t>(fw), static_cast<size_t>(fh), 4, img, hole, source,
                content_aware::FillOptions::makeDefault());
            if (filled.empty()) {
                err = "cancelled";
                return false;
            }
            for (int y = 0; y < fh; ++y) {
                for (int x = 0; x < fw; ++x) {
                    const size_t k = static_cast<size_t>(y) * fw + x;
                    if (!hole[k]) {
                        continue;
                    }
                    const int gx = fx0 + x, gy = fy0 + y;
                    const uint8_t* sg = sampleAt(ctx.sel, gx + ctx.ox, gy + ctx.oy);
                    const float g = sg != nullptr ? static_cast<float>(sg[0]) / 255.0f : 1.0f;
                    if (g <= 0.0f) {
                        continue;
                    }
                    float outRgba[4];
                    const uint8_t* d = sampleAt(ctx.grid, gx, gy);
                    for (int ch = 0; ch < 4; ++ch) {
                        const float base = d != nullptr ? d[ch] / 255.0f : 0.0f;
                        outRgba[ch] =
                            filled[k * 4 + static_cast<size_t>(ch)] * g + base * (1.0f - g);
                    }
                    applyPixel(gx, gy, outRgba);
                }
            }
        }
    }
    // 第二步：新址泊松融入（src = 捕获内容按位移摆放，dst = 当前像素，mask = 位移后选区形）
    {
        const int nx0 = rx0 + dx, ny0 = ry0 + dy;
        const int nx1 = rx1 + dx, ny1 = ry1 + dy;
        const int cx0 = std::max(0, nx0), cy0 = std::max(0, ny0);
        const int cx1 = std::min(ctx.gw, nx1), cy1 = std::min(ctx.gh, ny1);
        const int cw = cx1 - cx0, chh = cy1 - cy0;
        if (cw > 0 && chh > 0) {
            std::vector<float> src(static_cast<size_t>(cw) * chh * 4),
                dst(static_cast<size_t>(cw) * chh * 4);
            std::vector<bool> mask(static_cast<size_t>(cw) * chh, false);
            bool any = false;
            for (int y = 0; y < chh; ++y) {
                for (int x = 0; x < cw; ++x) {
                    const size_t k = static_cast<size_t>(y) * cw + x;
                    const int gx = cx0 + x, gy = cy0 + y;
                    // 捕获域内 = 内容，域外 = 透明
                    const int lx = gx - nx0, ly = gy - ny0;
                    if (lx >= 0 && ly >= 0 && lx < rw && ly < rh) {
                        const size_t s = static_cast<size_t>(ly) * rw + static_cast<size_t>(lx);
                        for (int c = 0; c < 4; ++c) {
                            src[k * 4 + c] = content[s * 4 + c];
                        }
                    }
                    const uint8_t* d = sampleAt(ctx.grid, gx, gy);
                    if (d != nullptr) {
                        for (int c = 0; c < 4; ++c) {
                            dst[k * 4 + c] = d[c] / 255.0f;
                        }
                    }
                    const uint8_t* sg =
                        sampleAt(ctx.sel, gx - dx + ctx.ox, gy - dy + ctx.oy);
                    if (sg != nullptr && sg[0] > 0) {
                        mask[k] = true;
                        any = true;
                    }
                }
            }
            if (any) {
                const std::vector<float> blended = poisson::seamlessClone(
                    static_cast<size_t>(cw), static_cast<size_t>(chh), 4, src, dst, mask);
                for (int y = 0; y < chh; ++y) {
                    for (int x = 0; x < cw; ++x) {
                        const size_t k = static_cast<size_t>(y) * cw + x;
                        if (!mask[k]) {
                            continue;
                        }
                        const int gx = cx0 + x, gy = cy0 + y;
                        const uint8_t* sg =
                            sampleAt(ctx.sel, gx - dx + ctx.ox, gy - dy + ctx.oy);
                        const float g = sg != nullptr ? static_cast<float>(sg[0]) / 255.0f : 1.0f;
                        if (g <= 0.0f) {
                            continue;
                        }
                        float outRgba[4];
                        const uint8_t* d = sampleAt(ctx.grid, gx, gy);
                        for (int ch = 0; ch < 4; ++ch) {
                            const float base = d != nullptr ? d[ch] / 255.0f : 0.0f;
                            outRgba[ch] = blended[k * 4 + static_cast<size_t>(ch)] * g +
                                          base * (1.0f - g);
                        }
                        applyPixel(gx, gy, outRgba);
                    }
                }
            }
        }
    }
    // 应用批变更（COW 换瓦片）
    bool anyChange = false;
    for (int ty = 0; ty < static_cast<int>(ctx.grid->rows); ++ty) {
        for (int tx = 0; tx < static_cast<int>(ctx.grid->cols); ++tx) {
            const int px0 = tx * static_cast<int>(kTile), py0 = ty * static_cast<int>(kTile);
            const int px1 = std::min(ctx.gw, px0 + static_cast<int>(kTile));
            const int py1 = std::min(ctx.gh, py0 + static_cast<int>(kTile));
            bool tileTouched = false;
            for (int y = py0; y < py1 && !tileTouched; ++y) {
                for (int x = px0; x < px1; ++x) {
                    if (touched[static_cast<size_t>(y) * ctx.gw + x]) {
                        tileTouched = true;
                        break;
                    }
                }
            }
            if (!tileTouched) {
                continue;
            }
            const uint32_t key = static_cast<uint32_t>(ty) * ctx.grid->cols + static_cast<uint32_t>(tx);
            std::vector<uint8_t> buf(static_cast<size_t>(kTile) * kTile * 4u, 0);
            const uint8_t* orig = nullptr;
            auto it = ctx.grid->tiles.find(key);
            if (it != ctx.grid->tiles.end() && it->second != nullptr) {
                orig = it->second->pixels->mapCpu();
                if (orig != nullptr) {
                    std::memcpy(buf.data(), orig, buf.size());
                }
            }
            for (int y = py0; y < py1; ++y) {
                for (int x = px0; x < px1; ++x) {
                    if (!touched[static_cast<size_t>(y) * ctx.gw + x]) {
                        continue;
                    }
                    std::memcpy(&buf[(static_cast<size_t>(y - py0) * kTile +
                                      static_cast<size_t>(x - px0)) * 4u],
                                &merged[(static_cast<size_t>(y) * ctx.gw + x) * 4u], 4u);
                }
            }
            if (orig != nullptr) {
                anyChange = anyChange || std::memcmp(orig, buf.data(), buf.size()) != 0;
            } else {
                anyChange = true;
            }
            auto tile = std::make_shared<Tile>();
            auto ebuf = std::make_unique<EngineBuffer>(kTile, kTile);
            std::memcpy(ebuf->mapCpuWrite(), buf.data(), buf.size());
            tile->pixels = std::move(ebuf);
            outGrid->tiles[key] = std::move(tile);
        }
    }
    if (!anyChange) {
        err = "no change";
        return false;
    }
    ctx.layer->pixels = outGrid;
    if (ctx.layer->mask != nullptr && ctx.layer->mask->enabled) {
        ctx.layer->render = composeMasked(ctx.layer->pixels, *ctx.layer->mask);
    }
    return true;
}

// ---- M19c 内容感知比例 ----

bool contentAwareScale(Document& doc, uint32_t newW, uint32_t newH, std::string& err) {
    Layer* layer = nullptr;
    for (Layer& l : doc.layers) {
        if (l.id == doc.activeId) {
            layer = &l;
            break;
        }
    }
    if (layer == nullptr || layer->pixels == nullptr) {
        err = "no active layer pixels";
        return false;
    }
    if (newW == 0 || newH == 0 || newW > 16384 || newH > 16384) {
        err = "invalid size";
        return false;
    }
    const TileGrid* grid = layer->pixels.get();
    const int gw = static_cast<int>(grid->cols * kTile);
    const int gh = static_cast<int>(grid->rows * kTile);
    const int ox = static_cast<int>(std::lround(layer->transform.originX));
    const int oy = static_cast<int>(std::lround(layer->transform.originY));
    // 全图层网格雕刻（PS 语义：层内容含透明区整体缩放——按内容 bbox 雕会改变内容纵横比）
    const size_t w = static_cast<size_t>(gw), h = static_cast<size_t>(gh);
    if (w == 0 || h == 0) {
        err = "active layer has no pixels";
        return false;
    }
    if (w * h > 20e6) {
        err = "layer too large for content-aware scale (20MP budget)";
        return false;
    }
    std::vector<float> img(w * h * 4);
    bool any = false;
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            const uint8_t* d = sampleAt(grid, static_cast<int>(x), static_cast<int>(y));
            if (d != nullptr) {
                for (int c = 0; c < 4; ++c) {
                    img[(y * w + x) * 4 + c] = d[c] / 255.0f;
                }
                any = any || d[3] > 0;
            }
        }
    }
    if (!any) {
        err = "active layer has no pixels";
        return false;
    }
    // 保护图 = 选区灰度（文档域 → 网格域；无选区 = 无保护）
    std::vector<float> protect;
    const bool hasProt = doc.selection != nullptr;
    if (hasProt) {
        protect.assign(w * h, 0.0f);
        for (size_t y = 0; y < h; ++y) {
            for (size_t x = 0; x < w; ++x) {
                const uint8_t* sg = sampleAt(doc.selection.get(),
                                             static_cast<int>(x) + ox,
                                             static_cast<int>(y) + oy);
                protect[y * w + x] =
                    sg != nullptr ? static_cast<float>(sg[0]) / 255.0f : 0.0f;
            }
        }
    }
    const std::vector<float> carved =
        seam::carve(w, h, 4, img, hasProt ? &protect : nullptr, newW, newH);
    if (carved.empty() || carved.size() != newW * newH * 4) {
        err = "carve failed";
        return false;
    }
    // 重建图层网格（内容 (0,0) 锚定新尺寸）
    const size_t tw = (newW + kTile - 1) / kTile, th = (newH + kTile - 1) / kTile;
    auto outGrid = std::make_shared<TileGrid>();
    outGrid->cols = static_cast<uint32_t>(tw);
    outGrid->rows = static_cast<uint32_t>(th);
    outGrid->revision = grid->revision + 1;
    for (size_t tyI = 0; tyI < th; ++tyI) {
        for (size_t txI = 0; txI < tw; ++txI) {
            std::vector<uint8_t> buf(static_cast<size_t>(kTile) * kTile * 4u, 0);
            bool any = false;
            for (uint32_t r = 0; r < kTile; ++r) {
                const size_t y = tyI * kTile + r;
                if (y >= newH) {
                    break;
                }
                for (uint32_t c = 0; c < kTile; ++c) {
                    const size_t x = txI * kTile + c;
                    if (x >= newW) {
                        break;
                    }
                    std::memcpy(&buf[(static_cast<size_t>(r) * kTile + c) * 4u],
                                &carved[(y * newW + x) * 4], 4u);
                    any = any || buf[(static_cast<size_t>(r) * kTile + c) * 4u + 3u] > 0;
                }
            }
            if (any) {
                auto tile = std::make_shared<Tile>();
                auto ebuf = std::make_unique<EngineBuffer>(kTile, kTile);
                std::memcpy(ebuf->mapCpuWrite(), buf.data(), buf.size());
                tile->pixels = std::move(ebuf);
                outGrid->tiles[static_cast<uint32_t>(tyI) * outGrid->cols +
                               static_cast<uint32_t>(txI)] = std::move(tile);
            }
        }
    }
    layer->pixels = outGrid;
    layer->render = nullptr;  // 蒙版/样式派生域随内容重投影：由 restyleDocument 重建
    if (layer->mask != nullptr) {
        // 蒙版域不再对齐新内容几何：保留原蒙版非破坏语义由后续批处理，v1 随内容清蒙版登记
        // （与几何变换清派生栅格同类；像素蒙版重采样登记后续）
        layer->mask->enabled = false;
    }
    // 画布随缩放（0,0 锚定），几何命令派生态清理（与 resample 同不变式）
    if (!resizeCanvas(doc, 0, 0, static_cast<int>(newW), static_cast<int>(newH))) {
        err = "canvas resize failed";
        return false;
    }
    return true;
}

}  // namespace montage
