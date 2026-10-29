#ifndef MONTAGE_ENGINE_PIXEL_ACCESS_H
#define MONTAGE_ENGINE_PIXEL_ACCESS_H

// P6 D1-b 像素访问垫片（位深泛化的唯一进出原语）。三深度网格 ↔ 归一化 f32 域：
//   gridGetF32 / gridPutF32    —— 网格局部坐标单像素归一化读写（U8 v/255、U16 v/65535、F32 原值）
//   gridToF32 / f32ToGrid      —— 整网格 ↔ 连续 f32 RGBA 平面（w*h*4，编辑域工作缓冲）
//   gridToU8Grid               —— 任意深度 → U8 显示域网格（渲染/缩略图/探针消费端降位）
// U8 网格走快速路径（memcpy 语义等价）；消费方未适配深度前一律经此垫片，禁止摸裸指针。
// 直 alpha 语义不变；F32 值域不钳（[0,1] 外值按写入源语义保留，显示端钳制）。

#include <cstring>
#include <memory>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace pixel_access {

// ---- 深格式单像素 f32 读写（共享原语：brush 深域双写 / composeMasked / 编辑垫片）----
// U8 v/255、U16 v/65535、F32 原值；写端钳 0..1（F32 原样）。
inline void pixelDepthReadF32(PixelDepth d, const uint8_t* p, float out[4]) {
    switch (d) {
        case PixelDepth::U8:
            for (int c = 0; c < 4; ++c) {
                out[c] = p[c] * (1.0f / 255.0f);
            }
            break;
        case PixelDepth::U16: {
            const uint16_t* q = reinterpret_cast<const uint16_t*>(p);
            constexpr float kInv = 1.0f / 65535.0f;
            for (int c = 0; c < 4; ++c) {
                out[c] = q[c] * kInv;
            }
            break;
        }
        case PixelDepth::F32: {
            const float* q = reinterpret_cast<const float*>(p);
            out[0] = q[0];
            out[1] = q[1];
            out[2] = q[2];
            out[3] = q[3];
            break;
        }
    }
}

inline void pixelDepthWriteF32(PixelDepth d, uint8_t* p, const float in[4]) {
    switch (d) {
        case PixelDepth::U8:
            for (int c = 0; c < 4; ++c) {
                float v = in[c];
                v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                p[c] = static_cast<uint8_t>(v * 255.0f + 0.5f);
            }
            break;
        case PixelDepth::U16: {
            uint16_t* q = reinterpret_cast<uint16_t*>(p);
            for (int c = 0; c < 4; ++c) {
                float v = in[c];
                v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                q[c] = static_cast<uint16_t>(v * 65535.0f + 0.5f);
            }
            break;
        }
        case PixelDepth::F32: {
            float* q = reinterpret_cast<float*>(p);
            q[0] = in[0];
            q[1] = in[1];
            q[2] = in[2];
            q[3] = in[3];
            break;
        }
    }
}

// 网格局部坐标 (gx, gy) 单像素归一化读（out = RGBA 0..1；越界返回 false 且 out 清零）。
inline bool gridGetF32(const TileGrid& grid, int gx, int gy, float out[4]) {
    if (gx < 0 || gy < 0 || gx >= static_cast<int>(grid.cols) * static_cast<int>(kTileSize) ||
        gy >= static_cast<int>(grid.rows) * static_cast<int>(kTileSize)) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return false;
    }
    const uint32_t tx = static_cast<uint32_t>(gx) / kTileSize;
    const uint32_t ty = static_cast<uint32_t>(gy) / kTileSize;
    const auto it = grid.tiles.find(ty * grid.cols + tx);
    if (it == grid.tiles.end() || it->second == nullptr) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return false;
    }
    const PixelSource& src = *it->second->pixels;
    const uint8_t* row = src.mapCpu() + static_cast<size_t>(gy % kTileSize) * src.rowBytes();
    const uint32_t lx = static_cast<uint32_t>(gx) % kTileSize;
    switch (grid.depth) {
        case PixelDepth::U8: {
            const uint8_t* p = row + static_cast<size_t>(lx) * 4u;
            out[0] = p[0] * (1.0f / 255.0f);
            out[1] = p[1] * (1.0f / 255.0f);
            out[2] = p[2] * (1.0f / 255.0f);
            out[3] = p[3] * (1.0f / 255.0f);
            return true;
        }
        case PixelDepth::U16: {
            const uint16_t* p = reinterpret_cast<const uint16_t*>(row) + static_cast<size_t>(lx) * 4u;
            constexpr float kInv = 1.0f / 65535.0f;
            out[0] = p[0] * kInv;
            out[1] = p[1] * kInv;
            out[2] = p[2] * kInv;
            out[3] = p[3] * kInv;
            return true;
        }
        case PixelDepth::F32: {
            const float* p = reinterpret_cast<const float*>(row) + static_cast<size_t>(lx) * 4u;
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            out[3] = p[3];
            return true;
        }
    }
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    return false;
}

