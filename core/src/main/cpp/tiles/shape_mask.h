#ifndef MONTAGE_TILES_SHAPE_MASK_H
#define MONTAGE_TILES_SHAPE_MASK_H

// M11c 形状掩码核（06 规划 §4 M11）：圆角矩形（SDF 距离场）/ 直线（胶囊距离场）。
// 输出文档域 TileGrid（R=G=B=覆盖灰度、A=255），2×2 超采样抗锯齿，与 M10a 多边形核同族。
// 矩形/椭圆/多边形复用既有 rectMask/ellipseMask/polygonMask（selection_ops）。

#include <memory>

#include "engine/document.h"

namespace montage {
namespace tiles {

// 圆角矩形（内切于拖拽对角矩形；radius clamp 到半短边）。r=0 退化为硬边矩形。
std::shared_ptr<const TileGrid> roundedRectMask(double x0, double y0, double x1, double y1,
                                                double radius, uint32_t docW, uint32_t docH);

// 直线（胶囊）：点到线段距离 ≤ width/2 的带（width = 描边宽）。
std::shared_ptr<const TileGrid> capsuleMask(double x0, double y0, double x1, double y1,
                                            double width, uint32_t docW, uint32_t docH);

}  // namespace tiles
}  // namespace montage

#endif  // MONTAGE_TILES_SHAPE_MASK_H
