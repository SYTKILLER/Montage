#ifndef MONTAGE_ENGINE_WARP_OPS_H
#define MONTAGE_ENGINE_WARP_OPS_H

// M20 网格/三角纹理渲染（photocraft crates/algo/src/warp.rs C++ 翻译，MIT OR Apache-2.0）。
// 源矩形细分 4px 网格（≤512 格/轴）正向映射顶点，双三角栅格化+重心插补源位，双三次
// 预乘采样；收缩处每像素按逆映射 Jacobian 的网格双线性子采样（每源像素≈1 子样，细线
// 不走样）；折叠时后画覆盖先画。任何正向映射的统一渲染路径（透视/操控/广角/网格变形）。

#include "engine/inpaint_ops.h"  // RectI

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace montage {
namespace warp {

using inpaint::RectI;

struct Img {
    std::vector<float> px;  // w*h*4 RGBA（非预乘存储）
    int w = 0;
    int h = 0;
};

enum class Interp : int32_t { Nearest = 0, Bilinear = 1, Bicubic = 2 };

// 顶点 = (dst, src) 文档像素；tri 顶点序号；tris 顺序=深度序（后者覆盖）。
// 输出锚定 outOrigin（文档域），尺寸 outW/outH 由调用方给（覆盖变形 bbox）。
struct TriRender {
    Img out;
    double originX = 0.0;
    double originY = 0.0;
};

TriRender warpTrianglesImg(const Img& src, const RectI& srcRect,
                           const std::vector<std::pair<std::array<double, 2>, std::array<double, 2>>>& verts,
                           const std::vector<std::array<int, 3>>& tris, Interp interp,
                           double originX, double originY, int outW, int outH);

// 便捷封装：自动求输出 bbox（变形顶点范围）、按 CELL=4px 细分正向映射 f 渲染
//（warp_mesh_surface 直迁）。origin/out 尺寸回传。
TriRender warpMeshImg(const Img& src, const RectI& srcRect,
                      const std::function<std::array<double, 2>(double, double)>& f, Interp interp);

}  // namespace warp

}  // namespace montage

#endif  // MONTAGE_ENGINE_WARP_OPS_H
