#ifndef MONTAGE_IO_CMS_LUT3D_H
#define MONTAGE_IO_CMS_LUT3D_H

// M22 色彩管理内核 · 显示变换上 GPU 用的 3D 查找表（画布 shader 末趟——渲染端接入
// 登记后续批；本批交内核与宿主 oracle）。
// 翻译自 photocraft crates/cms/src/lut3d.rs（MIT OR Apache-2.0）。

#include <cstdint>
#include <vector>

#include "io/cms/transform.h"

namespace montage {
namespace cms {

// size³ RGBA 查找表：入口 r + g·size + b·size² 存输入 (r,g,b)/(size−1) 的输出，alpha 1。
// 这是 x 轴为红的 3D 纹理内存序，可直接上传（线性过滤下按 (c·(size−1)+0.5)/size 采样）。
struct Lut3d {
    size_t size = 0;
    std::vector<std::array<float, 4>> data;

    // 采样 3 输入变换（1 输出变换复制为灰）。
    static Lut3d fromTransform(const Transform& t, size_t size);

    // 三线性查找（GPU 过滤的计算方式）。
    std::array<float, 3> sample(const std::array<float, 3>& rgb) const;

    // 小端 RGBA16F 纹素（每个 8 字节）。
    std::vector<uint8_t> toRgba16fBytes() const;

    // RGBA8 纹素（无 float 3D 纹理的后端用）。
    std::vector<uint8_t> toRgba8() const;
};

// IEEE 754 binary16 位型（就近舍入；溢出 → ±inf，NaN 保留）。
uint16_t f32ToF16(float v);
// binary16 位型 → f32。
float f16ToF32(uint16_t h);

}  // namespace cms
}  // namespace montage

#endif  // MONTAGE_IO_CMS_LUT3D_H
