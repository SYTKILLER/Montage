#include "engine/filter_ops.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include <hilog/log.h>

#include "engine/filters.h"
#include "engine/parallel.h"

namespace montage {

namespace {
constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Filter";
}  // namespace

constexpr uint32_t kTile = kTileSize;

// bbox 展开为连续 strip（直 alpha RGBA8888，行距 = w*4）；网格外 = 透明。
void filterGridToStrip(const TileGrid& grid, uint32_t gx, uint32_t gy, uint32_t w, uint32_t h,
                 std::vector<uint8_t>& strip) {
    strip.assign(static_cast<size_t>(w) * h * 4u, 0);
    for (const auto& [key, tile] : grid.tiles) {
        const uint32_t tx = key % grid.cols;
        const uint32_t ty = key / grid.cols;
        const int gx0 = static_cast<int>(tx * kTile);
        const int gy0 = static_cast<int>(ty * kTile);
        // 瓦片与 bbox 的交叠域
        const int x0 = std::max(gx0, static_cast<int>(gx));
        const int y0 = std::max(gy0, static_cast<int>(gy));
        const int x1 = std::min(gx0 + static_cast<int>(kTile), static_cast<int>(gx + w));
        const int y1 = std::min(gy0 + static_cast<int>(kTile), static_cast<int>(gy + h));
        if (x0 >= x1 || y0 >= y1) {
            continue;
        }
        const uint8_t* src = tile->pixels->mapCpu();
        const uint32_t stride = tile->pixels->rowBytes();
        for (int y = y0; y < y1; ++y) {
            std::memcpy(strip.data() + (static_cast<size_t>(y - gy) * w + (x0 - gx)) * 4u,
                        src + static_cast<size_t>(y - gy0) * stride + static_cast<size_t>(x0 - gx0) * 4u,
                        static_cast<size_t>(x1 - x0) * 4u);
        }
        tile->pixels->unmap();
    }
}

// strip 发布回 TileGrid（全部 bbox 瓦片直拷；revision 随源递增）
std::shared_ptr<const TileGrid> filterStripToGrid(const std::vector<uint8_t>& strip, uint32_t w,
                                            uint32_t h, uint64_t revision) {
    TileGridBuilder builder((w + kTile - 1) / kTile, (h + kTile - 1) / kTile);
    for (uint32_t ty = 0; ty < builder.rows(); ++ty) {
        for (uint32_t tx = 0; tx < builder.cols(); ++tx) {
            EngineBuffer& buf = builder.ensureTile(tx, ty);
            uint8_t* dst = buf.mapCpuWrite();
            const uint32_t y0 = ty * kTile;
            const uint32_t x0 = tx * kTile;
            const uint32_t rows = std::min(kTile, h - y0);
            const uint32_t cols = std::min(kTile, w - x0);
            for (uint32_t r = 0; r < rows; ++r) {
                std::memcpy(dst + static_cast<size_t>(r) * kTile * 4u,
                            strip.data() + static_cast<size_t>(y0 + r) * w * 4u +
                                static_cast<size_t>(x0) * 4u,
                            static_cast<size_t>(cols) * 4u);
            }
        }
    }
    return builder.publish(revision);
}

uint64_t nextGridRevision() {
    static std::atomic<uint64_t> rev{1};
    return rev.fetch_add(1);
}

namespace {

// 选区灰度查表（doc.selection 域 = 文档坐标；strip 域 = 层局部 + bbox 原点）
struct SelSampler {
    const TileGrid* sel = nullptr;
    int ox = 0;
    int oy = 0;

