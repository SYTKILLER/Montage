#ifndef MONTAGE_ENGINE_RESAMPLE_HQ_H
#define MONTAGE_ENGINE_RESAMPLE_HQ_H

// M20a 高品质重采样核（photocraft algo/resample.rs 全量 C++ 翻译，Avidan 之外 PS 25.4 实测
// 相位表/Keys 参数拟合）：Nearest/Bilinear/Bicubic(−0.75)/Bicubic Smoother(1.15 stretch)/
// Sharper(−1)/Automatic(两遍拆分)/Lanczos3/PreserveDetails（放大=Lanczos+USM 30%/0.6·scale，
// 缩小=Bicubic Sharper）。两遍规划（小轴先行、遍间按位深度量化=PS 存储裁剪）、1/128 像素
// 相位表、缩小<1/4 先盒平均、画布边缘重复（放大后贴边层不褪边）。
// 核心在 f32 RGBA（预乘域）上运算；引擎走 TileGrid RGBA8，宿主 oracle 走 16 位模式。

#include "engine/document.h"
#include "engine/inpaint_ops.h"  // RectI

#include <memory>

namespace montage {
using inpaint::RectI;

// 与 photocraft Resample 枚举一致（bridge 数值序同此）
enum class ResampleMethod : int32_t {
    Nearest = 0,
    Bilinear = 1,
    Bicubic = 2,
    BicubicSmoother = 3,
    BicubicSharper = 4,
    BicubicAutomatic = 5,
    Lanczos = 6,
    PreserveDetails = 7,
};

// f32 RGBA 紧凑图像（非预乘存储；核内预乘运算）
struct ImageF {
    std::vector<float> px;  // w*h*4
    int w = 0;
    int h = 0;
};

// 可分离高品质重采样（about 原点；bitDepth: 8=引擎/16=宿主 PS oracle；canvas 可空）。
// canvas：画布矩形（边缘重复语义，见 resize_surface_in_canvas）；空矩形=无。
ImageF resampleImageHQ(const ImageF& src, int dstW, int dstH, ResampleMethod method,
                       int bitDepth, const RectI& canvas);

// 引擎网格入口：RGBA8 TileGrid → HQ 重采样到 dstCols×dstRows（透明出界=默认像素；
// 网格边缘重复）。revision = src.revision+1。
std::shared_ptr<const TileGrid> resampleGridHQ(const TileGrid& src, uint32_t dstCols,
                                               uint32_t dstRows, ResampleMethod method);

// 图像大小（M20a 升级版）：resampleDocument 的 method 化——层/蒙版像素走 HQ 核。
bool resampleDocumentHQ(Document& doc, uint32_t newW, uint32_t newH, ResampleMethod method);

}  // namespace montage

#endif  // MONTAGE_ENGINE_RESAMPLE_HQ_H
