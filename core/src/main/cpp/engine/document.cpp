#include "engine/document.h"

#include <algorithm>
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

}  // namespace montage
