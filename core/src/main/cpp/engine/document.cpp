#include "engine/document.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

namespace montage {

EngineBuffer::EngineBuffer(uint32_t width, uint32_t height)
    : width_(width), height_(height), rowBytes_(width * 4u) {
    bytes_ = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(rowBytes_) * height_));
    if (bytes_ == nullptr) {
        throw std::bad_alloc();
    }
    std::memset(bytes_, 0, static_cast<size_t>(rowBytes_) * height_);
}

EngineBuffer::~EngineBuffer() {
    std::free(bytes_);
}

const uint8_t* EngineBuffer::mapCpu() const {
    return bytes_;
}

uint8_t* EngineBuffer::mapCpuWrite() {
    return bytes_;
}

EngineBuffer& TileGridBuilder::ensureTile(uint32_t tx, uint32_t ty) {
    const uint32_t idx = tileIndex(tx, ty);
    auto it = tiles_.find(idx);
    if (it != tiles_.end()) {
        // 构建期内同一瓦片只写入一次（按行带推进），重复获取返回既有 buffer
        auto* tile = const_cast<Tile*>(it->second.get());
        return *static_cast<EngineBuffer*>(tile->pixels.get());
    }
    auto tile = std::make_shared<Tile>();
    tile->pixels = std::make_unique<EngineBuffer>(kTileSize, kTileSize);
    auto* buf = static_cast<EngineBuffer*>(tile->pixels.get());
    tiles_.emplace(idx, std::move(tile));
    return *buf;
}

bool TileGridBuilder::hasTile(uint32_t tx, uint32_t ty) const {
    return tiles_.find(tileIndex(tx, ty)) != tiles_.end();
}

std::shared_ptr<const TileGrid> TileGridBuilder::publish(uint64_t revision) {
    auto grid = std::make_shared<TileGrid>();
    grid->cols = cols_;
    grid->rows = rows_;
    grid->revision = revision;
    grid->tiles = tiles_;  // shared_ptr 共享，拷贝轻量
    return grid;
}

double clampZoom(double zoom) {
    return std::min(32.0, std::max(1.0 / 32.0, zoom));
}

const char* blendModeName(BlendMode mode) {
    switch (mode) {
        case BlendMode::Darken: return "Darken";
        case BlendMode::Multiply: return "Multiply";
        case BlendMode::ColorBurn: return "Color Burn";
        case BlendMode::LinearBurn: return "Linear Burn";
        case BlendMode::Lighten: return "Lighten";
        case BlendMode::Screen: return "Screen";
        case BlendMode::ColorDodge: return "Color Dodge";
        case BlendMode::LinearDodge: return "Linear Dodge (Add)";
        case BlendMode::Overlay: return "Overlay";
        case BlendMode::SoftLight: return "Soft Light";
        case BlendMode::HardLight: return "Hard Light";
        case BlendMode::VividLight: return "Vivid Light";
        case BlendMode::LinearLight: return "Linear Light";
        case BlendMode::PinLight: return "Pin Light";
        case BlendMode::HardMix: return "Hard Mix";
        case BlendMode::Difference: return "Difference";
        case BlendMode::Exclusion: return "Exclusion";
        case BlendMode::Subtract: return "Subtract";
        case BlendMode::Divide: return "Divide";
        case BlendMode::Hue: return "Hue";
        case BlendMode::Saturation: return "Saturation";
        case BlendMode::Color: return "Color";
        case BlendMode::Luminosity: return "Luminosity";
        case BlendMode::Normal:
        default:
            return "Normal";
    }
}

bool blendModeFromName(const std::string& name, BlendMode& out) {
    static const std::pair<const char*, BlendMode> kTable[] = {
        {"Normal", BlendMode::Normal},
        {"Darken", BlendMode::Darken},
        {"Multiply", BlendMode::Multiply},
        {"Color Burn", BlendMode::ColorBurn},
        {"Linear Burn", BlendMode::LinearBurn},
        {"Lighten", BlendMode::Lighten},
        {"Screen", BlendMode::Screen},
        {"Color Dodge", BlendMode::ColorDodge},
        {"Linear Dodge (Add)", BlendMode::LinearDodge},
        {"Overlay", BlendMode::Overlay},
        {"Soft Light", BlendMode::SoftLight},
        {"Hard Light", BlendMode::HardLight},
        {"Vivid Light", BlendMode::VividLight},
        {"Linear Light", BlendMode::LinearLight},
        {"Pin Light", BlendMode::PinLight},
        {"Hard Mix", BlendMode::HardMix},
        {"Difference", BlendMode::Difference},
        {"Exclusion", BlendMode::Exclusion},
        {"Subtract", BlendMode::Subtract},
        {"Divide", BlendMode::Divide},
        {"Hue", BlendMode::Hue},
        {"Saturation", BlendMode::Saturation},
        {"Color", BlendMode::Color},
        {"Luminosity", BlendMode::Luminosity},
    };
    for (const auto& [key, mode] : kTable) {
        if (name == key) {
            out = mode;
            return true;
        }
    }
    return false;
}

