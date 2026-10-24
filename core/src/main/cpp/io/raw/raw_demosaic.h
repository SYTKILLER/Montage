#ifndef MONTAGE_IO_RAW_RAW_DEMOSAIC_H
#define MONTAGE_IO_RAW_RAW_DEMOSAIC_H

// M23a：Bayer 去马赛克。
// 翻译自 photocraft crates/raw/src/demosaic.rs（MIT OR Apache-2.0）。
// * Bilinear：3×3 窗内同色邻均值。
// * Malvar–He–Cutler：双线性 + 梯度校正项（5×5 核）。
// * AHD（Hirakawa–Parks 2005）：水平/垂直插值候选，按 CIELAB 局部同质性逐像素选。
// * 通用周期 CFA（X-Trans 6×6）：边缘导向插值（demosaicCfa）。
// 输入是 PAD 镜像反射边界的归一化 CFA 平面；phase 给出图像原点 2×2 格的颜色
// （0=R, 1=G, 2=B），行主序。输出交错 RGB。

#include <cstdint>
#include <vector>

#include "io/raw/raw_types.h"

namespace montage {
namespace raw {

// 去马赛克算法。
enum class Demosaic { Bilinear, Mhc, Ahd };

inline const char* demosaicId(Demosaic m) {
    switch (m) {
        case Demosaic::Bilinear: return "bilinear";
        case Demosaic::Mhc: return "mhc";
        default: return "ahd";
    }
}

// Padded 平面的边界宽（偶数，CFA 相位不变）。
constexpr size_t kDemosaicPad = 6;

// 每边带 kDemosaicPad 额外像素的 w×h 平面。
struct Padded {
    std::vector<float> data;
    size_t w = 0, h = 0, stride = 0;

    Padded() = default;
    Padded(size_t width, size_t height) { reset(width, height); }
    void reset(size_t width, size_t height);

    // 镜像反射填充边界。
    void fillBorders();
};

// 从平面原点看到的 CFA 重复：平面像素 (x,y) 的颜色为 colors[(y%h)*w + x%w]。
struct DemosaicPattern {
    size_t w = 0, h = 0;
    std::vector<uint8_t> colors;

    uint8_t at(long long x, long long y) const;
};

// 对带填充的 CFA 平面去马赛克为交错 RGB（w × h × 3）。
// `toXyz`：AHD 的 CIELAB 变换用（相机 → XYZ D50）。
std::vector<float> demosaicBayer(const Padded& p, const uint8_t phase[4], Demosaic method, const Mat3& toXyz);

// 任意周期 CFA 的去马赛克（X-Trans 等）。
std::vector<float> demosaicCfa(const Padded& p, const DemosaicPattern& pat);

}  // namespace raw
}  // namespace montage

#endif  // MONTAGE_IO_RAW_RAW_DEMOSAIC_H