// 网格局部坐标 (gx, gy) 单像素归一化写（需构建期可变网格；TileGridBuilder 瓦片为
// EngineBuffer 可变访问）。越界忽略。
inline void gridPutF32(TileGridBuilder& builder, int gx, int gy, const float in[4]) {
    if (gx < 0 || gy < 0) {
        return;
    }
    const uint32_t ux = static_cast<uint32_t>(gx);
    const uint32_t uy = static_cast<uint32_t>(gy);
    if (ux >= builder.cols() * kTileSize || uy >= builder.rows() * kTileSize) {
        return;
    }
    EngineBuffer& buf = builder.ensureTile(ux / kTileSize, uy / kTileSize);
    uint8_t* row = buf.mapCpuWrite() + static_cast<size_t>(uy % kTileSize) * buf.rowBytes();
    const uint32_t lx = ux % kTileSize;
    switch (buf.depth()) {
        case PixelDepth::U8: {
            uint8_t* p = row + static_cast<size_t>(lx) * 4u;
            for (int c = 0; c < 4; ++c) {
                float v = in[c];
                v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                p[c] = static_cast<uint8_t>(v * 255.0f + 0.5f);
            }
            return;
        }
        case PixelDepth::U16: {
            uint16_t* p = reinterpret_cast<uint16_t*>(row) + static_cast<size_t>(lx) * 4u;
            for (int c = 0; c < 4; ++c) {
                float v = in[c];
                v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                p[c] = static_cast<uint16_t>(v * 65535.0f + 0.5f);
            }
            return;
        }
        case PixelDepth::F32: {
            float* p = reinterpret_cast<float*>(row) + static_cast<size_t>(lx) * 4u;
            p[0] = in[0];
            p[1] = in[1];
            p[2] = in[2];
            p[3] = in[3];
            return;
        }
    }
}

// 整网格 → 连续 f32 RGBA 平面（w*h*4；grid 的像素域 = cols*kTile × rows*kTile 但
// 有效域由调用方 w/h 裁定——通常为层内容尺寸）。越界/缺瓦片 = 透明。
std::vector<float> gridToF32(const TileGrid& grid, uint32_t w, uint32_t h);

// 连续 f32 RGBA 平面 → 任意深度网格（256 对齐稀疏瓦片，与导入发布同构）。
std::shared_ptr<const TileGrid> f32ToGrid(const float* data, uint32_t w, uint32_t h,
                                          PixelDepth depth, uint64_t revision);

// 任意深度 → U8 显示域网格（渲染/缩略图/探针降位；钳制 0..1 后量化）。
// U8 输入直接共享指针返回（零拷贝）。
std::shared_ptr<const TileGrid> gridToU8Grid(const TileGrid& grid, uint64_t revision);

// ---- 编辑族统一垫片（P6 D1-b v1 边界）----
// 画笔（最高频）走 brush.cpp 真深域双写；其余编辑操作（滤镜/填充/渐变/清除/变换…）
// 经 DeepEditGuard：构造降 8 临时工作网格（全链 U8 假设零改动）→ 析构升位写回层深度。
// 用法：history().beginEdit(...) 之后构造（快照留深态），操作完毕析构（endEdit 前升位）。
// 精度语义：操作在 8 位处理域执行（U16/F32 内核 float 化登记后续批）。
struct DeepEditGuard {
    Layer* layer = nullptr;
    PixelDepth savedDepth = PixelDepth::U8;
    bool active = false;

    DeepEditGuard() = default;
    explicit DeepEditGuard(Layer& l, uint64_t revision);
    ~DeepEditGuard();
    DeepEditGuard(DeepEditGuard&& o) noexcept
        : layer(o.layer), savedDepth(o.savedDepth), active(o.active) {
        o.layer = nullptr;
        o.active = false;
    }
    DeepEditGuard& operator=(DeepEditGuard&& o) noexcept {
        if (this != &o) {
            this->~DeepEditGuard();
            layer = o.layer;
            savedDepth = o.savedDepth;
            active = o.active;
            o.layer = nullptr;
            o.active = false;
        }
        return *this;
    }
    DeepEditGuard(const DeepEditGuard&) = delete;
    DeepEditGuard& operator=(const DeepEditGuard&) = delete;
};

// 文档全层 guard（变换族：flip/rotate/resize/resample/bake——像素搬运降位-升位无损；
// resample 插值域 U8 登记）。拷贝自 DeepEditGuard 语义，vector 析构逆序=升位逆序正确。
std::vector<DeepEditGuard> guardAllLayers(std::vector<Layer>& layers);

}  // namespace pixel_access
}  // namespace montage

#endif  // MONTAGE_ENGINE_PIXEL_ACCESS_H