uint8_t selectionGrayAt(const TileGrid& mask, int docX, int docY) {
    if (docX < 0 || docY < 0) {
        return 0;
    }
    const uint32_t ux = static_cast<uint32_t>(docX);
    const uint32_t uy = static_cast<uint32_t>(docY);
    const uint32_t tx = ux / kTileSize;
    const uint32_t ty = uy / kTileSize;
    if (tx >= mask.cols || ty >= mask.rows) {
        return 0;
    }
    auto it = mask.tiles.find(ty * mask.cols + tx);
    if (it == mask.tiles.end()) {
        return 0;  // 稀疏网格缺瓦片 = 未选中
    }
    const uint8_t* px = it->second->pixels->mapCpu();
    return px[(static_cast<size_t>(uy % kTileSize) * kTileSize) + static_cast<size_t>(ux % kTileSize)];
}

uint8_t layerMaskGrayAt(const LayerMask& mask, int gx, int gy) {
    const int px0 = mask.offsetX;
    const int py0 = mask.offsetY;
    if (gx < px0 || gy < py0 || gx >= px0 + static_cast<int>(mask.width) ||
        gy >= py0 + static_cast<int>(mask.height)) {
        return mask.outside;
    }
    if (mask.pixels == nullptr) {
        return mask.outside;
    }
    const TileGrid& patch = *mask.pixels;
    constexpr int kTile = static_cast<int>(kTileSize);
    const int mx = gx - px0;
    const int my = gy - py0;
    const uint32_t mtx = static_cast<uint32_t>(mx) / kTileSize;
    const uint32_t mty = static_cast<uint32_t>(my) / kTileSize;
    auto it = patch.tiles.find(mty * patch.cols + mtx);
    if (it == patch.tiles.end()) {
        return 0;  // 网格内缺瓦片 = 透明（构建方保证不缺，防御）
    }
    const uint8_t* mPx = it->second->pixels->mapCpu();
    return mPx[(static_cast<size_t>(my % kTileSize) * kTileSize) + static_cast<size_t>(mx % kTileSize)];
}

std::shared_ptr<const TileGrid> composeMasked(std::shared_ptr<const TileGrid> src,
                                              const LayerMask& mask) {
    if (src == nullptr) {
        return nullptr;
    }
    const TileGrid& shared = *src;
    auto out = std::make_shared<TileGrid>();
    out->cols = shared.cols;
    out->rows = shared.rows;
    out->revision = shared.revision;
    // 无 patch：整层取 outside（255 = 全显 → 原样返回；0 = 全隐 → 空网格）
    if (mask.pixels == nullptr) {
        if (mask.outside >= 128) {
            return src;
        }
        return out;
    }
    constexpr int kTile = static_cast<int>(kTileSize);
    // patch 在图层局部坐标的包围盒
    const int px0 = mask.offsetX;
    const int py0 = mask.offsetY;
    const int px1 = px0 + static_cast<int>(mask.width);
    const int py1 = py0 + static_cast<int>(mask.height);
    const bool outsideReveal = mask.outside >= 128;

    for (const auto& [key, tile] : shared.tiles) {
        const uint32_t tx = key % shared.cols;
        const uint32_t ty = key / shared.cols;
        const int x0 = static_cast<int>(tx * kTileSize);
        const int y0 = static_cast<int>(ty * kTileSize);
        const int x1 = x0 + kTile;
        const int y1 = y0 + kTile;
        // 与 patch 不相交：outside 语义决定共享/丢弃
        if (x1 <= px0 || x0 >= px1 || y1 <= py0 || y0 >= py1) {
            if (outsideReveal) {
                out->tiles[key] = tile;
            }
            continue;
        }
        auto buf = std::make_unique<EngineBuffer>(kTileSize, kTileSize);
        uint8_t* dst = buf->mapCpuWrite();
        const uint8_t* srcPx = tile->pixels->mapCpu();
        const uint32_t srcStride = tile->pixels->rowBytes();
        for (int ry = 0; ry < kTile; ++ry) {
            const int gy = y0 + ry;  // 图层局部 y
            const uint8_t* srcRow = srcPx + static_cast<size_t>(ry) * srcStride;
            uint8_t* dstRow = dst + static_cast<size_t>(ry) * kTileSize * 4u;
            for (int rx = 0; rx < kTile; ++rx) {
                const int gx = x0 + rx;
                const uint8_t m = layerMaskGrayAt(mask, gx, gy);
                const uint8_t* s = srcRow + static_cast<size_t>(rx) * 4u;
                uint8_t* d = dstRow + static_cast<size_t>(rx) * 4u;
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = static_cast<uint8_t>((s[3] * m + 127) / 255);
            }
        }
        auto t = std::make_shared<Tile>();
        t->pixels = std::move(buf);
        out->tiles[key] = std::move(t);
    }
    return out;
}

