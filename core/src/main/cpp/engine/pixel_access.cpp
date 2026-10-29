#include "engine/pixel_access.h"

#include <algorithm>
#include <cmath>

namespace montage {
namespace pixel_access {

std::vector<float> gridToF32(const TileGrid& grid, uint32_t w, uint32_t h) {
    std::vector<float> out(static_cast<size_t>(w) * h * 4u, 0.0f);
    if (w == 0 || h == 0) {
        return out;
    }
    for (uint32_t ty = 0; ty < grid.rows; ++ty) {
        for (uint32_t tx = 0; tx < grid.cols; ++tx) {
            const auto it = grid.tiles.find(ty * grid.cols + tx);
            if (it == grid.tiles.end() || it->second == nullptr) {
                continue;
            }
            const PixelSource& src = *it->second->pixels;
            const uint8_t* base = src.mapCpu();
            // 瓦片与有效域交集
            const uint32_t x0 = tx * kTileSize;
            const uint32_t y0 = ty * kTileSize;
            const uint32_t x1 = std::min<uint32_t>(x0 + kTileSize, w);
            const uint32_t y1 = std::min<uint32_t>(y0 + kTileSize, h);
            if (x0 >= x1 || y0 >= y1) {
                continue;
            }
            for (uint32_t y = y0; y < y1; ++y) {
                const uint8_t* row = base + static_cast<size_t>(y - y0) * src.rowBytes();
                float* orow = out.data() + (static_cast<size_t>(y) * w + x0) * 4u;
                switch (grid.depth) {
                    case PixelDepth::U8:
                        for (uint32_t x = x0; x < x1; ++x) {
                            const uint8_t* p = row + static_cast<size_t>(x - x0) * 4u;
                            orow[0] = p[0] * (1.0f / 255.0f);
                            orow[1] = p[1] * (1.0f / 255.0f);
                            orow[2] = p[2] * (1.0f / 255.0f);
                            orow[3] = p[3] * (1.0f / 255.0f);
                            orow += 4;
                        }
                        break;
                    case PixelDepth::U16: {
                        constexpr float kInv = 1.0f / 65535.0f;
                        for (uint32_t x = x0; x < x1; ++x) {
                            const uint16_t* p =
                                reinterpret_cast<const uint16_t*>(row) +
                                static_cast<size_t>(x - x0) * 4u;
                            orow[0] = p[0] * kInv;
                            orow[1] = p[1] * kInv;
                            orow[2] = p[2] * kInv;
                            orow[3] = p[3] * kInv;
                            orow += 4;
                        }
                        break;
                    }
                    case PixelDepth::F32:
                        for (uint32_t x = x0; x < x1; ++x) {
                            const float* p =
                                reinterpret_cast<const float*>(row) +
                                static_cast<size_t>(x - x0) * 4u;
                            orow[0] = p[0];
                            orow[1] = p[1];
                            orow[2] = p[2];
                            orow[3] = p[3];
                            orow += 4;
                        }
                        break;
                }
            }
        }
    }
    return out;
}

std::shared_ptr<const TileGrid> f32ToGrid(const float* data, uint32_t w, uint32_t h,
                                          PixelDepth depth, uint64_t revision) {
    if (w == 0 || h == 0) {
        auto empty = std::make_shared<TileGrid>();
        empty->depth = depth;
        empty->revision = revision;
        return empty;
    }
    const uint32_t cols = (w + kTileSize - 1) / kTileSize;
    const uint32_t rows = (h + kTileSize - 1) / kTileSize;
    TileGridBuilder builder(cols, rows, depth);
    for (uint32_t ty = 0; ty < rows; ++ty) {
        for (uint32_t tx = 0; tx < cols; ++tx) {
            EngineBuffer& buf = builder.ensureTile(tx, ty);
            uint8_t* base = buf.mapCpuWrite();
            const uint32_t x0 = tx * kTileSize;
            const uint32_t y0 = ty * kTileSize;
            const uint32_t x1 = std::min<uint32_t>(x0 + kTileSize, w);
            const uint32_t y1 = std::min<uint32_t>(y0 + kTileSize, h);
            for (uint32_t y = y0; y < y1; ++y) {
                uint8_t* orow = base + static_cast<size_t>(y - y0) * buf.rowBytes();
                const float* irow = data + (static_cast<size_t>(y) * w + x0) * 4u;
                switch (depth) {
                    case PixelDepth::U8:
                        for (uint32_t x = x0; x < x1; ++x) {
                            uint8_t* p = orow + static_cast<size_t>(x - x0) * 4u;
                            for (int c = 0; c < 4; ++c) {
                                float v = irow[c];
                                v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                                p[c] = static_cast<uint8_t>(v * 255.0f + 0.5f);
                            }
                            irow += 4;
                        }
                        break;
                    case PixelDepth::U16:
                        for (uint32_t x = x0; x < x1; ++x) {
                            uint16_t* p = reinterpret_cast<uint16_t*>(orow) +
                                          static_cast<size_t>(x - x0) * 4u;
                            for (int c = 0; c < 4; ++c) {
                                float v = irow[c];
                                v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                                p[c] = static_cast<uint16_t>(v * 65535.0f + 0.5f);
                            }
                            irow += 4;
                        }
                        break;
                    case PixelDepth::F32:
                        for (uint32_t x = x0; x < x1; ++x) {
                            float* p = reinterpret_cast<float*>(orow) +
                                       static_cast<size_t>(x - x0) * 4u;
                            p[0] = irow[0];
                            p[1] = irow[1];
                            p[2] = irow[2];
                            p[3] = irow[3];
                            irow += 4;
                        }
                        break;
                }
            }
        }
    }
    return builder.publish(revision);
}

std::shared_ptr<const TileGrid> gridToU8Grid(const TileGrid& grid, uint64_t revision) {
    if (grid.depth == PixelDepth::U8) {
        return std::make_shared<TileGrid>(grid);  // 复制壳（tiles shared_ptr 共享）
    }
    // 尺寸推断：网格像素域（列数×瓦片；调用方传有效域时可另用 f32ToGrid 路径）。
    const uint32_t w = grid.cols * kTileSize;
    const uint32_t h = grid.rows * kTileSize;
    std::vector<float> flat = gridToF32(grid, w, h);
    for (float& v : flat) {
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    return f32ToGrid(flat.data(), w, h, PixelDepth::U8, revision);
}

DeepEditGuard::DeepEditGuard(Layer& l, uint64_t revision) : layer(&l) {
    if (l.pixels != nullptr && l.pixels->depth != PixelDepth::U8) {
        savedDepth = l.depth;
        const uint64_t r = l.pixels->revision;
        l.pixels = gridToU8Grid(*l.pixels, revision != 0 ? revision : r + 1);
        l.depth = PixelDepth::U8;
        active = true;
    }
}

DeepEditGuard::~DeepEditGuard() {
    if (!active || layer == nullptr || layer->pixels == nullptr) {
        return;
    }
    const TileGrid& u8 = *layer->pixels;
    const uint32_t w = u8.cols * kTileSize;
    const uint32_t h = u8.rows * kTileSize;
    std::vector<float> flat = gridToF32(u8, w, h);
    layer->pixels = f32ToGrid(flat.data(), w, h, savedDepth, u8.revision + 1);
    layer->depth = savedDepth;
}

std::vector<DeepEditGuard> guardAllLayers(std::vector<Layer>& layers) {
    std::vector<DeepEditGuard> guards;
    guards.reserve(layers.size());
    for (Layer& l : layers) {
        guards.emplace_back(l, l.pixels != nullptr ? l.pixels->revision + 1 : 1);
    }
    return guards;
}

}  // namespace pixel_access
}  // namespace montage
