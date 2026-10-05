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

}  // namespace montage