// 调整层 RGB LUT 生成（M9a）：LUT 类 kind 全覆盖；HueSaturation 走 GPU 逐像素 shader 不在此列。
void buildAdjustmentRgbLut(const LayerAdjustment& adj, float r[256], float g[256], float b[256]) {
    // 恒等初始化
    for (int i = 0; i < 256; ++i) {
        r[i] = g[i] = b[i] = static_cast<float>(i) / 255.0f;
    }
    switch (adj.kind) {
        case AdjustmentKind::Levels: {
            const float lo = std::min(adj.inBlack, adj.inWhite - 0.001f);
            const float hi = std::max(adj.inWhite, lo + 0.001f);
            const float gm = std::min(10.0f, std::max(0.1f, adj.gamma));
            for (int i = 0; i < 256; ++i) {
                float t = (static_cast<float>(i) / 255.0f - lo) / (hi - lo);
                t = std::min(1.0f, std::max(0.0f, t));
                r[i] = g[i] = b[i] = std::pow(t, 1.0f / gm);
            }
            break;
        }
        case AdjustmentKind::Curves: {
            const auto& xs = adj.curveX;
            const auto& ys = adj.curveY;
            if (xs.size() < 2 || xs.size() != ys.size()) {
                break;  // 恒等
            }
            for (int i = 0; i < 256; ++i) {
                const float x = static_cast<float>(i) / 255.0f;
                float v = x;
                if (x <= xs.front()) {
                    v = ys.front();
                } else if (x >= xs.back()) {
                    v = ys.back();
                } else {
                    for (size_t k = 0; k + 1 < xs.size(); ++k) {
                        if (x >= xs[k] && x <= xs[k + 1]) {
                            const float t = (x - xs[k]) / std::max(1e-6f, xs[k + 1] - xs[k]);
                            v = ys[k] + (ys[k + 1] - ys[k]) * t;
                            break;
                        }
                    }
                }
                r[i] = g[i] = b[i] = std::min(4.0f, std::max(-0.0f, v));
            }
            break;
        }
        case AdjustmentKind::Invert: {
            for (int i = 0; i < 256; ++i) {
                r[i] = g[i] = b[i] = static_cast<float>(255 - i) / 255.0f;
            }
            break;
        }
        case AdjustmentKind::Threshold: {
            const float level = std::min(255.0f, std::max(0.0f, adj.p0));
            for (int i = 0; i < 256; ++i) {
                r[i] = g[i] = b[i] = static_cast<float>(i) >= level ? 1.0f : 0.0f;  // PS：≥阈值白
            }
            break;
        }
        case AdjustmentKind::Posterize: {
            // PS 语义：floor 均分箱（levels 级，值 0/step/2step...），lround 会错 bin
            const int levels = static_cast<int>(std::min(32.0f, std::max(2.0f, adj.p0)));
            const float step = 255.0f / (levels - 1);
            for (int i = 0; i < 256; ++i) {
                const int bin = std::min(levels - 1,
                    static_cast<int>(static_cast<float>(i) * levels / 255.0f));  // N 段分箱
                r[i] = g[i] = b[i] = bin * step / 255.0f;
            }
            break;
        }
        case AdjustmentKind::BrightnessContrast: {
            // PS 线性近似（现代版非线性为曲线拟合，登记 v1 近似）
            const float bright = adj.p0 / 255.0f;         // -150..150 → ±0.588
            const float c = adj.p1 / 100.0f;              // -0.5..1
            const float k = std::max(0.0f, 1.0f + c);     // 斜率
            for (int i = 0; i < 256; ++i) {
                const float v = static_cast<float>(i) / 255.0f;
                float out = (v - 0.5f) * k + 0.5f + bright;
                r[i] = g[i] = b[i] = std::min(1.0f, std::max(0.0f, out));
            }
            break;
        }
        case AdjustmentKind::ColorBalance: {
            const float dr = adj.p0 / 255.0f;  // -100..100 → ±0.392
            const float dg = adj.p1 / 255.0f;
            const float db = adj.p2 / 255.0f;
            for (int i = 0; i < 256; ++i) {
                const float v = static_cast<float>(i) / 255.0f;
                r[i] = std::min(1.0f, std::max(0.0f, v + dr));
                g[i] = std::min(1.0f, std::max(0.0f, v + dg));
                b[i] = std::min(1.0f, std::max(0.0f, v + db));
            }
            break;
        }
        default:
            break;  // HueSaturation（shader 类）与未知 kind → 恒等
    }
}

}  // namespace montage