    float grayAt(int lx, int ly) const {
        if (sel == nullptr) {
            return 1.0f;
        }
        return static_cast<float>(selectionGrayAt(*sel, ox + lx, oy + ly)) / 255.0f;
    }
};

}  // namespace

// 瓦片粒度内容包围盒。与墨迹精确的 gridInkBBox（document.h）分工：滤镜/样式域刻意
// 保持瓦片对齐（strip 原点 256 对齐，卷积/采样核在墨迹边缘留边界余量，P1-5 登记）。
bool layerContentBBox(const TileGrid& grid, uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h) {
    uint32_t tx0 = UINT32_MAX, ty0 = UINT32_MAX, tx1 = 0, ty1 = 0;
    for (const auto& [key, tile] : grid.tiles) {
        tx0 = std::min(tx0, key % grid.cols);
        ty0 = std::min(ty0, key / grid.cols);
        tx1 = std::max(tx1, key % grid.cols);
        ty1 = std::max(ty1, key / grid.cols);
    }
    if (tx0 == UINT32_MAX) {
        return false;
    }
    x = tx0 * kTile;
    y = ty0 * kTile;
    w = (tx1 - tx0 + 1u) * kTile;
    h = (ty1 - ty0 + 1u) * kTile;
    return true;
}

// P5② 分块并行 strip 执行器：512 对齐块 + filterHalo halo；块内整幅 applyFilter
//（复用全部核语义），有效区回写。块间无依赖 → parallelFor；块缘 clamp 只污染 halo
//（convSep 域语义），有效区与整幅逐字节一致。核可达 ≥ 256 退整幅（分块无收益）。
bool applyFilterStripTiled(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h,
                           const filters::FilterParams& p, int threads, std::string& err) {
    const int halo = filters::filterHalo(p);
    if (halo <= 0 || halo * 2 >= 256) {
        return false;
    }
    const uint32_t block = 512;
    const uint32_t nx = (w + block - 1) / block;
    const uint32_t ny = (h + block - 1) / block;
    const size_t nBlocks = static_cast<size_t>(nx) * ny;
    if (nBlocks <= 1) {
        return false;  // 单块 = 整幅路径（省双份拷贝）
    }
    std::atomic<bool> ok{true};
    parallelFor(0, nBlocks, 1, threads, [&](size_t lo, size_t hi) {
        for (size_t k = lo; k < hi; ++k) {
            const uint32_t bx = static_cast<uint32_t>(k % nx);
            const uint32_t by = static_cast<uint32_t>(k / nx);
            const int32_t x0 = static_cast<int32_t>(bx * block);
            const int32_t y0 = static_cast<int32_t>(by * block);
            const int32_t rw = static_cast<int32_t>(
                std::min<uint64_t>(block, w - bx * block));
            const int32_t rh = static_cast<int32_t>(
                std::min<uint64_t>(block, h - by * block));
            const int32_t sw = rw + halo * 2;
            const int32_t sh = rh + halo * 2;
            std::vector<uint8_t> in(static_cast<size_t>(sw) * sh * 4u, 0);
            std::vector<uint8_t> out(static_cast<size_t>(sw) * sh * 4u, 0);
            // 块 strip（含 halo）。域外填充语义随核：clamp 核复制图像边界像素
            //（== 整幅图像缘 clamp），透明核零填充（== getPxT 域外透明）
            const bool edgeClampFill = filters::filterEdgeClamped(p);
            for (int32_t r = 0; r < sh; ++r) {
                const int32_t gy = y0 - halo + r;
                const int32_t cy = std::clamp(gy, 0, static_cast<int32_t>(h) - 1);
                const bool rowIn = gy >= 0 && gy < static_cast<int32_t>(h);
                for (int32_t c = 0; c < sw; ++c) {
                    const int32_t gx = x0 - halo + c;
                    const int32_t cx = std::clamp(gx, 0, static_cast<int32_t>(w) - 1);
                    if (!edgeClampFill && !(gx == cx && gy == cy)) {
                        continue;  // 透明核：域外留零
                    }
                    std::memcpy(&in[(static_cast<size_t>(r) * sw + c) * 4u],
                                src + (static_cast<size_t>(rowIn ? gy : cy) * w + cx) * 4u,
                                4u);
                }
            }
            std::string berr;
            if (!filters::applyFilter(in.data(), out.data(), static_cast<uint32_t>(sw),
                                      static_cast<uint32_t>(sh), p, berr)) {
                ok = false;
                continue;
            }
            for (int32_t r = 0; r < rh; ++r) {
                std::memcpy(&dst[(static_cast<size_t>(y0 + r) * w + x0) * 4u],
                            &out[(static_cast<size_t>(r + halo) * sw + halo) * 4u],
                            static_cast<size_t>(rw) * 4u);
            }
        }
    });
    if (!ok) {
        err = "tiled filter: block failed";
        return false;
    }
    return true;
}

bool applyFilterToLayer(Document& doc, LayerId layerId, const filters::FilterParams& p,
                        std::string& err) {
    Layer* layer = nullptr;
    for (Layer& l : doc.layers) {
        if (l.id == layerId) {
            layer = &l;
            break;
        }
    }
    if (layer == nullptr) {
        err = "layer not found";
        return false;
    }
    if (layer->pixels == nullptr) {
        err = "layer is empty";
        return false;
    }
    if (layer->text != nullptr) {
        err = "rasterize text layer first";
        return false;
    }
    uint32_t gx = 0, gy = 0, w = 0, h = 0;
    if (!layerContentBBox(*layer->pixels, gx, gy, w, h)) {
        err = "no content";
        return false;
    }
    // D-1 预算：按 kind 折算（审查修复）——原 64MP 一刀切只算 strip 8B/px，模糊/锐化/
    // 自定义卷积内部还有 float 预乘中间缓冲（峰值 40..72 B/px，64MP 下 2.6..4.6GB）。
    // 预算闸对齐 ~1.5GB 峰值档：float 重路径 16MP、模糊路径 32MP、其余 64MP
    const uint64_t area = static_cast<uint64_t>(w) * h;
    const int kind = p.kind;
    const bool heavyFloat = kind == filters::FUnsharpMask || kind == filters::FSmartSharpen ||
                            kind == filters::FHighPass || kind == filters::FColorHalftone;
    const bool blurPath = kind == filters::FGaussianBlur || kind == filters::FBoxBlur ||
                          kind == filters::FMotionBlur || kind == filters::FAddNoise ||
                          kind == filters::FCustom;
    const uint64_t budget = heavyFloat ? 16ull * 1000 * 1000
                                       : (blurPath ? 32ull * 1000 * 1000 : 64ull * 1000 * 1000);
    if (area > budget) {
        err = "filter area exceeds budget";
        return false;
    }
    std::vector<uint8_t> strip;
    filterGridToStrip(*layer->pixels, gx, gy, w, h, strip);
    std::vector<uint8_t> out(strip.size(), 0);
    // P5②：大面积 + 平移不变核 → 分块并行（块输出与整幅逐字节一致；小图/图案核走整幅）
    bool tiledDone = false;
    if (area > 1500000ull) {
        tiledDone = applyFilterStripTiled(strip.data(), out.data(), w, h, p, 0, err);
    }
    if (!tiledDone && !filters::applyFilter(strip.data(), out.data(), w, h, p, err)) {
        return false;
    }
    // 选区约束（M7a coverage lerp；selection 在文档域，strip 原点 = transform.origin + bbox）
    const SelSampler sel{doc.selection.get(),
                         static_cast<int>(std::lround(layer->transform.originX)) + static_cast<int>(gx),
                         static_cast<int>(std::lround(layer->transform.originY)) + static_cast<int>(gy)};
    if (doc.selection != nullptr) {
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const float g = sel.grayAt(static_cast<int>(x), static_cast<int>(y));
                if (g >= 0.9995f) {
                    continue;
                }
                uint8_t* o = out.data() + (static_cast<size_t>(y) * w + x) * 4u;
                const uint8_t* s = strip.data() + (static_cast<size_t>(y) * w + x) * 4u;
                for (int c = 0; c < 4; ++c) {
                    o[c] = static_cast<uint8_t>(std::lround(s[c] * (1.0f - g) + o[c] * g));
                }
            }
        }
    }
    layer->pixels = filterStripToGrid(out, w, h, layer->pixels->revision + 1);
    if (layer->mask != nullptr && layer->mask->enabled) {
        layer->render = composeMasked(layer->pixels, *layer->mask);
    } else {
        layer->render = nullptr;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                 "filter kind=%{public}d applied at (%{public}u,%{public}u) %{public}u x %{public}u",
                 p.kind, gx, gy, w, h);
    return true;
}

}  // namespace montage
